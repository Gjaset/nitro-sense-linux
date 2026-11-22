# Informe 1/3 — Capa de acceso a hardware: SdkWrapper.dll + ProfileHelperModel.dll (NitroSense V3.01.3056)

> Generado por subagente (ses_ead7227b6ffemi5V1it3sNQtZQ), 21-oct-2026.
> Archivos: FILE_APP_SDK_WRAPPER_DLL, FILE_APP_PROFILE_HELPER_DLL (verificación cruzada con FILE_APP_DOTNETLIB_DLL).

---

## 0. Identidad de los binarios analizados

| Archivo | Identidad real | Tipo |
|---|---|---|
| `FILE_APP_SDK_WRAPPER_DLL` | **SdkWrapper.dll** v1.0.9610.29350 (recursos: "NitroSense" 3.1.3056.0, Acer Inc., cert EV GlobalSign, SHA-256 `0f3d8ab4…331ff284`) | PE32+ **mixed C++/CLI** (ILONLY=0), .NET 4.5 |
| `FILE_APP_PROFILE_HELPER_DLL` | **ProfileHelperModel.dll 6.4.1.25** — componente de **Intel(R) Extreme Tuning Utility** (rsrc: "Intel(R) Corporation", "Copyright 2007-2018 Intel"), ofuscado con **ConfuserEx v0.4.0-custom** | PE32 i386, .NET CIL |
| (auxiliar) `FILE_APP_DOTNETLIB_DLL` | **TsDotNetLib.dll** 3.1.3056.0 — biblioteca IPC de NitroSense (el mismo IL está embebido dentro de SdkWrapper.dll) | PE32+ .NET puro |

Ruta PDB filtrada (codebase compartido PredatorSense/NitroSense):
`C:\Users\Per key\Desktop\Old Projects\PredatorSense_V3\nitrosense-nb-v3_2019\Library\lib\x64\SdkWrapper.pdb` (y `…\TsDotNetLib\obj\x64\Release\TsDotNetLib.pdb`)

## 1. Mecanismo de acceso a hardware — CONCLUSIÓN PRINCIPAL

