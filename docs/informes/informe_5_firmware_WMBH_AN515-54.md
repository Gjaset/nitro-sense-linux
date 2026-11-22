# Informe 5 — Lado firmware: protocolo WMBH completo desde el SSDT9 del Nitro AN515-54

> Fuente: dump ACPI REAL del equipo (/home/znamreg/ports/programas/nitro_an51554_acpi/).
> MOF binario (bmof_10.bin) parseado con bmfdec; AML descompilado con iasl. 21-oct-2026.

## 1. Mapa `_WDG` del dispositivo `\_SB.PCI0.WMID` (ssdt9.dsl)

Cada entrada: GUID(16) + ObjectId(2) + Instance(1) + Flags(1). Entradas relevantes:

| GUID | ObjectId | Handler | Clase MOF |
|---|---|---|---|
| `676AA15E-…` | 0x00BC (evento) | EVBD/_WED | WMIEvent |
| `61EF69EA-…` | "AA" | `WMAA` | APGeAction (Set/GetFunction) |
| `79772EC5-…` | "BE" | `WMBE` | BatteryControl |
| `79772EC6-…` | "BF" | — | LongRunFunction |
| `F75F5666-…` | "BK" | `WMBK` | BIOSSetting |
| `FE1DBBDA-…` | "BL" | `WMBL` | UtilityFunction (passwords BIOS, LED…) |
| `77B0C3A7-…` | "BG" | `WMBG` | S3ResumeFunction |
| **`7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56`** | **"BH"** | **`WMBH`** | **AcerGamingFunction** |
| `4BB53443-…` | "AC" | `WMAC` | AcerBiosConfigurationTool |
| `9F98130E-…` | "AB" | `WMAB` | DataTrans (BIOSSettings cifradas) |
| `05901221-…` | "AB" | (bloque de datos) | MOF binario |

`WMBH(Arg0, Arg1=MethodId, Arg2=BufferIn)` → devuelve `BHSK`/`BHGK` (buffers de estado/respuesta).

## 2. Clase MOF `AcerGamingFunction` (bmof_10, "Version 2.63") — firmas oficiales

23 métodos declarados: SetGamingProfile(1), SetGamingLED(2), GetGamingProfile(3), GetGamingLED(4), **GetGamingSysInfo(5)**, **SetGamingRgbKb(6)**, GetGamingRgbKb(7), SetGamingProfileSetting(8), GetGamingProfileSetting(9), SetGamingLEDBehavior(10), GetGamingLEDBehavior(11), SetGamingLEDColor(12), GetGamingLEDColor(13), SetGamingFanBehavior(14), GetGamingFanBehavior(15), **SetGamingFanSpeed(16)**, **GetGamingFanSpeed(17)**, **SetGamingFanTable(18)**, GetGamingFanTable(19), **SetGamingKBBacklight(20)**, GetGamingKBBacklight(21), **SetGamingMiscSetting(22)**, GetGamingMiscSetting(23).

## 3. Lo que ESTA BIOS implementa realmente en `WMBH` (ssdt9 líneas 3124-3539)

⚠️ **Métodos 1-4, 8-15 NO tienen rama en WMBH** → esta BIOS (2019, AN515-54) NO soporta LED/Profile/FanBehavior por esta vía. Coincide con el chequeo "BIOS support WMI version" de PSSvc al iniciar y con la tabla de capacidades por modelo (AN515-54 = Type 1).

### 3.1 `SetGamingFanSpeed` (method 16) — CONTROL CUSTOM % ★
```
u64 entrada: byte[0] = selector: 1 = fan CPU, 4 = fan GPU
             byte[1] = velocidad 0-100 (0x64); >100 → error 0x02
→ byte[0]==1: EC0.CPUF = %     (EC MMIO 0xFC7E0800+0x37)
→ byte[0]==4: EC0.GPUF = %     (EC MMIO 0xFC7E0800+0x3A)
respuesta BHSK: [0]=0 OK / [0]=2 error
```

### 3.2 `GetGamingFanSpeed` (method 17)
```
u32 entrada: byte[0] = 1 (CPU) / 4 (GPU)
respuesta BHGK: [1] = EC0.CPUF o EC0.GPUF
```

### 3.3 `SetGamingFanTable` (method 18) / `GetGamingFanTable` (19)
```
u64: byte[0] = tabla 1-3 (fuera de rango → error 0x02) → EC0.CPOC (MMIO +0x28)
get: devuelve CPOC en byte[1]
```
Confirma informe 2: nivel OC 0/1/2 → tabla 1/2/3.

