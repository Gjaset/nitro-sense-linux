# Informe 4 — Prior art: interfaz WMI gaming de Acer confirmada desde el módulo comunitario

> Fuente: JafarAkhondali/acer-predator-turbo-and-rgb-keyboard-linux-module (631★), clonado en
> `/home/znamreg/ports/referencias/`. Archivo: `src/facer.c`. Extraído 21-oct-2026.

## 1. GUID WMI de la interfaz gaming — LA PIEZA QUE FALTABA

```c
#define WMID_GUID4  "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"   // ← gaming: fans/LED/modos

static acpi_status WMI_gaming_execute_u64(u32 method_id, u64 in, u64 *out)
{
    struct acpi_buffer input = { sizeof(u64), &in };
    status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);
    // resultado: buffer u32/u64 o entero ACPI
}
```
Otros GUIDs del ecosistema: `WMID_GUID1 6AF4F258-…`, `WMID_GUID2 95764E09-…`, `WMID_GUID3 61EF69EA-…`, `AMW0_GUID1 67C3371D-…` (LED antiguos), evento `676AA15E-…`.

## 2. Method IDs de la interfaz gaming (WMID_GUID4)

| Method ID | Nombre | Equivalente NitroSense (MOF) |
|---|---|---|
| 2 / 4 | SET/GET_GAMING_LED | Set/GetGamingProfileSetting (LED color) |
| 5 | GET_GAMING_SYS_INFO | GetGamingSysInfo (sensores/velocidades fan) |
| 6 | SET_GAMING_STATIC_LED | SetGamingLEDGroupColor |
| 14 | **SET_GAMING_FAN_BEHAVIOR** | **SetGamingFanBehavior** |
| 20 / 21 | SET/GET_GAMINGKBBL | SetGamingKBBacklight |
| 22 / 23 | SET/GET_GAMING_MISC_SETTING | **SetGamingMiscSetting** (modo perf.) |

## 3. Layout del u64 de SetGamingFanBehavior (desde `WMID_gaming_set_fan_mode`)

```c
u64 gpu_fan_config1 = 0, gpu_fan_config2 = 0;
if (cpu_fans > 0)        config2 |= 1;                              // bit 0: hay fan CPU
for (i < cpu_fans+gpu_fans) config2 |= 1 << (i+1);                 // bits 1..n: selección de cada fan
for (i < gpu_fans)       config2 |= 1 << (i+3);                    // bits 3..: fans GPU
if (cpu_fans > 0)        config1 |= fan_mode;                      // bits 0-1: modo fan CPU
for (i < cpu_fans+gpu_fans) config1 |= fan_mode << (2*i+2);        // 2 bits por fan
for (i < gpu_fans)       config1 |= fan_mode << (2*i+6);
WMI_gaming_execute_u64(14, config2 | (config1 << 16));             // ← u64 final
// fan_mode: 1 = auto, 2 = turbo
```

## 4. Layout del u64 de SetGamingMiscSetting — CONFIRMA a PSSvc

```c
#define ACER_GAMING_MISC_SETTING_INDEX_MASK  GENMASK_ULL(7, 0)   // bits 0-7:  índice/función
#define ACER_GAMING_MISC_SETTING_VALUE_MASK  GENMASK_ULL(15, 8)  // bits 8-15: valor
#define ACER_GAMING_MISC_SETTING_STATUS_MASK  GENMASK_ULL(7, 0)   // en el RESULTADO: 0 = OK

input = FIELD_PREP(INDEX, setting) | FIELD_PREP(VALUE, value);     // = (value<<8) | index
// → NitroSense enviaba (mode<<8)|0x0B → index 0x0B = modo de operación/perfil turbo ✓
```

## 5. Lectura de sensores (GET_GAMING_SYS_INFO, method 5)

```c
enum { GET_PREDATOR_V4_BAT_STATUS = 0x02, CPU_FAN_SPEED = 0x0201, GPU_FAN_SPEED = 0x0601 };
#define FAN_SPEED_READ_BIT_MASK   GENMASK(20, 8)   // RPM en bits 8-20 del resultado
#define SENSOR_READING_BIT_MASK   GENMASK_ULL(23, 8)
#define SUPPORTED_SENSORS_MASK    GENMASK_ULL(39, 24)
```

## 6. Estado de las piezas del port

| Pieza | Estado | Fuente |
|---|---|---|
| Protocolo pipe CMF/RMF + 35 comandos | ✓ completo | Informes 2 y 3 |
| Codificación modo operación `(mode<<8)\|0x0B` | ✓ confirmada ×2 | Informe 2 + facer.c |
| GUID WMI gaming | ✓ **7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56** | facer.c (este informe) |
| Method IDs WMI | ✓ tabla arriba | facer.c |
| u64 fan BEHAVIOR (auto/turbo) | ✓ layout bit a bit | facer.c (este informe) |
| u64 fan SPEED custom % (NitroSense lo tiene; el módulo NO) | ⏳ subagente analizando ADMINAGENT_EXE | informe 5 en camino |
| LEDs RGB zonas (módulo lo hace con GID 14/2/6) | ✓ referencia | facer.c |
| Perfiles XTU (AES) | ✓ formato + cifrado documentados; clave requiere dump runtime | Informes 1/1b |
| Verificación en hardware real | pendiente | requiere el laptop Acer |

**Conclusión del día**: el port a Linux tiene YA todas las piezas estáticas del lado "control básico"
(auto/turbo/modos/LED). La única pieza REA restante (fan speed custom %) se extrae de ADMINAGENT_EXE
(los informes 2 y 1b localizaron ahí las llamadas a SetGamingFanSpeed).
