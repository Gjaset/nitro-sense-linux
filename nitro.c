/* nitro — CLI del port de NitroSense para Linux (Acer Nitro/Predator)
 *
 * Interfaz de usuario para el módulo nitro-wmi.
 * Compilar: make cli
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define SYS_WMI_BUS "/sys/bus/wmi/devices"
#define NITRO_GUID_PREFIX "7A4DDFE7-"
#define NITRO_DIR "nitro_wmi"

static char sysdir[512];

static int find_sysdir(void)
{
	DIR *d = opendir(SYS_WMI_BUS);
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, NITRO_GUID_PREFIX,
			    strlen(NITRO_GUID_PREFIX)) == 0) {
			struct stat st;

			snprintf(sysdir, sizeof(sysdir), "%s/%s/%s",
				 SYS_WMI_BUS, e->d_name, NITRO_DIR);
			closedir(d);
			if (stat(sysdir, &st) == 0)
				return 0;	/* módulo cargado */
			return -1;
		}
	}
	closedir(d);
	return -1;
}

/* -1: no existe; >=0: contenido */
static int read_attr(const char *name, char *buf, size_t bufsz)
{
	char path[600];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", sysdir, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, bufsz - 1, f);
	fclose(f);
	if (n == 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static int write_attr(const char *name, const char *value)
{
	char path[600];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", sysdir, name);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "error: no se puede abrir %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	fprintf(f, "%s", value);
	fclose(f);
	return 0;
}

static int read_attr_u32(const char *name, int *out)
{
	char buf[64];

	if (read_attr(name, buf, sizeof(buf)) < 0)
		return -1;
	*out = atoi(buf);
	return 0;
}

static void warn_low_fan(int pct)
{
	if (pct >= 0 && pct < 20)
		fprintf(stderr,
			"AVISO: %d%% es muy bajo; el EC puede ignorarlo o el equipo puede calentarse.\n",
			pct);
}

static int cmd_status(void)
{
	int v;
	char res[128];

	printf("nitro-wmi — %s\n", sysdir);
	if (read_attr_u32("cpu_temp", &v) == 0)
		printf("  Temp CPU      : %d °C\n", v);
	if (read_attr_u32("cpu_fan_rpm", &v) == 0)
		printf("  RPM fan CPU   : %d\n", v);
	if (read_attr_u32("gpu_fan_rpm", &v) == 0)
		printf("  RPM fan GPU   : %d\n", v);
	if (read_attr_u32("fan_cpu_percentage", &v) == 0)
		printf("  Fan CPU %%     : %d\n", v);
	if (read_attr_u32("fan_gpu_percentage", &v) == 0)
		printf("  Fan GPU %%     : %d\n", v);
	if (read_attr_u32("turbo_mode", &v) == 0)
		printf("  Modo turbo    : %s\n", v ? "ON" : "OFF");
	{
		char m[32];
		if (read_attr("fan_mode", m, sizeof(m)) >= 0) {
			int mi = atoi(m);
			if (m[0] == 'm')
				printf("  Modo fans     : %s", m);
			else
				printf("  Modo fans     : %s\n",
				       mi == 1 ? "turbo" : mi == 2 ? "custom" : "auto");
		}
	}
	if (read_attr_u32("fan_table", &v) == 0)
		printf("  Tabla fan     : %d (1=Normal 2=Fast 3=Turbo)\n", v);
	if (read_attr_u32("gpu_oc_level", &v) == 0)
		printf("  Nivel OC GPU  : %d\n", v);
	if (read_attr("raw_result", res, sizeof(res)) >= 0)
		printf("  Último raw    : %s", res);
	return 0;
}

static int cmd_fan(const char *which, const char *val)
{
	char attr[64];
	int pct;
	char buf[16];

	if (strcmp(which, "cpu") == 0)
		snprintf(attr, sizeof(attr), "fan_cpu_percentage");
	else if (strcmp(which, "gpu") == 0)
		snprintf(attr, sizeof(attr), "fan_gpu_percentage");
	else {
		fprintf(stderr, "uso: nitro fan cpu|gpu [0-100|auto]\n");
		return 1;
	}

	if (!val) {			/* get */
		if (read_attr_u32(attr, &pct) < 0) {
			fprintf(stderr, "error de lectura\n");
			return 1;
		}
		printf("%s: %d%%\n", which, pct);
		return 0;
	}

	if (strcmp(val, "auto") == 0) {
		/* Auto = SetGamingFanBehavior(0x410009): el EC retoma el control */
		printf("auto: poniendo fans en modo automático (behavior 0x410009)…\n");
		return write_attr("fan_mode", "0");
	}

	pct = atoi(val);
	if (pct < 0 || pct > 100) {
		fprintf(stderr, "rango inválido (0-100)\n");
		return 1;
	}
	warn_low_fan(pct);
	snprintf(buf, sizeof(buf), "%d", pct);
	return write_attr(attr, buf);
}

static int cmd_turbo(const char *val)
{
	int cur;

	if (!val) {
		if (read_attr_u32("turbo_mode", &cur) < 0) {
			fprintf(stderr, "error de lectura\n");
			return 1;
		}
		printf("turbo: %s\n", cur ? "ON" : "OFF");
		return 0;
	}
	if (strcmp(val, "on") == 0)
		return write_attr("turbo_mode", "1");
	if (strcmp(val, "off") == 0)
		return write_attr("turbo_mode", "0");
	fprintf(stderr, "uso: nitro turbo on|off\n");
	return 1;
}

static int cmd_table(const char *val)
{
	if (!val) {
		int v;

		if (read_attr_u32("fan_table", &v) < 0) {
			fprintf(stderr, "error de lectura\n");
			return 1;
		}
		printf("tabla: %d (1=Normal 2=Fast 3=Turbo)\n", v);
		return 0;
	}
	if (val[0] < '1' || val[0] > '3' || val[1]) {
		fprintf(stderr, "uso: nitro table 1|2|3\n");
		return 1;
	}
	return write_attr("fan_table", val);
}

static int cmd_gpuoc(const char *val)
{
	if (!val) {
		int v;

		if (read_attr_u32("gpu_oc_level", &v) < 0) {
			fprintf(stderr, "error de lectura\n");
			return 1;
		}
		printf("gpu oc: %d\n", v);
		return 0;
	}
	if (val[0] < '0' || val[0] > '2' || val[1]) {
		fprintf(stderr, "uso: nitro gpuoc 0|1|2\n");
		return 1;
	}
	return write_attr("gpu_oc_level", val);
}

static int cmd_monitor(int interval)
{
	if (interval < 1)
		interval = 2;
	printf("Ctrl-C para salir\n");
	while (1) {
		int t = -1, rc = -1, rg = -1, fc = -1, fg = -1;

		read_attr_u32("cpu_temp", &t);
		read_attr_u32("cpu_fan_rpm", &rc);
		read_attr_u32("gpu_fan_rpm", &rg);
		read_attr_u32("fan_cpu_percentage", &fc);
		read_attr_u32("fan_gpu_percentage", &fg);
		printf("CPU %2d°C | fan CPU %4d RPM (%3d%%) | fan GPU %4d RPM (%3d%%)\r",
		       t, rc, fc, rg, fg);
		fflush(stdout);
		sleep(interval);
	}
	return 0;
}

static int cmd_raw(const char *m, const char *hex)
{
	char buf[64], res[128];

	if (!m || !hex) {
		fprintf(stderr, "uso: nitro raw <method_id> <hex_u64>\n");
		return 1;
	}
	/* validar hex */
	for (const char *p = hex; *p; p++)
		if (!isxdigit((unsigned char)*p)) {
			fprintf(stderr, "entrada no hex\n");
			return 1;
		}
	snprintf(buf, sizeof(buf), "%u %s", (unsigned)atoi(m), hex);
	if (write_attr("raw_call", buf))
		return 1;
	if (read_attr("raw_result", res, sizeof(res)) < 0) {
		fprintf(stderr, "sin resultado\n");
		return 1;
	}
	printf("%s", res);
	return 0;
}

static int cmd_mode(const char *val)
{
	static const char *names[] = { "auto", "turbo", "custom" };
	int i;

	if (!val) {
		char m[32];

		if (read_attr("fan_mode", m, sizeof(m)) < 0) {
			fprintf(stderr, "sin dato (módulo recién cargado)\n");
			return 1;
		}
		if (m[0] == 'm') {	/* "mixed(x/y)" */
			printf("modo: %s", m);
			return 0;
		}
		i = atoi(m);
		if (i < 0 || i > 2) {
			fprintf(stderr, "valor inesperado: %s\n", m);
			return 1;
		}
		printf("modo: %s\n", names[i]);
		return 0;
	}
	for (i = 0; i < 3; i++)
		if (strcmp(val, names[i]) == 0) {
			char b[8];

			snprintf(b, sizeof(b), "%d", i);
			if (i == 2)
				printf("custom: fijar también nitro fan cpu|gpu <%%> "
				       "(el %% se aplica como curva fija)\n");
			return write_attr("fan_mode", b);
		}
	fprintf(stderr, "uso: nitro mode auto|turbo|custom\n");
	return 1;
}

static void usage(void)
{
	puts("nitro — port de NitroSense para Linux (módulo nitro-wmi)\n"
	     "\n"
	     "  nitro status                 estado completo\n"
	     "  nitro fan cpu|gpu [0-100]    leer o fijar velocidad de fan (%%)\n"
	     "  nitro fan cpu|gpu auto       restaurar control automático del EC\n"
	     "  nitro mode [auto|turbo|custom]  modo conjunto de fans (CoolBoost=turbo)\n"
	     "  nitro rpm                    RPMs de fans\n"
	     "  nitro temp                   temperatura CPU\n"
	     "  nitro turbo [on|off]         modo turbo/rendimiento (TKST)\n"
	     "  nitro table [1|2|3]          tabla de curva de fan (1=Normal 2=Fast 3=Turbo)\n"
	     "  nitro gpuoc [0|1|2]          nivel de overclock GPU\n"
	     "  nitro monitor [seg]          monitor en vivo (2s por defecto)\n"
	     "  nitro raw <method> <hex>     llamada WMI cruda (experimentos)\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 0;
	}
	if (find_sysdir()) {
		fprintf(stderr,
			"nitro-wmi no está cargado (no hay %s/%s*/%s).\n"
			"Carga el módulo: cd ~/ports/nitro-wmi; and sudo insmod nitro_wmi.ko\n",
			SYS_WMI_BUS, NITRO_GUID_PREFIX, NITRO_DIR);
		return 2;
	}

	if (strcmp(argv[1], "status") == 0)
		return cmd_status();
	if (strcmp(argv[1], "fan") == 0)
		return cmd_fan(argv[2] ? argv[2] : "", argv[3]);
	if (strcmp(argv[1], "mode") == 0)
		return cmd_mode(argv[2]);
	if (strcmp(argv[1], "rpm") == 0) {
		int c = -1, g = -1;

		read_attr_u32("cpu_fan_rpm", &c);
		read_attr_u32("gpu_fan_rpm", &g);
		printf("CPU: %d RPM | GPU: %d RPM\n", c, g);
		return 0;
	}
	if (strcmp(argv[1], "temp") == 0) {
		int t = -1;

		read_attr_u32("cpu_temp", &t);
		printf("CPU: %d °C\n", t);
		return 0;
	}
	if (strcmp(argv[1], "turbo") == 0)
		return cmd_turbo(argv[2]);
	if (strcmp(argv[1], "table") == 0)
		return cmd_table(argv[2]);
	if (strcmp(argv[1], "gpuoc") == 0)
		return cmd_gpuoc(argv[2]);
	if (strcmp(argv[1], "monitor") == 0)
		return cmd_monitor(argc > 2 ? atoi(argv[2]) : 2);
	if (strcmp(argv[1], "raw") == 0)
		return cmd_raw(argv[2], argv[3]);

	usage();
	return 0;
}
