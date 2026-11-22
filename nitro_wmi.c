// SPDX-License-Identifier: GPL-2.0
/*
 * nitro-wmi — Control de fans/modos para laptops Acer Nitro/Predator en Linux
 *
 * Port de la funcionalidad central de NitroSense 3.01.3056 (Acer) a Linux,
 * basado en ingeniería inversa (REA/Ghidra) del protocolo y en el análisis
 * del firmware ACPI de un Acer Nitro AN515-54 (SSDT9, dispositivo WMID,
 * manejador WMBH de la clase AcerGamingFunction).
 *
 * Interfaz: bloque WMI GUID 7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56
 *
 * Layouts verificados contra el AML del firmware (WMBH):
 *   SetGamingFanSpeed(16):  in u64 = [0]=selector (1=CPU, 4=GPU), [1]=0-100
 *   GetGamingFanSpeed(17):  in u32 = [0]=selector → out byte[1]=%
 *   SetGamingFanTable(18):  in u64 = [0]=tabla 1-3
 *   GetGamingFanTable(19):   → out byte[1]=tabla actual
 *   SetGamingMiscSetting(22):in u64 = [0]=índice, [1]=valor
 *                            índice 1=TKST(turbo), 2=GPUM, 5=GPOC(0-2)
 *   GetGamingMiscSetting(23):in u32 = [0]=índice → out byte[1]=valor
 *   GetGamingSysInfo(5):     in u32 = [0]=1, [1]=sensor
 *                            1=temp CPU(CTMP), 2=RPM CPU(C1FR), 6=RPM GPU(G1FR)
 *                            → out bits 8-23 = valor
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_profile.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>

#define NITRO_WMI_GUID "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"
#define NITRO_EVENT_GUID "676AA15E-6A47-4D9F-A2CC-1E6D18D14026"
#define NITRO_EVENT_PREFIX "676AA15E"	/* prefijo del nombre del dispositivo */
#define NITRO_KEY_CODE KEY_MACRO1	/* tecla virtual para el launcher */

/*
 * Código de tecla (byte 1 del buffer APGe: [tipo, código, estado])
 * que dispara la tecla virtual. 0 = solo registrar los eventos en dmesg.
 * Ajuste en caliente: echo NN > .../parameters/nitro_event_code
 */
static uint nitro_event_code;
module_param(nitro_event_code, uint, 0644);
MODULE_PARM_DESC(nitro_event_code,
		 "codigo de tecla APGe (byte 1) que dispara la tecla NitroSense (0=solo log)");

/*
 * LMEN ("Launch Manager ENable", bit 7 del byte 3 del ECMM): en Windows,
 * el Launch Manager de Acer lo activa y el EC entrega las hotkeys de
 * lanzamiento (NitroSense/fan). El AML nunca lo escribe → en Linux nadie
 * lo activa y esas teclas quedan mudas. Lo habilitamos como en Windows.
 */
#define NITRO_ECMM_BASE		0xFC7E0800UL	/* OperationRegion ECMM (DSDT) */
#define NITRO_ECMM_SIZE		0x1000
#define NITRO_ECMM_LMEN_OFF	3		/* byte 3: LMEN|PSEE|RFEN|BTHE|BTCE */
#define NITRO_LMEN_BIT		BIT(7)

static void nitro_lmen_enable(struct device *dev)
{
	void __iomem *ecmm;
	u8 v;

	ecmm = ioremap(NITRO_ECMM_BASE, NITRO_ECMM_SIZE);
	if (!ecmm) {
		dev_warn(dev, "ECMM no mapeable\n");
		return;
	}
	v = ioread8(ecmm + NITRO_ECMM_LMEN_OFF);
	if (!(v & NITRO_LMEN_BIT)) {
		iowrite8(v | NITRO_LMEN_BIT, ecmm + NITRO_ECMM_LMEN_OFF);
		dev_info(dev,
			 "LMEN activado (Launch Manager) — hotkeys de teclas dedicadas habilitadas\n");
	}
	iounmap(ecmm);
}

/* Method IDs de AcerGamingFunction (MOF firmware v2.63) */
#define NITRO_M_GET_SYS_INFO		5
#define NITRO_M_SET_FAN_BEHAVIOR	14
#define NITRO_M_GET_FAN_BEHAVIOR	15
#define NITRO_M_SET_FAN_SPEED		16
#define NITRO_M_GET_FAN_SPEED		17
#define NITRO_M_SET_FAN_TABLE		18
#define NITRO_M_GET_FAN_TABLE		19
#define NITRO_M_SET_MISC		22
#define NITRO_M_GET_MISC		23

