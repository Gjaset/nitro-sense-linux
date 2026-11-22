/* nsclient — cliente de prueba del protocolo NitroSense (CMF/RMF) para nitro-daemon
 *
 * uso:
 *   nsclient <socket> <cmd_hex> [args...]
 *     args: u32:VAL  u64:VAL  s:TEXTO(→UTF-16LE)  h:HEXBYTES
 *   nsclient <socket> selftest      [battery completa]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_MSG (64 * 1024)

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get_u32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = read(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!r)
			return -1;
		p += r;
		n -= r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t r = write(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= r;
	}
	return 0;
}

/* construye un argumento según el prefijo */
static int build_arg(const char *spec, uint8_t **out, uint32_t *outlen)
{
	if (strncmp(spec, "u32:", 4) == 0) {
		uint32_t v = (uint32_t)strtoul(spec + 4, NULL, 0);

		*out = malloc(4);
		put_u32(*out, v);
		*outlen = 4;
		return 0;
	}
	if (strncmp(spec, "u64:", 4) == 0) {
		uint64_t v = strtoull(spec + 4, NULL, 0);

		*out = malloc(8);
		memcpy(*out, &v, 8);
		*outlen = 8;
		return 0;
	}
	if (strncmp(spec, "s:", 2) == 0) {
		const char *txt = spec + 2;
		size_t len = strlen(txt);

		/* UTF-16LE + terminador (como TsDotNetLib: Length*2 + 2) */
		*out = calloc(len + 1, 2);
		for (size_t i = 0; i < len; i++)
			(*out)[i * 2] = (uint8_t)txt[i];
		*outlen = (uint32_t)((len + 1) * 2);
		return 0;
	}
	if (strncmp(spec, "h:", 2) == 0) {
		const char *h = spec + 2;
		size_t hl = strlen(h);

		if (hl % 2)
			return -1;
		*out = malloc(hl / 2);
		for (size_t i = 0; i < hl / 2; i++) {
			unsigned b;

			if (sscanf(h + i * 2, "%2x", &b) != 1) {
				free(*out);
				return -1;
			}
			(*out)[i] = (uint8_t)b;
		}
		*outlen = (uint32_t)hl / 2;
		return 0;
	}
	return -1;
}

/* envía una orden y devuelve la respuesta parseada */
static int call(const char *sockpath, uint16_t cmd, int argc, char **argspecs,
		uint8_t *resp, size_t *resplen)
{
	struct sockaddr_un addr = { 0 };
	uint8_t msg[MAX_MSG];
	size_t len = 0, o;
	int fd, i;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "connect(%s): %s\n", sockpath, strerror(errno));
		close(fd);
		return -1;
	}

	/* CMF: [u16 cmd][u8 argc][args] */
	put_u16(msg, cmd);
	msg[2] = (uint8_t)argc;
	len = 3;
	for (i = 0; i < argc; i++) {
		uint8_t *data;
		uint32_t dlen;

		if (build_arg(argspecs[i], &data, &dlen) < 0) {
			fprintf(stderr, "arg inválida: %s\n", argspecs[i]);
			close(fd);
			return -1;
		}
		if (len + 4 + dlen > MAX_MSG) {
			free(data);
			close(fd);
			return -1;
		}
		put_u32(msg + len, dlen);
		len += 4;
		memcpy(msg + len, data, dlen);
		len += dlen;
		free(data);
	}
	if (write_full(fd, msg, len) < 0) {
		perror("write");
		close(fd);
		return -1;
	}

	/* RMF: [u8 retArgc][args] */
	o = 0;
	if (read_full(fd, resp, 1) < 0) {
		close(fd);
		return -1;
	}
	uint8_t nargs = resp[0];
	o = 1;
	for (i = 0; i < nargs; i++) {
		uint8_t szb[4];
		uint32_t sz;

		if (read_full(fd, szb, 4) < 0) {
			close(fd);
			return -1;
		}
		sz = get_u32(szb);
		if (o + 4 + sz > MAX_MSG) {
			close(fd);
			return -1;
		}
		memcpy(resp + o, szb, 4);
		o += 4;
		if (sz && read_full(fd, resp + o, sz) < 0) {
			close(fd);
			return -1;
		}
		o += sz;
	}
	close(fd);
	*resplen = o;
	return 0;
}

static uint32_t resp_status_of(const uint8_t *resp)
{
	/* RMF: [u8 nargs][u32 size(=4)][u32 status]… */
	return resp[0] ? get_u32(resp + 5) : 0xFFFF;
}

static void print_resp(const char *label, uint8_t *resp, size_t len)
{
	uint8_t nargs = resp[0];
	size_t o = 1;

	(void)len;
	printf("%-38s → status=0x%x", label, resp_status_of(resp));
	for (int i = 0; i < nargs; i++) {
		uint32_t sz = get_u32(resp + o);

		o += 4;
		if (i == 0) {
			o += sz;
			continue;
		}
		printf(" arg%d[%u]=", i, sz);
		for (uint32_t j = 0; j < sz && j < 16; j++)
			printf("%02x", resp[o + j]);
		if (sz > 16)
			printf("…");
		o += sz;
	}
	printf("\n");
}

