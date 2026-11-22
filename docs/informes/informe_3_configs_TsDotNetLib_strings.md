# Informe 3/3 — Configs, TsDotNetLib y barrido de strings (NitroSense V3.01.3056)

> Generado por subagente (ses_ead7227b4ffeer1T6mevblqoH9), 21-oct-2026.
> Archivos: Plugs/*.ini (28 modelos, 73 INIs), FILE_APP_DOTNETLIB_DLL (TsDotNetLib), barrido strings de los 19 FILE_APP_*.

---

## A) Inventario de configs — `Plugs/`

Hallazgo estructural: los 25 `HW_Support.ini` existentes son **byte-idénticos** (md5 `327390b90171bb2c5dabbfa6ce8a6316`). La diferenciación por modelo vive solo en `Feature.ini` (variantes `Type` 0/1/2 × `Year`) y en `NitroSense.ini` (solo 7 modelos: listas OC de GPU). Las líneas `Type:1` son anomalías reales del archivo (clave con `:` en vez de `=`) — el porte debe tolerarlas.

### A.1 Unión de esquema (solo claves existentes)

**Feature.ini** — `[MachineType] Type = 0|1|2` · `[LauchTask]*(sic)* DelayTime = 7` · `[YearType] Year = 2019|2020|2021|2022` (ausente en AN515-42/52/53) · `[Planet9Support] Planet9 = 1, Planet9_Link = https://p9.gg/NitroSense`

**HW_Support.ini** — `[LightingType]` **`Type:1`**, `PerKey=0` · `[ZoneDefaultColor] Zone1..4=#FF0000` · `[HotkeyDefaultColor] Group1=#009BF0, Group2=#00FF00, Group3=#FF0000` · `[ZoneColorAdjust] R=1.0, G=1.0, B=1.0` · `[HotkeyColorAdjust] R=0.9, G=1.0, B=1.0` · `[AdvanceSetting] Count=3, FUN1=Animation, FUN2=LCD, FUN3=Temperature` · `[KeyboardSetting] Count=3, FUN1=Backlight, FUN2=Sticky_Key, FUN3=Windowskey1` · `[FanSupport] CPU=1, GPU=1, System=0` · `[FanDtail]*(sic)*` **`Type:1`**

**NitroSense.ini** — `[OC_CPU] Count=0` (siempre) · `[PL_CPU] BIOSVersion=000` · `[OC_GPU] Count=8|10|11`, `GPUName1..11 = PCI\VEN_10DE&DEV_xxxx&SUBSYS_yyyy1025` · 37 secciones `[PCI\VEN_10DE&…&SUBSYS_…1025]` con siempre: `Quiet=0, Default=0, Performance=100, RAM_Quiet=0, RAM_Default=0, RAM_Performance=200`

### A.2 Tabla modelo → capacidades (28 modelos; P9=Planet9)

| Modelo | Type | Year | HW | NS | OC_GPU |
|---|---|---|---|---|---|
| N9500-A | 1 | 2020 | sí | no | — |
| Nitro AN515-42 | 0 | — | **no** | no | — |
| Nitro AN515-43 / -54 | 1 | 2019 | sí | no | — |
| Nitro AN515-44 / -55, N9500-A | 1 | 2020 | sí | no | — |
| Nitro AN515-45/-51s/-56/-57, AN517-41/-53/-54, T9300 | 1 | 2021 | sí | no | — |
| Nitro AN515-52 | 0 | — | **no** | no | — (idéntico a AN515-42 + 1 byte) |
| Nitro AN515-53 | 0 | — (sin `[LauchTask]`) | **no** | no | — |
| Nitro AN515-46 / -47 | 2 | 2022 | sí | sí | 8× `159E1025`: 1F9D, 25A2, 25A0, 2520, 24A0, 249D, 25AD, 25A5 |
| Nitro AN517-42 / -43 | 2 | 2022 | sí | sí | 8× `159D1025` (mismos DEV) |
| Nitro AN517-55 | 2 | 2022 | sí | sí | 10× `15971025` (mismos DEV) |
| **Nitro AN515-58 / ANX** | **2** | **2022** | sí | sí | **11**: 9× `159C1025` + `2560&16461025` |
| Nitro AN517-51, AN715-51 | 1 | 2019 | sí | no | — |
| Nitro AN517-52, AN715-41/-52 | 1 | 2020/2020 | sí | no | — |

Capacidades declaradas (iguales en los 25 con HW): ventiladores **CPU+GPU** (`System=0`), RGB **por zonas** (4 zonas, `PerKey=0`), 3 grupos de hotkey, avanzados `Animation/LCD/Temperature`, teclado `Backlight/Sticky_Key/Windowskey1`; OC **solo GPU** con offsets `Quiet 0 / Default 0 / Performance 100` (+`RAM_*` 0/0/200). `SUBSYS_…1025` = Acer (vendor PCI 1025).

### A.3 Modelo de referencia

**Nitro AN515-58** (empata con ANX, `NitroSense.ini` idéntico): 27 secciones, 112 claves, 3 archivos, 11 GPUs OC. Mejor plantilla canónica para el porte.

### A.4 Contenido literal (los 4 archivos pedidos)

`AN515-58/Feature.ini` ≡ `AN517-43/Feature.ini` (byte a byte); `AN515-58/HW_Support.ini` ≡ `AN517-43/HW_Support.ini` ≡ los otros 23:

```ini
[MachineType]
Type=2
[LauchTask]
DelayTime=7
[YearType]
Year=2022
[Planet9Support]
Planet9=1
Planet9_Link=https://p9.gg/NitroSense
```

```ini
[LightingType]
Type:1
PerKey=0
[ZoneDefaultColor]
Zone1=#FF0000
Zone2=#FF0000
Zone3=#FF0000
Zone4=#FF0000
[HotkeyDefaultColor]
Group1=#009BF0
Group2=#00FF00
Group3=#FF0000
[ZoneColorAdjust]
R=1.0
G=1.0
B=1.0
[HotkeyColorAdjust]
R=0.9
G=1.0
B=1.0
[AdvanceSetting]
Count=3
FUN1=Animation
FUN2=LCD
FUN3=Temperature
[KeyboardSetting]
Count=3
FUN1=Backlight
FUN2=Sticky_Key
FUN3=Windowskey1
[FanSupport]
CPU=1
GPU=1
System=0
[FanDtail]
Type:1
```

---

## B) `FILE_APP_DOTNETLIB_DLL` — **TsDotNetLib v3.01.3056** (Ghidra 12.1.4 + parser ECMA-335 para los 215 valores constantes)

### B.1 Identidad

`TsDotNetLib` v3.1.3056.0, .NET Framework 4.5, IL-only, PE32+ x86-64, sin strong-name, 28 360 B, SHA-256 `79a1645c…`, MVID `36d33b01-…`. Metadatos: 27 TypeDef, 251 Field, **21 MethodDef, 215 Constant**, 2 ModuleRef+ImplMap (P/Invoke), 52 TypeRef, 15 CustomAttribute. "Ts" = **Treadstone** (namespace del servicio: `treadstone::TsClientCommandProcessor`).

### B.2 Clases y métodos expuestos

| Clase | Miembros |
|---|---|
| `IPCMethods` (estática) | `FindWindow(string,string)→IntPtr` *(P/Invoke User32)*, `PostMessage(IntPtr,uint,UIntPtr,IntPtr)→bool` *(P/Invoke user32, SetLastError)*, `SendCommandByNamedPipe(NAMEDPIPE_COMMAND,int,object[])`, `SendCommandByWindowMessage(string,string,uint,uint,int)→bool`, `GetSizeOnMemory(object)→int`, `AnyToBytes(object)→byte[]`, `BytesToStruct<T>(byte[])→T` |
| `Log` | `.ctor(string,string)`, `LogWrite(...)`, `Write(LogType,string,string)` — niveles `[INFO]/[WARNING]/[ERROR]/[EXCEPTION]` |
| `Registry` (estática) | `ValueExistsLM`, `CheckLM(…,uint)`, `CheckCU`, `GetStringLM`, `SetValueLM`, `SetStringLM`, `SetBinaryLM`, `CheckLMBinary` (LM=HKLM, CU=HKCU) |
| `ConstDef` | constantes del contrato (ver B.3) |

### B.3 Contrato IPC (constantes verificadas en metadata)

- **Rutas de registro**: `SOFTWARE\OEM\NitroSense` + `\KeyAssignment`, `\LightSetting`, `\AdvanceSettings`, `\ColorRecord`, `\FanControl`, `\Overclock`
- **Pipes**: `ServicePipeName = PredatorSense_service_namedpipe`; `AdminAgentBasePipeName = PredatorSense_admin_agent_`
- **Framing CMF petición**: `CMF_CMD_INDEX=0/SIZE=2` (u16 cmd), `CMF_NUM_ARG_INDEX=2/SIZE=1` (u8 argc), `CMF_ARG_SIZE_SIZE=4` → **[u16 cmd][u8 argc][u32 size]×argc en offset 3 SIN alinear** ✱ confirma el protocolo
- **Framing RMF respuesta**: `RMF_NUM_ARG_INDEX=0/SIZE=1`, `RMF_ARG_SIZE_SIZE=4` → [u8 retArgc][u32 size]×n; primer argumento de retorno = **status u32**

### B.4 Cómo empaqueta los argumentos (CIL decodificado)

`SendCommandByNamedPipe` (306 B IL): por cada elemento del `object[]` → `GetSizeOnMemory` → cabecera CMF (`BitConverter.GetBytes`, `Array.Copy`) → `AnyToBytes` → buffer único → `NamedPipeClientStream.Connect()` → `Write` → `WaitForPipeDrain()` → `Close()`. Serialización por tipo:

- **string** → `Encoding.Unicode.GetBytes` (UTF-16LE); tamaño = `Length*2 + 2`
- **byte[]** → `Length` directo
- **struct/enum (incluido cualquier u64)** → `Marshal.SizeOf(obj)` bytes + `GCHandle.Alloc(Pinned)` + `Marshal.StructureToPtr` → **bytes crudos little-endian tal como están en memoria**. Inverso: `BytesToStruct<T>` = `AllocHGlobal`+`Marshal.Copy`+`PtrToStructure`+`FreeHGlobal`.

### B.5 El u64 de fan speed/behavior — dónde está (y dónde NO está) el bit-packing

- **TsDotNetLib NO construye el u64**: los opcode_counts de los 21 métodos no contienen **ni un solo** `shl`/`or`/`conv.i8`/`conv.u8`. La DLL es transporte puro.
- **El servicio tampoco**: cmd 0x0F (`FUN_14000ea60`) y 0x10 (`FUN_14000ecc0`) leen el u64 crudo del pipe, lo envuelven en **VARIANT VT_UI8** y lo reenvían **intacto** a `AcerGamingFunction.SetGamingFanBehavior` / `.SetGamingFanSpeed` vía `SELECT * FROM AcerGamingFunction` (2 in / 2 out).
- **Conclusión**: el bit-packing (grupo de fan / nivel 0-100 / auto-custom) lo hace el **cliente UI** (NitroSense.exe UWP, NO incluido en este paquete) y el layout lo define el **firmware ACPI**. Para el porte: u64 opaco end-to-end hasta el driver ACPI; semántica de bits se infiere de la UI o por experimentación en hardware.

### B.6 Mapa comando→método — verificado contra el servidor nativo

Tabla de dispatch = array denso de 35 punteros en `.rdata` (`0x140053430–0x140053540`), coincide **1:1** con el enum `ServiceCommand` de la DLL (0x00–0x22). Handlers 0x0F, 0x10, 0x18 y 0x1E verificados con pseudocódigo.

| Cmd | Handler | Método/acción WMI o SDK |
|---|---|---|
| 0x00 | `EchoArguments` | eco |
| 0x01–0x06 | registro | `RegCreateKey/RegDeleteKey/RegCreateValue/RegDeleteValue/RegSetValue/RegGetValue` |
| 0x07/0x08 | proceso | `CreateProcessAsUser`, `CreateProcessOnLogon` |
| 0x09/0x0A | `GamingPorfileWMISet/Get` *(sic)* | `SetGamingProfile` / `GetGamingProfile` |
| 0x0B/0x0C | color LED | `Get/SetGamingLEDGroupColor` |
| 0x0D | sysinfo | `GetGamingSystemInfo` |
| 0x0E | GPU load | `GetGPUUsageLoading` |
| **0x0F** | `0x14000ea60` | `AcerGamingFunction.SetGamingFanBehavior(u64 VT_UI8 crudo)` |
| **0x10** | `0x14000ecc0` | `AcerGamingFunction.SetGamingFanSpeed(u64 VT_UI8 crudo)` |
| 0x11–0x14 | WMI genérico | `WMISetFunction/WMISetLogoLED/WMISetLogoBehavior/WMIGetFunction` |
| 0x15–0x1A | OC GPU | `NvidiaOCGet/IntelOCGet/NvidiaOCSet/IntelOCSet/NvidiaSLISet/NvidiaGetCoprocStatus` |
| **0x18** | `0x140010410` | **Intel OC set**: nivel i32 (0/1/2) → perfil XTU pregenerado ("Normal/Turbo/Faster") → `SdkWrapperForNativeCode` |
| 0x1B–0x1D | KB/LED | `WMISetGamingKBBacklight/WMISetGamingRgbKbSetting/WMISetGamingLEDBehavior` |
| **0x1E** | `0x140012c60` | **`(mode << 8) | 0x0B`** → `SetGamingMiscSetting(u64)` ✱ confirmado |
| 0x1F/0x20 | GPU | `GPUGetCount`, `GPUGetFrequency` |
| 0x21/0x22 | misc | `WMISet/GetGamingMISCSetting` |

Otros enums extraídos: `AgentCommand` 0x8001–0x8005, `PSUICommand` 0x8001–0x8010 (0x8006 `HOTKEY_TRIGGER_FAN`, 0x800B MODE), `AdminAgentCommand` 0–15, `EVENT_TYPE` 0–14 (incl. `DUST_CLEAN=12`, `MODE_CHANGED=14`), `LogType` 0–3.

---

## C) Barrido global de strings (19 archivos `FILE_APP_*`)

| Archivo (tipo) | Hallazgos relevantes |
|---|---|
| **FILE_APP_SVC_EXE** (nativo) | `ROOT\WMI`+`ROOT\CIMV2`; WQL `SELECT * FROM AcerGamingFunction / APGeAction / APGeEvent / Win32_Processor`; `MSSMBios_RawSMBiosTables`; "Current BIOS support WMI version = %d.%02d"; pipe `predatorsense_service_namedpipe`; métodos WMI `Set/GetGaming*`; namespace `treadstone::TsClientCommandProcessor::*`; perfiles OC "Normal/Turbo/Faster path"; `C:\ProgramData\oem\NitroSense\NitroSense.ini`; `SOFTWARE\OEM\NitroSense\{Log,Overclock,AdvanceSettings}`; PDB PredatorSense_V3 |
| **FILE_APP_ADMINAGENT_EXE** (nativo) | GUID **`PSAdminAgent-{3257962E-6E0E-43B5-85E2-38BD5F6B22A5}`**; `ROOT\WMI`; `SELECT * FROM AcerGamingFunction`; registro `SOFTWARE\OEM\NitroSense\FanControl` con valores **`CurrentFanMode, CPUFanPercentage, CPUFanCustomAuto, GPU1FanPercentage/CustomAuto, CurrentCPUMode, CurrentGPUMode, CurrentOperationMode`**; `ProfilePool\{Light,Hotkey}ProfilePool`; UWP `AcerIncorporated.NitroSenseV31_48frkmn4z8aw4!App`; modos Waves/DTS/Whisper, "AC/DC/PD mode" |
| **FILE_APP_AGENT_EXE** (nativo) | GUID **`PSAgent-{D1EFBC5B-2F2D-49FE-B436-0D4BADF8B976}`**; OSD fan (`img_osd_fan_{auto,custom,max}.png`); lanza `PSCreateDefaultProfile.exe` |
| **FILE_APP_LAUNCHER_EXE** (nativo) | cliente pipe `predatorsense_service_namedpipe`; `OEM\NitroSense\ProfilePool` |
| **FILE_APP_CREATE_DEFAULT_PROFILE_EXE** (.NET) | `root\wmi`; `CoolBoostMode`; PDB `PSCreateDefaultProfile.pdb` |
| **FILE_APP_UPGRADETOOL_EXE** (.NET) | `NamedPipeClientStream`/`NAMEDPIPE_COMMAND` (usa TsDotNetLib); tarea programada XML; `SOFTWARE\OEM\UpgradeTool\NitroSenseV31N` |
| **FILE_APP_INTEL_OC_SDK_DLL** (.NET) | Intel XTU OC SDK: `root\CIMV2`; **WCF `NetNamedPipeBinding`**; GUIDs `{00AAC56B-CD44-11d0-8CC2-00C04FC295EE}`, `{B482F422-8810-4D32-ACFE-FD4A2D93AB24}`; XML de fan controllers/voltage modes |
| **FILE_APP_SDK_WRAPPER_DLL** (.NET C++/CLI) | `SdkWrapperLibrary.SdkWrapperForNativeCode` (`ApplyProfile/GetProfile/ImportProfile/ProposeProfile/UpdateProfile/XtuTuningProfile`) — puente al Intel OC SDK |
| **FILE_APP_PROFILE_HELPER_DLL** (.NET) | `ProfileHelperModel.Profile`, `Intel.Overclocking.SDK.Profile`, `intel.com/xtu/xtuProfileSchema` |
| **FILE_APP_ACRHTTP_DLL** (.NET i386) | Acer Live Updater: `SOFTWARE\Acer\{AcrHttp,Live Updater}`, endpoints `global-download.acer.com`, `appuws.acer.com/ServerInfo` |
| TOAST_CREATE / LISTCHECK / DEPLOYTOOl | sin hallazgos relevantes |
| **Terceros sin aporte**: MAXXAUDIO x64/x86 (Waves), DTSAUDIO (DTS), MSVCR120/VCRUNTIME140 (CRT) | — |

---

## Síntesis para el porte a Linux

UI → pipe `\\.\pipe\predatorsense_service_namedpipe` (framing CMF/RMF verificado, cmd u16 0x00–0x22) → PSSvc (35 handlers) → **WMI `AcerGamingFunction` en `root\WMI`** (u64 VT_UI8 opaco para fan; `(mode<<8)|0x0B` para modo de operación). OC GPU=NVIDIA (NVAPI), OC CPU=perfiles Intel XTU pregenerados por nivel 0/1/2.

Mapeo Linux: named pipe → **socket Unix** (mismo framing) · WMI → **método ACPI `WMBD/WMAB`** del driver `acer-wmi` o módulo WMI custom con `wmi_evaluate_method` · registro `SOFTWARE\OEM\NitroSense\*` → archivos de config propios · los 28 perfiles `Plugs/` solo aportan `MachineType/YearType` + lista de GPUs OC (8–11 IDs `VEN_10DE` subsistema Acer 1025).

**Pendiente**: bit-a-bit del u64 de fan — está en la app UWP NitroSense.exe (NO incluida en este MSI; viene de Microsoft Store `AcerIncorporated.NitroSenseV31_48frkmn4z8aw4`) o en el firmware ACPI. Alternativas: extraer el paquete UWP de una máquina Windows, inferir por experimentación, o consultar prior art comunitario.

0 intentos de inyección detectados (ninguna string pidió ejecutar/descargar nada).