/* Bitmap de fans para SetGamingFanBehavior (informe 6: bit0=CPU, bit3=GPU) */
#define NITRO_FAN_BITMAP		((u64)BIT(0) | (u64)BIT(3))
/* Modos por fan (informe 6 + acer-wmi.c): 2 bits en [17:16] CPU, [23:22] GPU */
#define NITRO_FAN_MODE_AUTO		1
#define NITRO_FAN_MODE_TURBO		2
#define NITRO_FAN_MODE_CUSTOM		3

/* Índices de SetGamingMiscSetting válidos en esta generación de BIOS */
#define NITRO_MISC_TURBO_MODE	1	/* TKST: estado turbo/perfil */
#define NITRO_MISC_GPU_MODE	2	/* GPUM */
#define NITRO_MISC_GPU_OC	5	/* GPOC: nivel OC GPU 0-2 */

/* Selectores de fan para methods 16/17 */
#define NITRO_FAN_CPU			1
#define NITRO_FAN_GPU			4

/* Selectores de sensor para GetGamingSysInfo (grupo 1) */
#define NITRO_SENSOR_CPU_TEMP		1	/* CTMP  (°C) */
#define NITRO_SENSOR_CPU_FAN_RPM		2	/* C1FR  (RPM) */
#define NITRO_SENSOR_GPU_FAN_RPM		6	/* G1FR  (RPM) */

/* Estado en byte[0] de la respuesta: 0=OK, 2=rango inválido, 3=índice inválido */
#define NITRO_OUT_STATUS(out)		((u32)((out) & 0xFF))
#define NITRO_OUT_BYTE1(out)		((u32)(((out) >> 8) & 0xFF))
#define NITRO_OUT_VALUE_8_20(out)	((u32)(((out) >> 8) & 0x1FFF))

struct nitro_wmi_priv {
	struct wmi_device *wdev;
	u64 debug_result;
	int debug_status;
	/* Cache de valores set: el GET misc de esta generación de BIOS
	 * es un stub en AML (nunca lee el índice → siempre status 3),
	 * igual que NitroSense rastrea el estado en el registro. */
	u32 turbo_mode;
	u32 gpu_oc_level;
	bool turbo_set;
	bool gpu_oc_set;
	/* Cache del fan mode (method 15 devuelve ceros: stub del FOMB) */
	u32 fan_mode;		/* 0=auto 1=turbo 2=custom (nomenclatura NitroSense) */
	bool fan_mode_set;
	bool fan_cpu_custom;
	bool fan_gpu_custom;
	/* platform_profile (QUIET/BALANCED/PERFORMANCE) */
	enum platform_profile_option profile;
	/* RGB teclado 4 zonas (methods 6/7) + brillo (method 20) */
	u32 kb_rgb[4];
	bool kb_rgb_set[4];
	u32 kb_brightness;
	bool kb_brightness_set;
};

static int nitro_eval(struct wmi_device *wdev, u32 method_id, u64 in,
		      bool is_u64_in, u64 *out, u32 *fw_status);

/*
 * Aplica SetGamingFanBehavior con el estado por-fan dado.
 * (NitroSense fija el % SOLO junto con modo CUSTOM: el EC ignora
 * CPUF/GPUF mientras el fan no esté en CUSTOM — informe 6 §2.)
 */
static void nitro_apply_behavior(struct wmi_device *wdev,
				  struct nitro_wmi_priv *priv,
				  u32 mode_or_zero)
{
	u64 in;
	u32 st = 0;

	if (mode_or_zero == 2) {
		/* custom explícito (comando mode custom): ambos CUSTOM */
		in = NITRO_FAN_BITMAP |
		     ((u64)NITRO_FAN_MODE_CUSTOM << 16) |
		     ((u64)NITRO_FAN_MODE_CUSTOM << 22);
		priv->fan_cpu_custom = true;
		priv->fan_gpu_custom = true;
	} else {
		/* por-fan: cpu actual, gpu actual (para acompañar un % set) */
		in = NITRO_FAN_BITMAP |
		     ((u64)(priv->fan_cpu_custom ? NITRO_FAN_MODE_CUSTOM
						 : NITRO_FAN_MODE_AUTO) << 16) |
		     ((u64)(priv->fan_gpu_custom ? NITRO_FAN_MODE_CUSTOM
						 : NITRO_FAN_MODE_AUTO) << 22);
	}

	nitro_eval(wdev, NITRO_M_SET_FAN_BEHAVIOR, in, true, NULL, &st);
	dev_dbg(&wdev->dev, "behavior 0x%llx status=%u\n", in, st);
}