/* ejecuta una orden y devuelve el status (0xFFFF si error de transporte) */
static uint32_t run(const char *sock, uint16_t cmd, int argc, char **specs,
		   uint8_t *resp, size_t *rlen, const char *label)
{
	if (call(sock, cmd, argc, specs, resp, rlen) < 0) {
		printf("%-38s → TRANSPORTE/ERROR\n", label);
		return 0xFFFF;
	}
	print_resp(label, resp, *rlen);
	return resp_status_of(resp);
}

static int selftest(const char *sock)
{
	uint8_t resp[MAX_MSG];
	size_t rlen;
	int pass = 0, total = 0;
	uint32_t st;

	/* 1. echo */
	{
		char *specs[] = { "s:hola", "u32:1234" };

		st = run(sock, 0x00, 2, specs, resp, &rlen, "echo");
		total++; pass += (st == 0);
	}
	/* 2. registro: crear, escribir DWORD, leer, borrar */
	{
		char *mk[] = { "s:SOFTWARE\\OEM\\NitroSense\\FanControl" };
		char *wr[] = { "s:SOFTWARE\\OEM\\NitroSense\\FanControl",
			       "s:CurrentFanMode", "u32:4", "h:03000000" };
		char *rd[] = { "s:SOFTWARE\\OEM\\NitroSense\\FanControl",
			       "s:CurrentFanMode" };
		char *del[] = { "s:SOFTWARE\\OEM\\NitroSense\\FanControl" };
		char *bad[] = { "s:SOFTWARE\\Acer\\Otra", "s:x" };

		st = run(sock, 0x01, 1, mk, resp, &rlen, "reg-create");
		total++; pass += (st == 0);
		st = run(sock, 0x06, 4, wr, resp, &rlen, "reg-write-dword");
		total++; pass += (st == 0);
		st = run(sock, 0x05, 2, rd, resp, &rlen, "reg-read-dword");
		total++; pass += (st == 0);
		st = run(sock, 0x02, 1, del, resp, &rlen, "reg-del-tree");
		total++; pass += (st == 0);
		st = run(sock, 0x01, 1, bad, resp, &rlen, "reg-scope-reject");
		total++; pass += (st == 0x13);
	}
	/* 3. no soportado */
	{
		char *x[] = { "u32:0" };

		st = run(sock, 0x18, 1, x, resp, &rlen, "unsupported-cmd(0x18)");
		total++; pass += (st == 0x10);
	}
	/* 4. lecturas WMI: temp CPU (sensor 1), RPM CPU (2), RPM GPU (6)
	 * input u32 LE = [byte0=1 grupo][byte1=sensor]: 0x0101/0x0201/0x0601 */
	{
		char *t[] = { "u32:0x101" };
		char *c[] = { "u32:0x201" };
		char *g[] = { "u32:0x601" };

		st = run(sock, 0x0D, 1, t, resp, &rlen, "sysinfo-temp(sensor 1)");
		total++; pass += (st == 0);
		st = run(sock, 0x0D, 1, c, resp, &rlen, "sysinfo-cpu-rpm(sensor 2)");
		total++; pass += (st == 0);
		st = run(sock, 0x0D, 1, g, resp, &rlen, "sysinfo-gpu-rpm(sensor 6)");
		total++; pass += (st == 0);
	}
	/* 5. fan behavior auto (0x410009) — como NitroSense CurrentFanMode=0 */
	{
		char *b[] = { "u64:0x410009" };

		st = run(sock, 0x0F, 1, b, resp, &rlen, "fan-behavior-auto");
		total++; pass += (st == 0);
	}
	/* 6. modo de operación 0 (auto) */
	{
		char *m[] = { "u32:0" };

		st = run(sock, 0x1E, 1, m, resp, &rlen, "op-mode-set(0)");
		total++; pass += (st == 0);
	}
	/* 7. GPU count */
	{
		st = run(sock, 0x1F, 0, NULL, resp, &rlen, "gpu-count");
		total++; pass += (st == 0);
	}

	printf("\nSELFTEST: %d/%d OK\n", pass, total);
	return pass == total ? 0 : 1;
}

int main(int argc, char **argv)
{
	uint8_t resp[MAX_MSG];
	size_t rlen;

	if (argc < 3) {
		fprintf(stderr,
			"uso: %s <socket> <cmd_hex> [args...]\n"
			"     %s <socket> selftest\n"
			"args: u32:VAL  u64:VAL  s:TEXTO  h:HEXBYTES\n",
			argv[0], argv[0]);
		return 2;
	}
	const char *sock = argv[1];

	if (strcmp(argv[2], "selftest") == 0)
		return selftest(sock);

	{
		uint16_t cmd = (uint16_t)strtoul(argv[2], NULL, 16);
		int nargs = argc - 3;
		char **specs = nargs ? &argv[3] : NULL;

		if (call(sock, cmd, nargs, specs, resp, &rlen) < 0)
			return 1;
		print_resp(argv[2], resp, rlen);
	}
	return 0;
}
