/* nitro-daemon — servidor del protocolo NitroSense (35 comandos) para Linux
 *
 * Etapa 2 del port: replica el servidor de named pipe de PSSvc.exe
 * ("predatorsense_service_namedpipe") sobre un socket UNIX, con el mismo
 * framing CMF/RMF documentado en los informes 2, 3 y 6.
 *
 *   CMF petición:  [u16 cmd @0][u8 argc @2] + argc × ([u32 size][size bytes])
 *                  (tamaños SIN alinear a partir del offset 3)
 *   RMF respuesta: [u8 retArgc] + n × ([u32 size][size bytes])
 *                  (el primer argumento de retorno es siempre u32 status)
 *
 * Auth: SO_PEERCRED (uid/pid del cliente) — equivalente a la allowlist
 * por PID/paquete de PSSvc + SDDL "Authenticated Users".
 *
 * Seguridad mejorada respecto a NitroSense (informe 2 §7): los comandos de
 * "registro" (0x01-0x06) están ACOTADOS a SOFTWARE\OEM\NitroSense y se
 * emulan en un archivo propio; no exponen escritura arbitraria.
 *
 * Uso: nitro-daemon [-s socket] [-r registry] [-w sysfs_dir] [-v]
 *   (por defecto: /run/nitro-sense/nitrosense.sock, /var/lib/nitro-sense/registry.conf)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>

#define DEFAULT_SOCK	"/run/nitro-sense/nitrosense.sock"
#define DEFAULT_REG	"/var/lib/nitro-sense/registry.conf"
#define DEFAULT_SYSFS	"/sys/bus/wmi/devices/7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56-6/nitro_wmi"
#define REG_SCOPE	"SOFTWARE/OEM/NitroSense"

#define MAX_MSG		(64 * 1024)
#define MAX_ARGS	16

/* status del protocolo: 0=OK; 2/3 = códigos del firmware (pasantía);
 * >=0x10: errores del daemon */
#define ST_OK		0x00
#define ST_FW_SHIFTED	0x100	/* interno: usar status del firmware */
#define ST_UNSUPPORTED	0x10
#define ST_INTERNAL	0x11
#define ST_PROTOCOL	0x12
#define ST_SCOPE	0x13

struct arg {
	uint32_t size;
	uint8_t *data;
};

struct req {
	uint16_t cmd;
	uint8_t argc;
	struct arg args[MAX_ARGS];
};

static const char *g_sock_path = DEFAULT_SOCK;
static const char *g_reg_path = DEFAULT_REG;
static char g_sysfs[512];
static int g_verbose;
static volatile sig_atomic_t g_stop;