/*
 * nitro_eval_buf — evalúa un método WMI con buffer de entrada arbitrario.
 * Núcleo compartido por nitro_eval (u64/u32) y raw_call (bytes libres).
 */
static int nitro_eval_buf(struct wmi_device *wdev, u32 method_id,
			  const void *data, size_t len, u64 *out,
			  u32 *fw_status)
{
	struct acpi_buffer input;
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u64 tmp = 0;
	acpi_status status;
	int ret = 0;

	if (!data || len == 0)
		return -EINVAL;
	input.length = (acpi_size)len;
	input.pointer = (void *)data;

	status = wmidev_evaluate_method(wdev, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status)) {
		dev_dbg(&wdev->dev, "method %u eval failed: %d\n",
			method_id, status);
		return -EIO;
	}

	obj = result.pointer;
	if (!obj) {
		ret = -ENODATA;
		goto out_free;
	}

	switch (obj->type) {
	case ACPI_TYPE_BUFFER:
		if (obj->buffer.length == sizeof(u32))
			tmp = *(u32 *)obj->buffer.pointer;
		else if (obj->buffer.length == sizeof(u64))
			tmp = *(u64 *)obj->buffer.pointer;
		else
			ret = -EIO;
		break;
	case ACPI_TYPE_INTEGER:
		tmp = obj->integer.value;
		break;
	default:
		ret = -EIO;
		break;
	}

out_free:
	kfree(result.pointer);
	if (ret)
		return ret;
	if (out)
		*out = tmp;
	if (fw_status)
		*fw_status = NITRO_OUT_STATUS(tmp);
	return 0;
}

/*
 * Ejecuta un método WMI del bloque gaming (entrada u64 o u32 según el MOF).
 * El STATUS del firmware se devuelve en *fw_status.
 */
static int nitro_eval(struct wmi_device *wdev, u32 method_id, u64 in,
		      bool is_u64_in, u64 *out, u32 *fw_status)
{
	u32 in32;

	if (is_u64_in)
		return nitro_eval_buf(wdev, method_id, &in, sizeof(in),
				      out, fw_status);
	in32 = (u32)in;
	return nitro_eval_buf(wdev, method_id, &in32, sizeof(in32),
			      out, fw_status);
}

/* ---------- helpers tipados sobre nitro_eval ---------- */

static int nitro_fan_set(struct wmi_device *wdev, u32 selector, u32 percent)
{
	u64 in = selector | ((u64)percent << 8);
	u32 st = 0;
	int ret = nitro_eval(wdev, NITRO_M_SET_FAN_SPEED, in, true, NULL, &st);

	if (ret)
		return ret;
	if (st == 2)
		return -EINVAL;
	if (st)
		return -EIO;
	return 0;
}

static int nitro_fan_get(struct wmi_device *wdev, u32 selector, u32 *percent)
{
	u64 out = 0;
	u32 st = 0;
	int ret = nitro_eval(wdev, NITRO_M_GET_FAN_SPEED, selector, false,
			     &out, &st);

	if (ret)
		return ret;
	if (st)
		return -EIO;
	*percent = NITRO_OUT_BYTE1(out);
	return 0;
}

static int nitro_sensor_read(struct wmi_device *wdev, u32 sensor, u32 *value)
{
	u64 out = 0;
	u32 st = 0;
	u32 in = 1 | (sensor << 8);	/* grupo 1 + sensor */
	int ret = nitro_eval(wdev, NITRO_M_GET_SYS_INFO, in, false, &out, &st);

	if (ret)
		return ret;
	if (st == 2)
		return -EINVAL;		/* sensor no soportado */
	if (st)
		return -EIO;
	*value = NITRO_OUT_VALUE_8_20(out);
	return 0;
}

static int nitro_misc_set(struct wmi_device *wdev, u32 index, u32 value)
{
	u64 in = index | ((u64)value << 8);
	u32 st = 0;
	int ret = nitro_eval(wdev, NITRO_M_SET_MISC, in, true, NULL, &st);

	if (ret)
		return ret;
	/* Quirk de esta generación de BIOS (WMBH rama 0x16): la respuesta es
	 * el ECO de la entrada — byte[0] = índice en escrituras válidas,
	 * 3 = índice inválido. Las BIOS nuevas devuelven 0=OK de verdad.
	 * Aceptamos ambos comportamientos. */
	if (st == 0 || st == index)
		return 0;
	if (st == 3)
		return -EINVAL;		/* índice no soportado */
	return -EIO;
}

