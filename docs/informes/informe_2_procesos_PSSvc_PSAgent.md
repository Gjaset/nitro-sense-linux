# Informe 2/3 — Procesos principales: PSAgent.exe / PSSvc.exe (NitroSense 3.1.3056.0)

> Generado por subagente (ses_ead7227b5ffekQPFhIDXmJByjx), análisis estático Ghidra, 21-oct-2026.
> Fuente: FILE_APP_AGENT_EXE y FILE_APP_SVC_EXE extraídos del MSI.

**Contexto**: NitroSense es un rebrand del código de **PredatorSense V3** (namespace C++ `treadstone`, PDB filtrada: `C:\Users\Per key\Desktop\Old Projects\PredatorSense_V3\nitrosense-nb-v3_2019\Bin\Release\`). Ambos PE x64 con image base `0x140000000`. Sha256: AGENT `e3c7821ff88c…`, SVC `e6638fecc417…`.

---

## 1. Import tables resumidas

### FILE_APP_AGENT_EXE (PSAgent.exe, GUI/UX helper, 775 KB)
| DLL | Símbolos clave | Interpretación |
|---|---|---|
| KERNEL32 (98) | `CreateFileW`, `ReadFile`, `WriteFile`, **`PeekNamedPipe`**, `CreateMutexW`, `Wow64DisableWow64FsRedirection`, `LoadLibraryExW`+`GetProcAddress` | cliente de named pipe + carga dinámica |
| USER32 (36) | `RegisterWindowMessageW`, `PostMessageW`, `FindWindowW`, `SetTimer/KillTimer`, `CreateWindowExW`, `EnumDisplaySettingsW`, `keybd_event` | ventana oculta receptora de mensajes + OSD |
| ADVAPI32 (6) | `RegCreateKeyExW`, `RegOpenKeyExW`, **`RegGetValueW`, `RegSetKeyValueW`** | persistencia de hotkeys |
| gdiplus (25) | `GdipLoadImageFromFile`, `GdipDrawImagePointsI`, `GdipDrawString` | OSD con PNG skinneado |
| SHELL32/SHLWAPI/imagehlp | `ShellExecuteW`, `PathRemoveFileSpecW`, `SHGetFolderPathW`, `MakeSureDirectoryPathExists` | lanzar apps, logs |
| WTSAPI32 | `WTSRegisterSessionNotification` | reaccionar a sesión/logon |
| ole32 | `CoInitializeEx`, `CoCreateInstance` | (uso menor) |

**Sin** `DeviceIoControl`, sin WMI directa. El AGENT **no** controla hardware.

### FILE_APP_SVC_EXE (PSSvc.exe, servicio, 850 KB)
| DLL | Símbolos clave | Interpretación |
|---|---|---|
| **SdkWrapper.dll** (7) | `SdkWrapperLibrary::SdkWrapperForNativeCode::{ctor, Initialize, IsSupportOC, GetMonitorValue(uint), GetMaxTurboBoostCPUSpeed, ApplyProfile(wchar_t*)}` | wrapper del SDK Intel XTU (CPU OC, sensores) |
| KERNEL32 (124) | **`CreateNamedPipeW`, `ConnectNamedPipe`, `DisconnectNamedPipe`, `SetNamedPipeHandleState`, `WaitNamedPipeW`, `GetNamedPipeClientProcessId`, `ImpersonateNamedPipeClient`** (importados en ADVAPI32), `ReadFile/WriteFile`; `WTSGetActiveConsoleSessionId`, `WTSQueryUserToken`(WTSAPI32), `CreateProcessAsUserW`, `CreateToolhelp32Snapshot`, `Process32FirstW/NextW`, `ProcessIdToSessionId`, `GetPrivateProfileStringW`, `CreateEventW/QueueUserWorkItem` | servidor pipe + lanzar procesos en sesión de usuario + leer INI |
| ADVAPI32 (36) | `StartServiceCtrlDispatcherW`, `RegisterServiceCtrlHandlerExW`, `SetServiceStatus`, `CreateServiceW/OpenSCManagerW/DeleteService/ChangeServiceConfig2W`, `ControlTraceW`(ETW), `RegisterEventSourceW/ReportEventW`, `RegCreateKeyExW/RegGetValueW/RegSetKeyValueW/RegDeleteTreeW/RegDeleteKeyValueW`, `ConvertStringSecurityDescriptorToSecurityDescriptorW` (SDDL), `DuplicateTokenEx/AllocateAndInitializeSid/SetTokenInformation` | ciclo de vida servicio + registro + seguridad pipe |
| SETUPAPI (4) | `SetupDiGetClassDevsW`, `SetupDiEnumDeviceInfo`, `SetupDiGetDeviceRegistryPropertyW` | enumerar GPUs por HWID |
| USERENV (2) | `CreateEnvironmentBlock/DestroyEnvironmentBlock` | entorno para CreateProcessAsUserW |
| OLEAUT32/ole32 | `CoCreateInstance`, `CoInitializeEx`, **`CoSetProxyBlanket`** | **WMI COM (ROOT\WMI, ROOT\CIMV2)** |

**Sin** `DeviceIoControl`: el hardware NO se toca por IOCTL desde ninguno de los dos binarios. Todo pasa por **WMI** (métodos MOF del BIOS Acer) y NVAPI (resuelta dinámicamente con `nvapi_QueryInterface`, string @`0x1400447a0`).

---

## 2. Flujo de control (quién decide y quién aplica)

```
┌────────────────────────┐   RegisterWindowMessage/PostMessage    ┌──────────────────────┐
│  UWP NitroSense (GUI)  │ ──────────────────────────────────────▶ │ PSAgent.exe          │
│  AcerIncorporated.     │   ("ToastShowingProcessMessage"=OSD,    │  (ventana "PSAGENT"/  │
│  NitroSenseV31_…!App   │    "LaunchAppProcessMessage")           │   "PSAgent")          │
└──────────┬─────────────┘                                        │  • OSD fan/turbo     │
           │ named pipe \\.\pipe\predatorsense_service_namedpipe │  • toasts (lanza      │
           │ [u16 cmd][u8 argc]{[u32 size][data]}*argc            │    PSToastCreator.exe)│
           ▼                                                      │  • hotkeys usuario    │
┌────────────────────────┐      IsAllowClient: allowlist           └──────────────────────┘
│ PSSvc.exe (SYSTEM)     │  UWP: GetPackageFamilyName ∈ {NitroSenseV31_…, PredatorSenseV30_…}
│ treadstone::           │  Win32: QueryFullProcessImageName ∈ {PSAgent/PSAdminAgent/PSLauncher/
│ TsClientCommand        │        PSSvc/PSToastCreator/PSCreateDefaultProfile/NitroSense.exe,
│  Processor (35 cmds)   │        C:\Program Files\WindowsApps\AcerIncorporated.NitroSenseV31_*}
│  • WMI AcerGamingFn    │
│  • WMI APGeAction      │      fallback elevado (ERROR_ACCESS_DENIED):
│  • NvAPI (GPU OC/SLI)  │ ────▶ pipe "PredatorSense_admin_agent_" → PSAdminAgent.exe
│  • SdkWrapper (XTU)    │
│  • Registro HKLM       │
└────────────────┬───────┘
           ▼  WMI ExecMethod, ROOT\WMI
   BIOS/EC: clase MOF "AcerGamingFunction" (SetGamingFanBehavior, SetGamingFanSpeed,
   SetGamingFanTable, SetGamingProfile, SetGamingMiscSetting, …)
```

**Cadena de llamadas clave (SVC)**:
- `WinMain` → `-install/-remove` (`CreateServiceW`/`DeleteService`, tipo "NitroSense Service", trigger-start) → `StartServiceCtrlDispatcherW`
- `OnStart` **FUN_14000a4d0** @`0x14000a4d0`: abre/crea `HKLM\SOFTWARE\OEM\NitroSense` → `QueueUserWorkItem` al pipe server
- Pipe server **FUN_14000a920**: crea instancias con **FUN_1400165a0** (path+SDDL+`CreateNamedPipeW`), `ConnectNamedPipe` → 1 thread por cliente → `process_pipe`
- **process_pipe = FUN_14000c630**: lee 512B por ReadFile (acumula si `ERROR_MORE_DATA`), impersona al cliente (`ImpersonateNamedPipeClient`), valida con **FUN_14000bfa0** (`IsAllowedUwpApp`, token AppContainer) o **FUN_14000c2a0** (`IsAllowedWin32App`, PID→ruta), `RevertToSelf`, parsea el mensaje y despacha con la **tabla de 35 punteros en `0x140053430`**
- Inicialización: `TsClientCommandProcessor::Initialize` **FUN_14000bba0** — WMI version del BIOS (`SELECT * FROM AcerGamingFunction`, "BIOS support WMI version = %d.%02d"), SMBIOS (`MSSmBios_RawSMBiosTables`, estructura **Type 172** propia de Acer: detección de 2 adaptadores AC, USB-PD, LCD Overdrive, GPU conmutable), soporte CPU/GPU OC (`ROOT\CIMV2\Win32_Processor` + INI), NVAPI
- Aplicación de OC: `set_operation_mode` → notify **FUN_140013230** → **FUN_140010a10** (`nvidiaOC_set_oclevel`: NvAPI GetPstates20/SetPstates20) y `set_oc_fan_table` **FUN_140012920** (WMI `SetGamingFanTable`)

**Catálogo de comandos del pipe (ID → handler → acción)** — extraído de la tabla @`0x140053430` + xrefs a strings:

| ID | Handler | Acción |
|---|---|---|
| 0x00 | FUN_14000cd40 | echo/dump de args (log "Number of arguments") |
| 0x01 | FUN_14000d070 | `RegCreateKeyExW` (fallback→admin agent) |
| 0x02 | FUN_14000d210 | `RegDeleteTreeW` |
| 0x03 | FUN_14000d360 | borrar valor (RegDeleteKeyValueW) |
| 0x04 | FUN_14000d560 | `RegDeleteKeyValueW` |
| 0x05 | FUN_14000d700 | comprobar+escribir valor registro |
| 0x06 | FUN_14000d940 | **escribir valor registro** (root por nombre: HKEY_LOCAL_MACHINE/…) |
| 0x07 | FUN_14000dba0 | **lanzar PSAdminAgent.exe** (elevado) |
| 0x08 | (no-op, `_guard_check_icall`) | — |
| 0x09 | FUN_14000dca0 | WMI `SetGamingProfile` |
| 0x0A | FUN_14000df00 | WMI `GetGamingProfile` |
| 0x0B/0x0C | FUN_14000e160/e3c0 | WMI LED color set/get (`SetGamingProfileSetting`/`GetGamingLEDColor`) |
| 0x0D | FUN_14000e620 | WMI `GetGamingSysInfo` |
| 0x0E | FUN_14000e880 | uso GPU (NvAPI `GetDynamicPstatesInfoEx`) |
| 0x0F | FUN_14000ea60 | WMI **`SetGamingFanBehavior`** (modo fan) |
| 0x10 | FUN_14000ecc0 | WMI **`SetGamingFanSpeed`** (velocidad fan) |
| 0x11 | FUN_14000ef20 | WMI `APGeAction.SetFunction` (genérico) |
| 0x12–0x14 | FUN_14000f180/f3e0/fc30 | Logo LED, Logo behavior, `APGeAction.GetFunction` |
| 0x15 | FUN_14000fe90 | NVidia OC get (frecuencias) |
| 0x16 | FUN_140010100 | Intel OC get (XTU: `GetMonitorValue`, max turbo) |
| 0x17 | FUN_1400102d0 | NVidia OC set |
| 0x18 | FUN_140010410 | **Intel OC set** (XTU `ApplyProfile` con .xtu) |
| 0x19 | FUN_140010680 | NVidia SLI set |
| 0x1A | FUN_140010750 | coproc status |
| 0x1B–0x1D | FUN_14000f640/f7f0/fae0 | KB backlight, RGB KB, LED behavior |
| **0x1E** | **FUN_140012c60** | **set_operation_mode → WMI `SetGamingMiscSetting`** |
| 0x1F/0x20 | FUN_140012dc0/12e50 | GPU count / frecuencia (monitores) |
| 0x21/0x22 | FUN_140013500/13650 | WMI misc setting set/get |

El **cliente real del pipe es la app UWP** a través de `TsDotNetLib.dll` (27 KB, el puente .NET; analizada por otro subagente). El propio PSSvc también contiene funciones cliente (FUN_140016f10/17070/17210/173f0) usadas para **reenviar** los comandos de registro 1/2/3/5 al admin agent elevado.

---

## 3. Mecanismo de IPC (exacto)

**Named pipe**: `\\.\pipe\` (string @`0x1400590b0`) + `predatorsense_service_namedpipe` (@`0x1400536a0`).
- `CreateNamedPipeW`: DUPLEX, **PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE** (modo=6), max 255 instancias, buffers 512 B, timeout 1000 ms.
- **SDDL** (@`0x140059140`): `D:(D;OICI;GA;;;BG)(D;OICI;GA;;;AN)(A;OICI;GRGWGX;;;AU)(A;OICI;GA;;;BA)` — deniega invitados/anónimo, RWX para usuarios autenticados, todo para admins. Sin SDDL válido lanza "SD initialization failed"/"DACL creation failed".
- **Request**: `[u16 command (≤0x22)][u8 argc]` + por cada argumento `[u32 size (unaligned, offset 3)][size bytes]`. El handler recibe el buffer + array de offsets de datos.
- **Response**: `[u8 retArgc]` + `[u32 size][data]…` (constructor FUN_14000cc40 @`0x14000cc40`). Típicamente 9 bytes: `[01][04 00 00 00][u32 status]`. Cliente no permitido → 4 bytes de estado de error y desconexión.
- **Autenticación del cliente**: por PID/ruta (`GetNamedPipeClientProcessId`+`QueryFullProcessImageNameW`) o `GetPackageFamilyName` para AppContainer, con allowlists.
- Agente elevado: pipe con prefijo `PredatorSense_admin_agent_` (@`0x1400536e0`).
- Agent→GUI/UWP: **RegisterWindowMessageW** (0x8001 "ToastShowingProcessMessage" = OSD, 0x8002 "LaunchAppProcessMessage", 0x8004 launch UWP) hacia la ventana `PSAGENT/PSAgent`. PSSvc no importa USER32: los mensajes al agente los envían otros procesos del suite (launcher/UWP-vía-helper), no el servicio.

---

## 4. Pseudocódigo de las funciones más relevantes

**(a) `process_pipe` — FUN_14000c630 (protocolo + dispatch)** — resumen:
```c
// un thread por conexión, instancias creadas en FUN_14000a920
process_pipe(conn):
    buf = ""; read pipe 512B chunks (ReadFile, acumula si ERROR_MORE_DATA 0xEA)
    ImpersonateNamedPipeClient(); token = OpenThreadToken()
    if GetTokenInformation(TokenIsAppContainer): allowed = IsAllowedUwpApp(token)   // FUN_14000bfa0
    else:                                             allowed = IsAllowedWin32App(pipe)// FUN_14000c2a0
    RevertToSelf()
    if !allowed: WriteFile(4 bytes de error); return
    if len(buf) < 3: return
    cmd  = u16@buf[0]            // ≤ 0x22
    argc = u8 @buf[2]
    offs[argc]; o = 3            // ← tamaño NO alineado en offset 3
    for i in 0..argc: offs[i]=o+4; size=u32@buf[o]; o += 4+size   // valida límites
    out,outsz = DISPATCH_TABLE[cmd](buf, offs)        // 35 punteros @0x140053430
    if out: WriteFile(handle, out, outsz)             // respuesta al cliente
```

**(b) `set_operation_mode` — FUN_140012c60 (modo de rendimiento)**:
```c
set_operation_mode(msg, offs, out, outsz):
    mode = (argsize[0] < 5) ? u32@arg0 : 0
    wmi_in = (mode << 8) | 0x0B                  // ← codificación del comando WMI
    log("WMI SetGamingMiscSettingFunction input: 0x%I64X", wmi_in)
    r = WmiExec(ROOT\WMI, "AcerGamingFunction", "SetGamingMiscSetting", in=wmi_in) // FUN_1400152e0
    log("… output: 0x%I32X", r)
    if mode==0: notify=0; elif mode==4: notify=2; else notify=1
    FUN_140013230(notify)      // → nvidiaOC_set_oclevel + set_oc_fan_table si hay soporte OC
    respond(1, u32 r)
```

**(c) `gaming_fan_group_speed_set_function` — FUN_14000ecc0 (velocidad de fan; `behavior` @FUN_14000ea60 es idéntico con `SetGamingFanBehavior`)**:
```c
fan_speed_set(msg, offs, out, outsz):
    in = (argsize[0] < 9) ? u64@arg0 : 0     // 64 bits construidos por el cliente
    variant_in = {(param_id)0x15, in}        // parámetro de entrada del método WMI
    r = WmiExecMethod(ROOT\WMI, "AcerGamingFunction", "SetGamingFanSpeed",
                      "SELECT * FROM AcerGamingFunction", in=u64, out=u32)  // FUN_1400148c0
    respond(1, u32 r)   // HRESULT/código del firmware
```

**(d) `set_oc_fan_table` — FUN_140012920 (tabla de curva de fan ↔ nivel OC)**:
```c
set_oc_fan_table(level):        // llamada internamente (no por pipe)
    table = level==0 ? 1 : level==1 ? 2 : level==2 ? 3 : 0     // nivel OC → fan table 1/2/3
    r = WmiExecMethod("AcerGamingFunction","SetGamingFanTable", in=table)
    // (a) error → reintento con variante; lee output u32 si el método devuelve datos
```

**(e) Lado AGENT — OSD de fan/turbo, FUN_140005f20** (resumen):
```c
OSD_handler(hwnd, msg=0x8001, wParam=osd_type, lParam=osd_state):
    dpi = 100|125|150|200 (EnumDisplaySettingsW.dmLogPixels)
    switch osd_type:
      0,1: img = fan set;  state 0/1/2 → LoadStringW 0x69/0x6A/0x6B
      2:   img fan custom; state → 0x6C/0x6D/0x6E
      3,5: nombre desde HKLM\...\KeyAssignment\OSDNameTemp
      4,6,7: turbo → 0x70/0x6F   // imágenes: img_osd_fan_auto/max/custom.png, img_osd_cpu/gpu_turbo.png
    mostrar ventana GDI+ abajo-centro; SetTimer(3000ms) → ocultar
```

---

## 5. Persistencia y datos de perfiles

- **Registro HKLM** (el pipe expone escritura arbitraria con cmd 0x01–0x06 — así escribe la UWP sin elevación): `SOFTWARE\OEM\NitroSense` (raíz), `\Log`, `\KeyAssignment` (hotkeys: `Group%d\userDefineName%d`, y `OSDNameTemp`), `\AdvanceSettings`, `\Overclock` (`CPU_OC_Support`, `GPU_OC_Support` como DWORD). Raíces admitidas por nombre: HKLM/HKCU/HKCC/HKCR/HKU.
- **INI** `C:\ProgramData\oem\NitroSense\NitroSense.ini` (`GetPrivateProfileStringW`): sección `[OC_CPU]` (`Count`, `CPUName%d`, `Path`) y `[OC_GPU]` (`Count`, `GPUName%d`, **`Quiet`/`Default`/`Performance`** MHz core, **`RAM_Quiet`/`RAM_Default`/`RAM_Performance`** MHz de memoria).
- **Perfiles CPU OC Intel XTU**: `C:\ProgramData\oem\NitroSense\Normal.xtu`, `Fast.xtu`, `Turbo.xtu` (aplicados vía `SdkWrapperForNativeCode::ApplyProfile(ruta)` — la ruta completa pasa como `wchar_t*`).
- **Pistas de comandos al hardware**: nivel OC 0/1/2 → **fan table WMI 1/2/3**; modo operación → `(mode<<8)|0x0B` (`SetGamingMiscSetting`); modos con modo 4 especial (notify 2). Los 64 bits de `SetGamingFanSpeed`/`SetGamingFanBehavior`/`SetGamingProfile` los construye el **cliente** (TsDotNetLib/UWP) — el formato bit-a-bit (grupo de fan, nivel 0–100, auto/custom) hay que extraerlo de esa DLL. Umbrales de seguridad en el AGENT (toasts): OC se detiene con batería <30%, se reanuda a ≥40%, y con "conecta ambos adaptadores CA"/USB-C PD detectado.
- **Logging**: `%PROGRAMDATA%\OEM\NitroSense\Log\%s\%s_%s.log` con prefijos `[INFO]/[WARNING]/[ERROR]/[EXCEPTION]`; además sesión ETW propia "PS CS ETW Monitor" (ControlTraceW).

---

## 6. Limitaciones

1. Análisis 100% estático (Ghidra, sin PDB); nombres de función son `FUN_*` salvo los reconstruidos por strings de log (alta confianza por los logs `treadstone::` que nombran cada método).
2. El layout de bits de los u64 de fan/profile no está en estos dos binarios: se empaqueta en el cliente (TsDotNetLib.dll / app UWP) o en SdkWrapper.dll (objetivo de otro subagente).
3. No confirmé qué proceso postea los mensajes OSD a PSAgent (PSSvc no importa USER32; probablemente PSAdminAgent/PSLauncher o la propia UWP) — requiere analizar esos binarios.
4. La clase WMI `AcerGamingFunction`/`APGeAction` es MOF del BIOS: los IDs de método (0x0B=SetGamingMiscSetting, 0x15=param de entrada) se infieren del código; el resto del MOF hay que extraerlo del namespace ROOT\WMI en una máquina real (o del módulo ACPI/DSDT del firmware).
5. El server MCP REA es compartido entre subagentes y cambió de documento activo a mitad de análisis; mitigué con verificación de `local_path` en cada resultado y con el CLI `rea` + snapshots (los snapshots se perdieron con el reinicio del servidor, pero los hallazgos ya estaban capturados).
6. Ghidra marca el handler del cmd 0x08 como `_guard_check_icall` — slot no usado/no-op, no un comando real.

## 7. Implicaciones para el puerto a Linux

- **La lógica de control es delgada y reutilizable**: el "cerebro" es el enum de 35 comandos + el despachante; el estado persiste en registro+INI. Un daemon Linux puede replicar el protocolo pipe (o sustituirlo por D-Bus/un socket UNIX) 1:1.
- **El control de fans/modos NO usa IOCTL ni EC directo**: todo va por **WMI ACPI** (`AcerGamingFunction.SetGamingFanSpeed/Behavior/Table/Profile/MiscSetting`). En Linux, el equivalente es el driver `acer-wmi`/`platform_profile` (o un módulo WMI custom con `wmi_evaluate_method` sobre el mismo GUID MOF) — **es la pieza crítica a portar**; conviene confirmar el GUID y el formato de los buffers con `bmof`/`wmi-dump` del firmware del equipo concreto.
- **Modos de rendimiento** = `SetGamingMiscSetting` con `(mode<<8)|0x0B` (4 modos: 0..4, con el 4 especial). En Linux esto mapea naturalmente a `platform_profile` (quiet/balanced/performance) si el firmware lo expone por WMI.
- **Fan tables** ligadas a niveles OC (1/2/3): replicable con curvas propias; la parte XTU (CPU OC) depende del SDK Intel XTU (inexistente en Linux) — sustituir por `msr`/`intel-undervolt` o simplemente omitir OC CPU.
- **GPU OC** por NVAPI (`SetPstates20`, deltas en kHz): en Linux hay que usar NVML/`nvidia-smi -lgc` (limitado) — no es portable al 100%.
- **UX**: el OSD/toasts del AGENT (PNG + GDI+) se puede rehacer con cualquier toolkit; los mensajes ventana se sustituyen por D-Bus signals.
- **Seguridad**: la validación de cliente del pipe (allowlist por PID/paquete) es trasplantable a un socket UNIX con `SO_PEERCRED` + cgroup/flatpak ID; el diseño "GUI sin privilegios + daemon privilegiado + helper elevado" es exactamente el patrón correcto para Linux (systemd service + polkit).
- Nota de seguridad: los comandos 0x01–0x07 del pipe permiten **escritura/borrado arbitrario en el registro** a cualquier proceso de la allowlist — en el puerto conviene acotar a claves prefijadas (`SOFTWARE\OEM\NitroSense`), no exponer registro genérico.