/* Descubre el sysfs del módulo (la instancia WMI -N puede variar) */
static int discover_sysfs(void)
{
	DIR *d = opendir("/sys/bus/wmi/devices");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "7A4DDFE7-", 9) == 0) {
			snprintf(g_sysfs, sizeof(g_sysfs),
				 "/sys/bus/wmi/devices/%s/nitro_wmi", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

/* ---------- utilidades binarias (little-endian) ---------- */

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint16_t get_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get_u32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n > 0) {
		ssize_t r = read(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		p += r;
		n -= r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n > 0) {
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

/* ---------- sysfs del módulo ---------- */

static int sysfs_read(const char *attr, char *buf, size_t bufsz)
{
	char path[600];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s", g_sysfs, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static int sysfs_write(const char *attr, const char *value)
{
	char path[600];
	int fd;
	ssize_t n = (ssize_t)strlen(value);

	snprintf(path, sizeof(path), "%s/%s", g_sysfs, attr);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	if (write_full(fd, value, n) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Llamada WMI cruda a través del escape hatch del módulo (raw_call/raw_result).
 * Devuelve el status del firmware en *fw_status y el valor de salida en *out.
 */
static int wmi_raw(uint32_t method, const uint8_t *data, size_t len,
		   uint32_t *fw_status, uint64_t *out)
{
	char line[300], res[128];
	size_t i, o;

	o = (size_t)snprintf(line, sizeof(line), "%u ", method);
	if (len * 2 > sizeof(line) - o - 1)
		return -1;
	for (i = 0; i < len; i++)
		o += (size_t)snprintf(line + o, sizeof(line) - o, "%02x", data[i]);

	if (sysfs_write("raw_call", line) < 0)
		return -1;
	if (sysfs_read("raw_result", res, sizeof(res)) < 0)
		return -1;
	if (sscanf(res, "status=0x%02x out=0x%016llx",
		   (unsigned *)fw_status, (unsigned long long *)out) != 2)
		return -1;
	return 0;
}

/* ---------- registro emulado (acotado a SOFTWARE/OEM/NitroSense) ---------- */

struct reg_entry {
	char *key;		/* "SOFTWARE/OEM/NitroSense/FanControl/CurrentFanMode" */
	uint32_t type;
	size_t len;
	uint8_t *data;
};

#define MAX_ENTRIES 512
static struct reg_entry g_reg[MAX_ENTRIES];
static int g_reg_count;

/* UTF-16LE (con o sin BOM/terminador) → ASCII; src compacto (unidades de 2 bytes) */
static int utf16le_to_ascii(const uint8_t *src, size_t len, char *dst, size_t dstsz)
{
	size_t i, o = 0;

	for (i = 0; i + 1 < len && o + 1 < dstsz; i += 2) {
		uint16_t u = src[i] | (src[i + 1] << 8);

		if (u == 0)
			break;
		dst[o++] = (u < 128) ? (char)u : '_';
	}
	dst[o] = 0;
	return (int)o;
}

/* normaliza backslashes → '/' y devuelve 0 si está dentro del scope */
static int normalize_path(const char *in, char *out, size_t outsz)
{
	size_t i;

	for (i = 0; in[i] && i < outsz - 1; i++)
		out[i] = (in[i] == '\\') ? '/' : in[i];
	out[i] = 0;
	return strncasecmp(out, REG_SCOPE, strlen(REG_SCOPE)) == 0 &&
	       (out[strlen(REG_SCOPE)] == 0 || out[strlen(REG_SCOPE)] == '/');
}

static void reg_load(void)
{
	FILE *f = fopen(g_reg_path, "r");
	char line[1024];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && g_reg_count < MAX_ENTRIES) {
		char *p = line, *t, *h;
		struct reg_entry *e = &g_reg[g_reg_count];

		while (*p == ' ' || *p == '\t')
			p++;
		/* formato: "clave\ttype\thex" */
		t = strchr(p, '\t');
		if (!t)
			continue;
		*t = 0;
		h = strchr(t + 1, '\t');
		if (!h)
			continue;
		*h = 0;
		e->type = (uint32_t)strtoul(t + 1, NULL, 10);
		h++;
		e->len = strlen(h) / 2;
		if (e->len == 0)
			continue;
		e->data = malloc(e->len);
		if (!e->data)
			continue;
		for (size_t j = 0; j < e->len; j++) {
			unsigned b;

			if (sscanf(h + j * 2, "%2x", &b) != 1) {
				free(e->data);
				e->data = NULL;
				break;
			}
			e->data[j] = (uint8_t)b;
		}
		if (!e->data)
			continue;
		while (*p == '\n' || *p == ' ')
			p++;
		e->key = strdup(p);
		if (!e->key) {
			free(e->data);
			continue;
		}
		/* quitar \n final de la clave (hex ya la consumió) */
		e->key[strcspn(e->key, "\n")] = 0;
		g_reg_count++;
	}
	fclose(f);
}

static void reg_save(void)
{
	FILE *f = fopen(g_reg_path, "w");

	if (!f)
		return;
	for (int i = 0; i < g_reg_count; i++) {
		struct reg_entry *e = &g_reg[i];

		fprintf(f, "%s\t%u\t", e->key, e->type);
		for (size_t j = 0; j < e->len; j++)
			fprintf(f, "%02x", e->data[j]);
		fprintf(f, "\n");
	}
	fclose(f);
}

static int reg_find(const char *key)
{
	for (int i = 0; i < g_reg_count; i++)
		if (strcasecmp(g_reg[i].key, key) == 0)
			return i;
	return -1;
}

static void reg_set(const char *key, uint32_t type, const uint8_t *data, size_t len)
{
	int i = reg_find(key);
	struct reg_entry *e;

	if (i >= 0) {
		e = &g_reg[i];
		free(e->data);
	} else {
		if (g_reg_count >= MAX_ENTRIES)
			return;
		e = &g_reg[g_reg_count++];
		e->key = strdup(key);
	}
	e->type = type;
	e->len = len;
	e->data = malloc(len ? len : 1);
	memcpy(e->data, data, len);
}

static void reg_del_value(const char *key)
{
	int i = reg_find(key);

	if (i < 0)
		return;
	free(g_reg[i].key);
	free(g_reg[i].data);
	memmove(&g_reg[i], &g_reg[i + 1],
		(g_reg_count - i - 1) * sizeof(g_reg[0]));
	g_reg_count--;
}

static void reg_del_tree(const char *prefix)
{
	size_t n = strlen(prefix);

	for (int i = 0; i < g_reg_count; ) {
		if (strncasecmp(g_reg[i].key, prefix, n) == 0 &&
		    (g_reg[i].key[n] == 0 || g_reg[i].key[n] == '/')) {
			free(g_reg[i].key);
			free(g_reg[i].data);
			memmove(&g_reg[i], &g_reg[i + 1],
				(g_reg_count - i - 1) * sizeof(g_reg[0]));
			g_reg_count--;
		} else {
			i++;
		}
	}
}

/* ---------- construcción de respuesta (RMF) ---------- */

struct resp {
	uint8_t *buf;
	size_t len, cap;
	uint8_t nargs;
};

static int resp_grow(struct resp *r, size_t need)
{
	if (r->len + need <= r->cap)
		return 0;
	r->cap = (r->len + need) * 2 + 64;
	r->buf = realloc(r->buf, r->cap);
	return r->buf ? 0 : -1;
}

static void resp_add(struct resp *r, const void *data, uint32_t size)
{
	if (resp_grow(r, 4 + size) < 0)
		return;
	put_u32(r->buf + r->len, size);
	r->len += 4;
	memcpy(r->buf + r->len, data, size);
	r->len += size;
	r->nargs++;
}

static void resp_u32(struct resp *r, uint32_t v) { resp_add(r, &v, 4); }
static void resp_u64(struct resp *r, uint64_t v) { resp_add(r, &v, 8); }

static void resp_status(struct resp *r, uint32_t st)
{
	r->nargs = 0;
	r->len = 0;
	resp_u32(r, st);	/* primer argumento = status, siempre */
}

/* ---------- parseo de petición (CMF) ---------- */

static int req_parse(int fd, struct req *q)
{
	uint8_t hdr[3];

	if (read_full(fd, hdr, 3) < 0)
		return -1;
	q->cmd = get_u16(hdr);
	q->argc = hdr[2];
	if (q->argc > MAX_ARGS)
		return -1;
	for (int i = 0; i < q->argc; i++) {
		uint8_t szb[4];
		uint32_t sz;

		if (read_full(fd, szb, 4) < 0)
			return -1;
		sz = get_u32(szb);
		if (sz > MAX_MSG)
			return -1;
		q->args[i].size = sz;
		q->args[i].data = malloc(sz ? sz : 1);
		if (!q->args[i].data)
			return -1;
		if (sz && read_full(fd, q->args[i].data, sz) < 0)
			return -1;
	}
	return 0;
}

static void req_free(struct req *q)
{
	for (int i = 0; i < q->argc; i++)
		free(q->args[i].data);
	memset(q, 0, sizeof(*q));
}

/* ---------- helpers de argumentos ---------- */

static int arg_is_u64(const struct req *q, int i, uint64_t *out)
{
	if (i >= q->argc || q->args[i].size != 8)
		return 0;
	memcpy(out, q->args[i].data, 8);
	return 1;
}

static int arg_is_u32(const struct req *q, int i, uint32_t *out)
{
	if (i >= q->argc || q->args[i].size != 4)
		return 0;
	*out = get_u32(q->args[i].data);
	return 1;
}

/* string UTF-16LE de TsDotNetLib → ascii */
static int arg_str(const struct req *q, int i, char *out, size_t outsz)
{
	if (i >= q->argc)
		return 0;
	return utf16le_to_ascii(q->args[i].data, q->args[i].size, out, outsz) >= 0;
}

/* ---------- despachador de los 35 comandos ---------- */

static void handle_wmi_passthrough(struct req *q, struct resp *r,
				   uint32_t method, int with_out)
{
	uint64_t out = 0;
	uint32_t st = 0;

	if (q->argc < 1) {
		resp_status(r, ST_PROTOCOL);
		return;
	}
	/* entrada: el buffer tal cual (u64 de 8 bytes, u32 de 4, 16 bytes…) */
	if (wmi_raw(method, q->args[0].data, q->args[0].size, &st, &out) < 0) {
		resp_status(r, ST_INTERNAL);
		return;
	}
	resp_status(r, st);
	if (with_out)
		resp_u64(r, out);
}

static uint32_t gpu_count(void)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	uint32_t n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char path[600], cls[32];
		int fd;

		snprintf(path, sizeof(path),
			 "/sys/bus/pci/devices/%s/class", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (read(fd, cls, sizeof(cls) - 1) > 0) {
			cls[sizeof(cls) - 1] = 0;
			/* clase 0x03xxxx = display controller */
			if (strncmp(cls, "0x03", 4) == 0)
				n++;
		}
		close(fd);
	}
	closedir(d);
	return n;
}

static void dispatch(struct req *q, struct resp *r)
{
	uint64_t in64 = 0;
	uint32_t in32 = 0, out_dummy = 0;
	char path[512], name[256];
	uint64_t wout;

	switch (q->cmd) {
	case 0x00:	/* echo */
		resp_status(r, ST_OK);
		for (int i = 0; i < q->argc; i++)
			resp_add(r, q->args[i].data, q->args[i].size);
		break;

	/* ---- registro emulado (acotado) ---- */
	case 0x01:	/* crear clave */
		if (!arg_str(q, 0, path, sizeof(path)) ||
		    !normalize_path(path, path, sizeof(path))) {
			resp_status(r, ST_SCOPE);
			break;
		}
		resp_status(r, ST_OK);	/* kv: crear clave = no-op */
		break;
	case 0x02:	/* borrar árbol */
		if (!arg_str(q, 0, path, sizeof(path)) ||
		    !normalize_path(path, path, sizeof(path))) {
			resp_status(r, ST_SCOPE);
			break;
		}
		reg_del_tree(path);
		reg_save();
		resp_status(r, ST_OK);
		break;
	case 0x03:
	case 0x04:	/* borrar valor */
		if (!arg_str(q, 0, path, sizeof(path)) ||
		    !arg_str(q, 1, name, sizeof(name)) ||
		    !normalize_path(path, path, sizeof(path))) {
			resp_status(r, ST_SCOPE);
			break;
		}
		strncat(path, "/", sizeof(path) - strlen(path) - 1);
		strncat(path, name, sizeof(path) - strlen(path) - 1);
		reg_del_value(path);
		reg_save();
		resp_status(r, ST_OK);
		break;
	case 0x05:	/* leer (extensión) o comprobar+escribir */
	case 0x06:	/* escribir valor */
		if (!arg_str(q, 0, path, sizeof(path)) ||
		    !arg_str(q, 1, name, sizeof(name)) ||
		    !normalize_path(path, path, sizeof(path))) {
			resp_status(r, ST_SCOPE);
			break;
		}
		strncat(path, "/", sizeof(path) - strlen(path) - 1);
		strncat(path, name, sizeof(path) - strlen(path) - 1);
		if (q->cmd == 0x05 && q->argc == 2) {
			/* lectura: [status][exists u32][data si existía] */
			int i = reg_find(path);

			resp_status(r, ST_OK);
			if (i >= 0) {
				resp_u32(r, 1);
				resp_add(r, g_reg[i].data,
					 (uint32_t)g_reg[i].len);
			} else {
				resp_u32(r, 0);
			}
			break;
		}
		/* escritura: args[2]=type u32, args[3]=data */
		if (!arg_is_u32(q, 2, &in32) || q->argc < 4) {
			resp_status(r, ST_PROTOCOL);
			break;
		}
		reg_set(path, in32, q->args[3].data, q->args[3].size);
		reg_save();
		resp_status(r, ST_OK);
		break;

	/* ---- no soportados por diseño (seguridad / sin equivalente) ---- */
	case 0x07:	/* CreateProcessAsUser — rechazado: no exponer spawn */
	case 0x08:	/* CreateProcessOnLogon */
	case 0x0E:	/* GPU usage loading (NVML) */
	case 0x11:	/* APGeAction.SetFunction (GUID distinto) */
	case 0x14:	/* APGeAction.GetFunction */
	case 0x15:	/* NV OC get */
	case 0x16:	/* Intel OC get (XTU no existe en Linux) */
	case 0x17:	/* NV OC set */
	case 0x18:	/* Intel OC set */
	case 0x19:	/* NV SLI set */
	case 0x1A:	/* NV coproc status */
	case 0x20:	/* GPU frequency */
		resp_status(r, ST_UNSUPPORTED);
		break;

	/* ---- pasarela WMI (AcerGamingFunction) ---- */
	case 0x09: handle_wmi_passthrough(q, r, 1, 0); break;  /* SetGamingProfile */
	case 0x0A: handle_wmi_passthrough(q, r, 3, 1); break;  /* GetGamingProfile */
	case 0x0B: handle_wmi_passthrough(q, r, 12, 0); break; /* SetGamingLEDColor */
	case 0x0C: handle_wmi_passthrough(q, r, 13, 1); break; /* GetGamingLEDColor */
	case 0x0D: handle_wmi_passthrough(q, r, 5, 1); break;  /* GetGamingSysInfo */
	case 0x0F: handle_wmi_passthrough(q, r, 14, 0); break; /* SetGamingFanBehavior */
	case 0x10: handle_wmi_passthrough(q, r, 16, 0); break; /* SetGamingFanSpeed */
	case 0x12: handle_wmi_passthrough(q, r, 2, 0); break;  /* Logo LED */
	case 0x13: handle_wmi_passthrough(q, r, 2, 0); break;  /* Logo behavior */
	case 0x1B: handle_wmi_passthrough(q, r, 20, 0); break;  /* KB backlight (16B) */
	case 0x1C: handle_wmi_passthrough(q, r, 6, 0); break;  /* RGB KB */
	case 0x1D: handle_wmi_passthrough(q, r, 10, 0); break;  /* LED behavior */
	case 0x21: handle_wmi_passthrough(q, r, 22, 0); break; /* SetGamingMiscSetting */
	case 0x22: handle_wmi_passthrough(q, r, 23, 1); break; /* GetGamingMiscSetting */

	case 0x1E: {	/* set_operation_mode — adaptado a esta BIOS */
		uint64_t in;
		uint32_t fw = 0;
		char val[8];

		if (!arg_is_u32(q, 0, &in32)) {
			if (!arg_is_u64(q, 0, &in64)) {
				resp_status(r, ST_PROTOCOL);
				break;
			}
			in32 = (uint32_t)in64;
		}
		/* estilo BIOS nuevas: (mode<<8)|0x0B vía SetGamingMiscSetting */
		in = ((uint64_t)in32 << 8) | 0x0B;
		if (wmi_raw(22, (uint8_t *)&in, 8, &fw, &wout) == 0 && fw == 0) {
			resp_status(r, fw);
			break;
		}
		/* fallback BIOS 2019 (AN515-54): TKST turbo on/off */
		snprintf(val, sizeof(val), "%u", in32 ? 1u : 0u);
		if (sysfs_write("turbo_mode", val) < 0) {
			resp_status(r, ST_INTERNAL);
			break;
		}
		resp_status(r, ST_OK);
		break;
	}
	case 0x1F:	/* GPU count */
		resp_status(r, ST_OK);
		resp_u32(r, gpu_count());
		break;

	default:
		resp_status(r, ST_UNSUPPORTED);
		break;
	}
	(void)out_dummy;
}

/* ---------- conexión ---------- */

static void log_peer(int fd)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	char comm[64] = "?";
	int cfd;

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return;
	cfd = open("/proc/self/fd/1", O_WRONLY); /* noop, evita warning */
	if (cfd >= 0)
		close(cfd);
	snprintf(comm, sizeof(comm), "/proc/%d/comm", cred.pid);
	cfd = open(comm, O_RDONLY);
	if (cfd >= 0) {
		ssize_t n = read(cfd, comm, sizeof(comm) - 1);

		close(cfd);
		if (n > 0) {
			comm[n] = 0;
			comm[strcspn(comm, "\n")] = 0;
		} else {
			strcpy(comm, "?");
		}
	}
	fprintf(stderr, "[conn] uid=%u pid=%u comm=%s fd=%d\n",
		cred.uid, cred.pid, comm, fd);
}

static void handle_conn(int fd)
{
	struct req q;
	struct resp r = { 0 };

	while (!g_stop) {
		if (req_parse(fd, &q) < 0)
			break;
		if (g_verbose)
			fprintf(stderr, "[req] cmd=0x%02x argc=%u\n",
				q.cmd, q.argc);
		memset(&r, 0, sizeof(r));
		r.buf = NULL;
		dispatch(&q, &r);
		/* RMF: [u8 retArgc][args] */
		{
			uint8_t hdr = r.nargs;
			uint8_t *full = malloc(1 + r.len);

			if (!full)
				break;
			full[0] = hdr;
			memcpy(full + 1, r.buf, r.len);
			if (write_full(fd, full, 1 + r.len) < 0) {
				free(full);
				free(r.buf);
				req_free(&q);
				break;
			}
			free(full);
		}
		free(r.buf);
		req_free(&q);
	}
	close(fd);
}

/* ---------- main ---------- */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr = { 0 };
	struct stat st;
	int sock, c;

	while ((c = getopt(argc, argv, "s:r:w:vh")) != -1) {
		switch (c) {
		case 's': g_sock_path = optarg; break;
		case 'r': g_reg_path = optarg; break;
		case 'w': snprintf(g_sysfs, sizeof(g_sysfs), "%s", optarg); break;
		case 'v': g_verbose = 1; break;
		default:
			fprintf(stderr,
				"uso: %s [-s socket] [-r registro] [-w sysfs_dir] [-v]\n",
				argv[0]);
			return 1;
		}
	}
	if (!g_sysfs[0]) {
		if (discover_sysfs() < 0)
			snprintf(g_sysfs, sizeof(g_sysfs), "%s", DEFAULT_SYSFS);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	/* directorio del socket */
	{
		char dir[512];
		char *slash;

		snprintf(dir, sizeof(dir), "%s", g_sock_path);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = 0;
			mkdir(dir, 0755);
		}
	}
	/* directorio del registro */
	{
		char dir[512];
		char *slash;

		snprintf(dir, sizeof(dir), "%s", g_reg_path);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = 0;
			mkdir(dir, 0755);
		}
	}
	if (stat(g_sysfs, &st) != 0) {
		fprintf(stderr, "aviso: %s no existe (¿módulo nitro_wmi cargado?)\n",
			g_sysfs);
	}
	reg_load();

	unlink(g_sock_path);
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return 1;
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}
	chmod(g_sock_path, 0666);	/* como SDDL "Authenticated Users" */
	if (listen(sock, 8) < 0) {
		perror("listen");
		return 1;
	}
	fprintf(stderr, "nitro-daemon: escuchando en %s\n", g_sock_path);
	fprintf(stderr, "  sysfs: %s\n  registro: %s\n", g_sysfs, g_reg_path);

	while (!g_stop) {
		int cfd = accept(sock, NULL, NULL);

		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			continue;
		}
		log_peer(cfd);
		handle_conn(cfd);
	}
	close(sock);
	unlink(g_sock_path);
	reg_save();
	fprintf(stderr, "nitro-daemon: salida limpia\n");
	return 0;
}