/*
 * Nota: GetGamingMiscSetting (23) es un stub en esta generación de BIOS
 * (su rama AML nunca lee el índice del buffer y devuelve status 3).
 * El estado se cachea al escribir, igual que NitroSense usa el registro.
 */

/* ---------- sysfs ---------- */

#define to_wdev(dev)	container_of(dev, struct wmi_device, dev)

static ssize_t fan_cpu_percentage_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val > 100)
		return -EINVAL;
	ret = nitro_fan_set(wdev, NITRO_FAN_CPU, val);
	if (ret)
		return ret;
	/* Fijar el % conmuta este fan a CUSTOM (flujo NitroSense):
	 * el EC solo aplica CPUF/GPUF en modo CUSTOM. */
	priv->fan_cpu_custom = true;
	priv->fan_mode = 2;
	priv->fan_mode_set = true;
	nitro_apply_behavior(wdev, priv, 0);
	return count;
}

static ssize_t fan_cpu_percentage_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u32 val;
	int ret = nitro_fan_get(wdev, NITRO_FAN_CPU, &val);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RW(fan_cpu_percentage);

static ssize_t fan_gpu_percentage_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val > 100)
		return -EINVAL;
	ret = nitro_fan_set(wdev, NITRO_FAN_GPU, val);
	if (ret)
		return ret;
	priv->fan_gpu_custom = true;
	priv->fan_mode = 2;
	priv->fan_mode_set = true;
	nitro_apply_behavior(wdev, priv, 0);
	return count;
}

static ssize_t fan_gpu_percentage_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u32 val;
	int ret = nitro_fan_get(wdev, NITRO_FAN_GPU, &val);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RW(fan_gpu_percentage);

static ssize_t fan_table_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	u64 out = 0;
	u32 val, st = 0;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val < 1 || val > 3)
		return -EINVAL;
	ret = nitro_eval(wdev, NITRO_M_SET_FAN_TABLE, val, true, &out, &st);
	if (ret)
		return ret;
	if (st == 2)
		return -EINVAL;
	if (st)
		return -EIO;
	return count;
}

static ssize_t fan_table_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u64 out = 0;
	u32 st = 0;
	int ret = nitro_eval(wdev, NITRO_M_GET_FAN_TABLE, 0, false, &out, &st);

	if (ret)
		return ret;
	if (st)
		return -EIO;
	return sysfs_emit(buf, "%u\n", NITRO_OUT_BYTE1(out));
}
static DEVICE_ATTR_RW(fan_table);

static ssize_t turbo_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val > 1)
		return -EINVAL;
	ret = nitro_misc_set(wdev, NITRO_MISC_TURBO_MODE, val);
	if (ret)
		return ret;
	priv->turbo_mode = val;
	priv->turbo_set = true;
	return count;
}

static ssize_t turbo_mode_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);

	/* GET no implementado por el firmware: devolver último valor fijado */
	if (!priv->turbo_set)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", priv->turbo_mode);
}
static DEVICE_ATTR_RW(turbo_mode);

static ssize_t gpu_oc_level_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val > 2)
		return -EINVAL;
	ret = nitro_misc_set(wdev, NITRO_MISC_GPU_OC, val);
	if (ret)
		return ret;
	priv->gpu_oc_level = val;
	priv->gpu_oc_set = true;
	return count;
}

static ssize_t gpu_oc_level_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);

	if (!priv->gpu_oc_set)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", priv->gpu_oc_level);
}
static DEVICE_ATTR_RW(gpu_oc_level);

static ssize_t cpu_fan_rpm_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u32 val;
	int ret = nitro_sensor_read(wdev, NITRO_SENSOR_CPU_FAN_RPM, &val);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(cpu_fan_rpm);

static ssize_t gpu_fan_rpm_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u32 val;
	int ret = nitro_sensor_read(wdev, NITRO_SENSOR_GPU_FAN_RPM, &val);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(gpu_fan_rpm);

static ssize_t cpu_temp_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	u32 val;
	int ret = nitro_sensor_read(wdev, NITRO_SENSOR_CPU_TEMP, &val);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(cpu_temp);

/*
 * raw_call — escape hatch para experimentación en vivo (métodos 1-15,
 * manejados por el VM FOMB del EC, u otros method ids).
 * Formato escritura: "<method_id> <hex>" — hex de N bytes (2..64, par;
 * 8 bytes para métodos u64, 4 para u32, 16 para SetGamingKBBacklight…)
 * Resultado: leer raw_result → "status=0x%02x out=0x%016llx"
 */