### 3.4 `SetGamingMiscSetting` (method 22) — MODOS
```
u64: byte[0] = índice de función, byte[1] = valor
  índice 1 → EC0.TKST = valor        (TURBO/perf mode state, MMIO +0x24)
  índice 2 → EC0.GPUM = 1            (GPU mode)
  índice 5 → EC0.GPOC = valor (solo 0-2; >2 → 0)   (GPU OC level, MMIO +0x29)
  otro índice → error 0x03
```
⚠️ PSSvc envía `(mode<<8)|0x0B` (índice 11) → es de BIOS MÁS NUEVAS. En ESTA máquina el modo turbo es índice 1 (TKST). El port debe detectar la versión WMI del BIOS (PSSvc lo loguea: "BIOS support WMI version") y elegir el índice.

### 3.5 `GetGamingSysInfo` (method 5) — SENSORES (helper GGSI)
```
u32 entrada: byte[0] = 1 (grupo lectura), byte[1] = sensor:
  1 → EC0.CTMP   (temperatura CPU)
  2 → EC0.C1FR   (RPM fan CPU)
  3 → EC0.S1TV
  6 → EC0.G1FR   (RPM fan GPU)
respuesta u64: bits 8-20 = valor (byte[1]=low, byte[2]=high)
```
✓ Coincide con facer.c: inputs 0x0201 (CPU RPM) / 0x0601 (GPU RPM), máscara de lectura GENMASK(20,8).

### 3.6 `SetGamingKBBacklight` (20) / `SetGamingRgbKb` (6)
Buffers de 16 bytes; RGB del teclado también accesible directo en EC MMIO `KB1R/KB1G/KB1B` desde `0xFC7E0800+0x3C`.

## 4. Mapa EC MMIO (OperationRegion ECMM, SystemMemory 0xFC7E0800, Field en dsdt.dsl ~L49242)

| Offset | Campo | Significado |
|---|---|---|
| 0x24 | TKST | Turbo/estado modo |
| 0x25 | GPUM | GPU mode |
| 0x28 | CPOC | Perfil OC/tabla fan (1-3) |
| 0x29 | GPOC | Nivel OC GPU (0-2) |
| 0x37 | CPUF | % fan CPU |
| 0x3A | GPUF | % fan GPU |
| 0x3C+ | KB1R/G/B… | RGB teclado por zonas |

## 5. Verificación cruzada final (4 fuentes)

| Dato | PSSvc (informe 2) | facer.c (informe 4) | MOF (bmof) | AML WMBH (este informe) |
|---|---|---|---|---|
| GUID gaming | nombre de clase | **7A4DDFE7** | 7A4DDFE7 ✓ | _WDG "BH" ✓ |
| FanBehavior id | 0x0F pipe→WMI | 14 ✓ | 14 ✓ | **no implementado en esta BIOS** |
| FanSpeed id | 0x10 pipe→WMI | — | 16 ✓ | **16 ✓ con layout u64 completo** |
| MiscSetting | (mode<<8)\|0x0B | 22, bits 8-15=value ✓ | 22 ✓ | 22 ✓ índices 1/2/5 en esta BIOS |
| FanTable | tablas 1/2/3 | — | 18 ✓ | 18 ✓ rango 1-3 ✓ |
| RPMs | — | 0x0201/0x0601, bits 8-20 ✓ | 5 ✓ | 5 ✓ CTMP/C1FR/G1FR ✓ |

## 6. Implicaciones para el port a Linux (AN515-54)

1. **Ruta recomendada: WMI** con `wmi_evaluate_method(GUID 7A4DDFE7, 0, method_id, in_u64, &out)` — espejo exacto de lo que hace el firmware; validaciones incluidas gratis (rango 0-100, tabla 1-3).
2. **Ruta directa EC MMIO** (`/dev/mem` 0xFC7E0800+off o módulo ec): la usan herramientas de la comunidad; más riesgo (sin validación), no necesaria si WMI funciona.
3. Funciones soportadas por ESTA máquina para el port: **fan custom % (CPU y GPU), lectura RPM, temp CPU, tabla fan 1-3, turbo (TKST), OC GPU (GPOC), backlight/RGB KB**. NO disponibles: FanBehavior(14) ni LED methods → el port debe degradar con gracia (como hace NitroSense según la versión WMI del BIOS).
4. El "modo de rendimiento" del port para esta máquina = MiscSetting índice 1 (TKST) + FanTable; el índice 0x0B solo para BIOS nuevas (a documentar para otros modelos).