**NitroSense NO toca el hardware directamente en esta capa.** En estos binarios **no existe ningún** GUID WMI, `DeviceIoControl`, IOCTL, acceso a puertos EC, MSR, InpOut/WinRing, ni ruta `\\.\…` (verificado: tabla de imports PE, ImplMap/PInvoke, heap #US y #Strings completos). Arquitectura real de 3 niveles:

```
UI (NitroSense.exe)
 ├─ SdkWrapper.dll ──► IntelOverclockingSDK.dll (Intel XTU SDK 6.2.0.10) ──► servicio/driver Intel XTU   [OC CPU, monitoreo]
 └─ TsDotNetLib.IPCMethods ──► named pipe "PredatorSense_service_namedpipe" ──► Servicio NitroSense (PSSvc.exe)
                                   └─ el servicio ejecuta WMI real (fans/LED/KB/modos) + registro HKLM
```

### 1.1 Evidencia A: SdkWrapper.dll delega en Intel XTU (no hay I/O propio)
- Imports nativos PE: solo `KERNEL32.dll` (37 funciones genéricas), `VCRUNTIME140.dll`, CRT, `USER32!UnregisterClassW`, `OLEAUT32` (3 ordinales), `mscoree!_CorDllMain`. **Cero APIs de dispositivo.**
- P/Invoke (ImplMap, 33 entradas): solo runtime C++/CLI (`DecodePointer`, `EncodePointer`, CRT thunks). **Nada de hardware.**
- AssemblyRefs: `mscorlib`, **`IntelOverclockingSDK 6.2.0.10`**, `ProfileHelperModel 6.2.0.10`.
- Tipos/metadatos que envuelven XTU (namespace `SdkWrapperLibrary` + TypeRefs):
  - `IntelOverclockingLibrary` (ns `Intel.Overclocking.SDK`), `ClientTuningControl` (`Intel.Overclocking.SDK.Tuning`) con `ITuningLibrary`
  - `XtuTuningProfile` (`Intel.Overclocking.SDK.Profile`), `IProfileLibrary`, `ImportState`, métodos `ApplyProfile`, `ProposeProfile`, `UpdateProfile`, `GetProfile`, `ImportProfile`, `GetAvailableControls`
  - `IOverclockingServiceInfo` (`Intel.Overclocking.SDK.ServiceInfo`), `IServiceInfoLibrary`, `GetOverclockingServiceVersion`, `IsOverclockSupported`
  - `IProcessorInfo`/`GetPhysicalCpuCoreCount`; `IMonitoringLibrary` (`Intel.Overclocking.SDK.Monitoring`)/`GetValue`
  - Helper propio: `SdkWrapperLibrary.IntelOverclockingInterfaceHelper` (3 métodos, 1 campo), enums `IdentifyOCLevel`, `OCCmd`, global nativo `GetGlobalOcLibraryHandle`
  - String de monitoreo en #US `0x70000003`: **"Max Turbo Boost CPU Speed"**

### 1.2 Evidencia B: el canal real de control de fans/LED/modos es IPC por named pipe + registro
`TsDotNetLib` (IL embebido en SdkWrapper.dll e idéntico en FILE_APP_DOTNETLIB_DLL; verificado con parse crudo ECMA-335):

**Pipe del servicio** (tabla Constant):
- `ConstDef.ServicePipeName = "PredatorSense_service_namedpipe"`
- `ConstDef.AdminAgentBasePipeName = "PredatorSense_admin_agent_"` (sufijo dinámico)

**Registro** (HKLM, vía `Microsoft.Win32.Registry.LocalMachine.OpenSubKey/GetValue/…`):
- `PS_REG_PATH = SOFTWARE\OEM\NitroSense`
- Subclaves: `…\KeyAssignment`, `…\LightSetting`, `…\AdvanceSettings`, `…\ColorRecord`, **`…\FanControl`**, `…\Overclock`

**Formato del protocolo del pipe** (ConstDef, valores exactos):
- CMF (comando): `[cmd:2 bytes @offset 0][numArgs:1 byte @2][argSize:4 bytes ×N][datos…]` (`CMF_CMD_INDEX=0, CMF_CMD_SIZE=2, CMF_NUM_ARG_INDEX=2, CMF_NUM_ARG_SIZE=1, CMF_ARG_SIZE_SIZE=4`)
- RMF (respuesta): `[numArgs:1 byte @0][argSize:4 bytes ×N][datos…]`

**Enum `ServiceCommand` completo (35 valores, tabla Constant):**
```
0x00 EchoTest      0x01-0x06 Reg Create/Delete/Get/SetKey/Value     0x07/0x08 CreateProcess(AsUser|OnLogon)
0x09/0x0A GamingProfile-WMI Set/Get        0x0B/0x0C GamingLEDGroupColor Set/Get
0x0D GetGamingSysinfo     0x0E GetGPUUsageLoading
0x0F GamingFanGroupBehaviorSet     0x10 GamingFanGroupSpeedSet      ◄◄ CONTROL DE VENTILADORES
0x11 WMISetFunction       0x12 WMISetLogoLED      0x13 WMISetLogoBehavior
0x14 WMIGetFunction       0x15 NvidiaOCGet        0x16 IntelOCGet
0x17 NvidiaOCSet          0x18 IntelOCSet         0x19 NvidiaSLISet
0x1A NvidiaCoprocStatus   0x1B WMISetGamingKBBacklight   0x1C WMISetGamingRgbKbSetting
0x1D WMISetGamingLEDBehavior   0x1E SetOperationMode  0x1F GPUGetCount
0x20 GPUGetFrequency      0x21 WMISetGamingMISCSetting   0x22 WMIGetGamingMISCSetting
```
Otros enums: `PSUICommand` 0x8001-0x8010 (UICMD_MODE, UICMD_HOTKEY_TRIGGER_FAN/CPU_OC/GPU_OC, UICMD_NV_WHISPERMODE_CHANGE…), `AgentCommand` 0x8001-0x8005, `AdminAgentCommand` 0x00-0x0F (USB keyboard color/pattern, DTS/Waves audio, sticky keys…), `EVENT_TYPE` (DUST_CLEAN, MODE_CHANGED…), `RadioClass` (Airplane/WLAN/BT/WWAN/GNSS/NFC).

Métodos IPC en `TsDotNetLib.IPCMethods`: `SendCommandByNamedPipe(string pipe, int cmd, object[])`, `SendCommandByWindowMessage(string cls, string title, uint msg, uint wParam, int lParam)` con P/Invoke `FindWindow`/`PostMessage` (user32 — los únicos P/Invoke reales del sistema), más serialización `AnyToBytes`/`BytesToStruct<T>`/`GetSizeOnMemory` (Marshal/GCHandle).

### 1.3 Evidencia C: ProfileHelperModel (Intel) — perfiles XTU cifrados
`IProfilesModel` (ns `ProfileHelperModel.Profile`): `ExportToFile(filePath, xmlDoc)`, `ImportFromFile(filename, settings)`, `VerifyHashFromXTU(xtuMd5HashCode)`, `GetProfileName`; clase `ProfileModelData`; propiedades `Key/Vector/OldKey/OldVector`. Cifrado de perfiles:
- **AES** (`AesCryptoServiceProvider`, `set_BlockSize/Key/IV`, `CreateEncryptor/CreateDecryptor`, CryptoStream) con literales: clave **`ye5r3uzjp9qe9hdzp6cks992`**, IV **`rjpkc345jhdytndir`**, elemento XML **`Authentication`**, namespace **`http://intel.com/xtu/xtuProfileSchema`**, formato de nombre `{0:yyyy-MM-dd_hh-mm-tt}`
- Integridad: `MD5CryptoServiceProvider` + `SHA256Managed` + `Convert.ToBase64String`
- Ofuscación ConfuserEx (nombres con caracteres bidi, anti-tamper con `VirtualProtect`, hook `AssemblyResolve`)

## 2. Mapa de la API pública del SDK wrapper (SdkWrapper.dll)

**Exports nativos C++ (tabla export PE — 11 símbolos, clase `SdkWrapperLibrary::SdkWrapperForNativeCode`):**
```cpp
void   Initialize();
bool   ApplyProfile(wchar_t* pszProfilePath);
bool   Tune(unsigned int nControlId, unsigned int value, bool& bResult);   // controles XTU
double GetMonitorValue(unsigned int nMonitorId);                          // sensores
unsigned int GetCpuPhysicalCoreCount();
double GetMaxTurboBoostCPUSpeed();
bool   GetServiceVersion(unsigned int nTcharLength, wchar_t* pszVersion);
bool   IsSupportOC();
// + ctor/dtor/operator=
```
**Superficie managed**: `SdkWrapperLibrary.IntelOverclockingInterfaceHelper` (envuelve las librerías XTU), enums `SdkWrapperLibrary.IdentifyOCLevel`, `SdkWrapperLibrary.OCCmd`, y el IL embebido de `TsDotNetLib` (ConstDef/Log/IPCMethods/Registry + enums de protocolo). Este wrapper es una facade de XTU para el proceso de UI; fans/LED van por pipe al servicio.

## 3. Limitaciones del análisis
- **El servicio (`FILE_APP_SVC_EXE`), el Agent y AdminAgent no se analizaron en este informe** (cubiertos por el informe 2): ahí vive el acceso WMI real. El GUID WMI/MOF de Acer **no está en estos binarios**; debe obtenerse del firmware/DSDT del equipo (el informe 3 lo confirma también desde el servicio: solo el nombre de clase `AcerGamingFunction` viaja en el WQL).
- SdkWrapper es mixed-mode: los cuerpos nativos de `ApplyProfile/Tune` no se descompilaron; la delegación a XTU se infiere de exports + interfaces/strings + `GetGlobalOcLibraryHandle` (confianza alta, no verificación instruction-by-instruction).
- `FILE_APP_PROFILE_HELPER_DLL` es componente de **Intel** (no helper de Acer); protegido con ConfuserEx → análisis limitado a metadatos/heaps estáticos (suficiente para API y cifrado).
- El servidor MCP REA se reinició a mitad de sesión; todo hallazgo clave fue re-verificado con parse crudo del metadato (offsets #US idénticos).
- Sin prompt-injection detectado en strings.

## 4. Implicaciones para un puerto a Linux

1. **Lo replicable NO es un driver EC**: el control de fans/LED/modos viaja por WMI del firmware ACPI (invocado por el servicio de Acer). El camino correcto en Linux es identificar el **bloque WMI de Acer** en el DSDT del equipo (`acpidump -b | iasl -d`, buscar el device WMI con métodos `*Gaming*`) y llamarlo con el driver `wmi` del kernel (como hace `acer-wmi`) o desde userspace.
2. **El protocolo IPC documentado** (pipe `PredatorSense_service_namedpipe`, formato CMF/RMF y enum ServiceCommand completo) es una especificación funcional lista para implementar un daemon Linux equivalente — o para saltarla y llamar WMI directamente. Los enums dicen exactamente *qué* operaciones existen: velocidad y comportamiento de fans **por grupo**, LED por grupo, backlight KB RGB, logo LED, modo de operación, OC Intel/NVIDIA, frecuencia GPU, sysinfo.
3. **OC de CPU (Intel XTU)**: sin equivalente en Linux (XTU SDK es servicio Intel con driver propio); solo parcialmente accesible por MSR (undervolt/pllimit). **OC NVIDIA**: replicable con NVML. **Fans/LED/modos**: 100% replicable vía WMI ACPI — el objetivo de mayor valor.
4. **Registro → Linux**: `HKLM\SOFTWARE\OEM\NitroSense\{FanControl,Overclock,LightSetting,KeyAssignment,AdvanceSettings,ColorRecord}` mapea a configuración propia (`/etc/nitro-sense/` o `sysfs` con clase `platform_profile`).
5. **Perfiles XTU**: el formato ya está documentado — XML con namespace `http://intel.com/xtu/xtuProfileSchema`, cifrado AES con la clave/IV embebidas, elemento `Authentication` con hash MD5/SHA256 — todo implementable en Linux sin el binario de Intel.