static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static ssize_t raw_call_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u8 data[64];
	char hex[136];
	u64 out = 0;
	u32 method, st = 0;
	size_t hlen, i;
	int ret;

	ret = sscanf(buf, "%u %135s", &method, hex);
	if (ret != 2 || method == 0 || method > 0x7F)
		return -EINVAL;

	hlen = strlen(hex);
	if (hlen < 2 || hlen > sizeof(data) * 2 || (hlen & 1))
		return -EINVAL;
	for (i = 0; i < hlen; i += 2) {
		int hi = hexval(hex[i]);
		int lo = hexval(hex[i + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		data[i / 2] = (u8)((hi << 4) | lo);
	}

	ret = nitro_eval_buf(wdev, method, data, hlen / 2, &out, &st);
	if (ret) {
		priv->debug_status = ret;
		priv->debug_result = 0;
		return ret;
	}
	priv->debug_status = (int)st;
	priv->debug_result = out;
	return count;
}

static ssize_t raw_result_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "status=0x%02x out=0x%016llx\n",
			 (u32)priv->debug_status & 0xFF,
			 priv->debug_result);
}
static DEVICE_ATTR_WO(raw_call);
static DEVICE_ATTR_RO(raw_result);

/*
 * fan_mode — modo conjunto de fans (SetGamingFanBehavior, method 14).
 * Entrada sysfs: 0=auto, 1=turbo, 2=custom (nomenclatura NitroSense,
 * CurrentFanMode del registro). Encodings informe 6:
 *   auto   = 0x00410009  (CPU=AUTO,  GPU=AUTO)
 *   turbo  = 0x00820009  (CPU=TURBO, GPU=TURBO)  (CoolBoost)
 *   custom = 0x00C30009  (CPU/GPU=CUSTOM; fijar % antes vía fan_*_percentage)
 * Nota: en esta BIOS methods 1-15 pasan por el intérprete FOMB del EC;
 * si devuelve error, se cachea igual que NitroSense.
 */
static ssize_t fan_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u32 mode;
	u64 in;
	u32 beh_mode;
	u32 st = 0;
	int ret;

	ret = kstrtou32(buf, 10, &mode);
	if (ret || mode > 2)
		return -EINVAL;

	switch (mode) {
	case 0:	/* auto: el EC retoma su curva (behavior 0x410009) */
	case 1:	/* turbo (CoolBoost): behavior 0x820009 */
		priv->fan_cpu_custom = false;
		priv->fan_gpu_custom = false;
		break;
	case 2:
		break;	/* nitro_apply_behavior(mode=2) marca ambos custom */
	}

	in = NITRO_FAN_BITMAP;
	beh_mode = (mode == 0) ? NITRO_FAN_MODE_AUTO :
		   (mode == 1) ? NITRO_FAN_MODE_TURBO : NITRO_FAN_MODE_CUSTOM;
	in |= ((u64)beh_mode << 16) | ((u64)beh_mode << 22);

	ret = nitro_eval(wdev, NITRO_M_SET_FAN_BEHAVIOR, in, true, NULL, &st);
	if (ret)
		return ret;
	if (st) {
		/* FOMB/EC rechazó el método: registrar y cachear igualmente
		 * (NitroSense también degrada según versión WMI del BIOS) */
		dev_dbg(&wdev->dev, "fan_behavior status=%u (FOMB)\n", st);
		return -ENODEV;
	}
	priv->fan_mode = mode;
	priv->fan_mode_set = true;
	if (mode == 2) {
		priv->fan_cpu_custom = true;
		priv->fan_gpu_custom = true;
	}
	return count;
}

static ssize_t fan_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u64 out = 0;
	u32 st = 0;
	u32 cpu_m, gpu_m;
	int ret;

	/* GetGamingFanBehavior (15): in=bitmap u32; out cpu=[9:8], gpu=[15:14]
	 * En esta BIOS el FOMB devuelve ceros (stub): 0/0 → usar cache. */
	ret = nitro_eval(wdev, NITRO_M_GET_FAN_BEHAVIOR,
			 (u64)NITRO_FAN_BITMAP, false, &out, &st);
	if (!ret && !st && (out >> 8 & 0x3) != 0) {
		cpu_m = (out >> 8) & 0x3;
		gpu_m = (out >> 14) & 0x3;
		if (cpu_m == NITRO_FAN_MODE_TURBO &&
		    gpu_m == NITRO_FAN_MODE_TURBO)
			return sysfs_emit(buf, "1\n");
		if (cpu_m == NITRO_FAN_MODE_AUTO &&
		    gpu_m == NITRO_FAN_MODE_AUTO)
			return sysfs_emit(buf, "0\n");
		if (cpu_m == NITRO_FAN_MODE_CUSTOM &&
		    gpu_m == NITRO_FAN_MODE_CUSTOM)
			return sysfs_emit(buf, "2\n");
		return sysfs_emit(buf, "mixed(%u/%u)\n", cpu_m, gpu_m);
	}
	if (!priv->fan_mode_set)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", priv->fan_mode);
}
static DEVICE_ATTR_RW(fan_mode);

/*
 * RGB del teclado por zonas — WMBH methods 6/7 (layout verificado en AML):
 *   entrada: 4 bytes [bit de zona: 1|2|4|8][R][G][B] → EC KBBA/KBxR/G/B
 *   salida (GET): [status][R][G][B]
 */
static int nitro_kb_rgb_set(struct wmi_device *wdev, u32 zone_bit,
			    u32 r, u32 g, u32 b)
{
	u8 data[4] = { (u8)zone_bit, (u8)r, (u8)g, (u8)b };
	u32 st = 0;
	int ret = nitro_eval_buf(wdev, 6, data, 4, NULL, &st);

	if (ret)
		return ret;
	if (st)
		return -EIO;
	return 0;
}

static int nitro_kb_rgb_get(struct wmi_device *wdev, u32 zone_bit,
			    u8 *r, u8 *g, u8 *b)
{
	u8 in[4] = { (u8)zone_bit, 0, 0, 0 };
	u64 out = 0;
	u32 st = 0;
	int ret = nitro_eval_buf(wdev, 7, in, 4, &out, &st);

	if (ret)
		return ret;
	if (st)
		return -EIO;
	*r = (out >> 8) & 0xFF;
	*g = (out >> 16) & 0xFF;
	*b = (out >> 24) & 0xFF;
	return 0;
}

#define NITRO_KB_RGB_ZONE(n, bit)					\
static ssize_t kb_rgb_zone##n##_store(struct device *dev,		\
				      struct device_attribute *attr,	\
				      const char *buf, size_t count)	\
{									\
	struct wmi_device *wdev = to_wdev(dev);			\
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);		\
	u32 rgb;							\
	int ret = kstrtou32(buf, 16, &rgb);				\
									\
	if (ret || rgb > 0xFFFFFF)					\
		return -EINVAL;						\
	ret = nitro_kb_rgb_set(wdev, bit, (rgb >> 16) & 0xFF,		\
			       (rgb >> 8) & 0xFF, rgb & 0xFF);		\
	if (ret)							\
		return ret;						\
	priv->kb_rgb[n - 1] = rgb;					\
	priv->kb_rgb_set[n - 1] = true;				\
	return count;							\
}									\
static ssize_t kb_rgb_zone##n##_show(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)			\
{\
	struct wmi_device *wdev = to_wdev(dev);			\
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);		\
	u8 r = 0, g = 0, b = 0;					\
									\
	if (nitro_kb_rgb_get(wdev, bit, &r, &g, &b) == 0)		\
		return sysfs_emit(buf, "%02x%02x%02x\n", r, g, b);	\
	if (priv->kb_rgb_set[n - 1])					\
		return sysfs_emit(buf, "%06x\n", priv->kb_rgb[n - 1]);\
	return -ENODATA;						\
}									\
static DEVICE_ATTR_RW(kb_rgb_zone##n)

NITRO_KB_RGB_ZONE(1, 1);
NITRO_KB_RGB_ZONE(2, 2);
NITRO_KB_RGB_ZONE(3, 4);
NITRO_KB_RGB_ZONE(4, 8);

/*
 * Brillo del teclado — method 20, 16 bytes:
 * [0]=KBLE(!=0xFF habilita) [1]=KBLS [2]=KBBP(%) [3]=KBCS [4]=KBED
 * [5..7]=KBCR/KBCG/KBCB (color único) — NitroSense usa KBBP=(nivel-1)*25
 */
static ssize_t kb_brightness_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct wmi_device *wdev = to_wdev(dev);
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	u8 data[16] = { 0 };
	u32 level;
	u32 st = 0;
	int ret;

	ret = kstrtou32(buf, 10, &level);
	if (ret || level < 1 || level > 4)
		return -EINVAL;
	data[2] = (u8)((level - 1) * 25);		/* KBBP: 0/25/50/75 */
	ret = nitro_eval_buf(wdev, 20, data, sizeof(data), NULL, &st);
	if (ret)
		return ret;
	if (st)
		return -EIO;
	priv->kb_brightness = level;
	priv->kb_brightness_set = true;
	return count;
}

static ssize_t kb_brightness_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);

	if (!priv->kb_brightness_set)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", priv->kb_brightness);
}
static DEVICE_ATTR_RW(kb_brightness);

static struct attribute *nitro_wmi_attrs[] = {
	&dev_attr_fan_cpu_percentage.attr,
	&dev_attr_fan_gpu_percentage.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_fan_table.attr,
	&dev_attr_turbo_mode.attr,
	&dev_attr_gpu_oc_level.attr,
	&dev_attr_cpu_fan_rpm.attr,
	&dev_attr_gpu_fan_rpm.attr,
	&dev_attr_cpu_temp.attr,
	&dev_attr_kb_rgb_zone1.attr,
	&dev_attr_kb_rgb_zone2.attr,
	&dev_attr_kb_rgb_zone3.attr,
	&dev_attr_kb_rgb_zone4.attr,
	&dev_attr_kb_brightness.attr,
	&dev_attr_raw_call.attr,
	&dev_attr_raw_result.attr,
	NULL
};

static const struct attribute_group nitro_wmi_group = {
	.name	= "nitro_wmi",
	.attrs	= nitro_wmi_attrs,
};

/* ---------- platform_profile (integración estándar del kernel) ---------- */
/*
 * DESHABILITADO en v0.4.4: la limpieza devm de platform_profile provocó
 * un device_del(NULL) al desmontar (oops CR2=8, ver acta). Se reintroducirá
 * en v0.5 con el patrón verificado del source del kernel (no devm).
 */
#define NITRO_ENABLE_PPROF 0

#if NITRO_ENABLE_PPROF
/*
 * Mapeo de perfiles (equivalente a los 3 perfiles XTU de NitroSense):
 *   QUIET        → turbo OFF + tabla de fans 1 (Normal)
 *   BALANCED     → turbo OFF + tabla de fans 2 (Fast)
 *   PERFORMANCE  → turbo ON  + tabla de fans 3 (Turbo)
 */
static int nitro_pprof_probe(void *drvdata, unsigned long *choices)
{
	*choices = BIT(PLATFORM_PROFILE_QUIET) |
		   BIT(PLATFORM_PROFILE_BALANCED) |
		   BIT(PLATFORM_PROFILE_PERFORMANCE);
	return 0;
}

static int nitro_pprof_get(struct device *dev,
			   enum platform_profile_option *profile)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);

	*profile = priv->profile;
	return 0;
}

static int nitro_pprof_set(struct device *dev,
			   enum platform_profile_option profile)
{
	struct nitro_wmi_priv *priv = dev_get_drvdata(dev);
	struct wmi_device *wdev = priv->wdev;
	u64 table;
	u32 st = 0;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		table = 1;
		ret = nitro_misc_set(wdev, NITRO_MISC_TURBO_MODE, 0);
		break;
	case PLATFORM_PROFILE_BALANCED:
		table = 2;
		ret = nitro_misc_set(wdev, NITRO_MISC_TURBO_MODE, 0);
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		table = 3;
		ret = nitro_misc_set(wdev, NITRO_MISC_TURBO_MODE, 1);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (ret)
		return ret;

	ret = nitro_eval(wdev, NITRO_M_SET_FAN_TABLE, table, true, NULL, &st);
	if (ret)
		return ret;
	if (st == 2)
		return -EINVAL;
	if (st)
		return -EIO;

	priv->profile = profile;
	return 0;
}

static const struct platform_profile_ops nitro_pprof_ops = {
	.probe		= nitro_pprof_probe,
	.profile_get	= nitro_pprof_get,
	.profile_set	= nitro_pprof_set,
};
#endif /* NITRO_ENABLE_PPROF */

/* ---------- dispositivo de eventos (tecla NitroSense, GUID 676AA15E) ---------- */

static void nitro_event_notify(struct wmi_device *wdev, union acpi_object *data)
{
	struct input_dev *input = dev_get_drvdata(&wdev->dev);
	const u8 *bytes = NULL;
	size_t len = 0;

	if (!data)
		return;
	if (data->type == ACPI_TYPE_BUFFER) {
		bytes = data->buffer.pointer;
		len = data->buffer.length;
	} else if (data->type == ACPI_TYPE_INTEGER) {
		bytes = (const u8 *)&data->integer.value;
		len = sizeof(data->integer.value);
	}

	if (bytes && len)
		dev_info(&wdev->dev, "evento APGe: %*phN\n",
			 (int)min(len, (size_t)16), bytes);

	if (!input || !bytes || !len || !nitro_event_code)
		return;
	/* byte 1 = código de tecla (byte 0 = tipo: 1/2 hotkey, 4 brillo…) */
	if (len < 2 || bytes[1] != (u8)nitro_event_code)
		return;

	/* pulso de tecla virtual */
	input_report_key(input, NITRO_KEY_CODE, 1);
	input_sync(input);
	input_report_key(input, NITRO_KEY_CODE, 0);
	input_sync(input);
}

static int nitro_event_probe(struct wmi_device *wdev)
{
	struct input_dev *input;
	int ret;

	/* Habilitar la entrega de hotkeys dedicadas como hace el
	 * Launch Manager de Acer en Windows (LMEN, bit 7 ECMM+3) */
	nitro_lmen_enable(&wdev->dev);

	input = devm_input_allocate_device(&wdev->dev);
	if (!input)
		return -ENOMEM;

	input->name = "NitroSense key";
	input->phys = "nitro-wmi/event0";
	input->id.bustype = BUS_HOST;
	input_set_capability(input, EV_KEY, NITRO_KEY_CODE);
	dev_set_drvdata(&wdev->dev, input);

	ret = input_register_device(input);
	if (ret)
		return ret;

	dev_info(&wdev->dev, "nitro-wmi: input de tecla registrado\n");
	return 0;
}

/* ---------- driver WMI ---------- */

static const struct wmi_device_id nitro_wmi_id_table[] = {
	{ .guid_string = NITRO_WMI_GUID },			/* métodos gaming */
	{ .guid_string = NITRO_EVENT_GUID, .context = (void *)1UL },	/* eventos */
	{ }
};
MODULE_DEVICE_TABLE(wmi, nitro_wmi_id_table);

static int nitro_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct nitro_wmi_priv *priv;
	int ret;

	if (context)			/* GUID de eventos (676AA15E) */
		return nitro_event_probe(wdev);

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->wdev = wdev;
	priv->profile = PLATFORM_PROFILE_BALANCED;
	dev_set_drvdata(&wdev->dev, priv);

	ret = sysfs_create_group(&wdev->dev.kobj, &nitro_wmi_group);
	if (ret)
		return ret;

#if NITRO_ENABLE_PPROF
	{
		struct device *ppdev;

		ppdev = devm_platform_profile_register(&wdev->dev,
						       "nitro-wmi",
						       priv,
						       &nitro_pprof_ops);
		if (IS_ERR(ppdev))
			dev_warn(&wdev->dev,
				 "platform_profile no disponible: %ld\n",
				 PTR_ERR(ppdev));
	}
#endif

	dev_info(&wdev->dev,
		 "nitro-wmi: AcerGamingFunction registrado (NitroSense port)\n");
	return 0;
}

static void nitro_wmi_remove(struct wmi_device *wdev)
{
	if (!strncmp(dev_name(&wdev->dev), NITRO_EVENT_PREFIX,
		     strlen(NITRO_EVENT_PREFIX))) {
		/* dispositivo de eventos: quitar el input aquí (no via
		 * devm — orden de devres con el core WMI es problemático) */
		struct input_dev *input = dev_get_drvdata(&wdev->dev);

		dev_info(&wdev->dev, "quitando input de tecla\n");
		if (input) {
			dev_set_drvdata(&wdev->dev, NULL);
			input_unregister_device(input);
		}
		return;
	}
	dev_info(&wdev->dev, "quitando grupo sysfs\n");
	sysfs_remove_group(&wdev->dev.kobj, &nitro_wmi_group);
}

static struct wmi_driver nitro_wmi_driver = {
	.driver		= {
		.name	= "nitro-wmi",
	},
	.id_table	= nitro_wmi_id_table,
	.probe		= nitro_wmi_probe,
	.remove		= nitro_wmi_remove,
	.notify		= nitro_event_notify,
};
module_wmi_driver(nitro_wmi_driver);

MODULE_AUTHOR("Port project (RE de NitroSense 3.01.3056 + firmware AN515-54)");
MODULE_DESCRIPTION("Control de ventiladores y modos para Acer Nitro/Predator (port de NitroSense)");
MODULE_VERSION("0.4.4");
MODULE_LICENSE("GPL");
