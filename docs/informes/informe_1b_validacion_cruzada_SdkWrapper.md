# Informe 1b — Validación cruzada: SdkWrapper + ProfileHelperModel (detalle de exports y IL)

> Generado por subagente (ses_ead9... → ses_e9ff4b0e3ffe5jaKM0k6Wqyp2i), 21-oct-2026.
> Confirma el informe 1 y añade el detalle IL completo de los 11 exports + el hallazgo clave del u64.

## 0) Identidad de los artefactos

| | FILE_APP_SDK_WRAPPER_DLL | FILE_APP_PROFILE_HELPER_DLL |
|---|---|---|
| Ensamblado | `SdkWrapper` v1.0.9610.29350 (FileProduct 3.1.3056.0, ProductName **NitroSense**) | `ProfileHelperModel` v6.4.1.25 (strong-name, token `251acf66a09dfdd3`) |
| MVID | `89bca823-8479-4467-bd39-e66b8384909a` | `bb311f0d-e4d6-4341-9870-0fb758ecff3c` |
| Tipo | PE32+ x64, **C++/CLI mixto**, .NET 4.5 | PE32 i386, CIL, **ofuscado con ConfuserEx** |
| SHA-256 | `0f3d8ab47fb5346e6d11ab561f7fdbe49b0db2f02c1954274c984d9c331ff284` | `7739224b91aac0347cfccab050560be84e3c55c95c6694661ec5b0877011633e` |
| PDB | `...Old Projects\PredatorSense_V3\nitrosense-nb-v3_2019\Library\lib\x64\SdkWrapper.pdb` | n/a |

## 1) API pública completa del SDK Wrapper (11 exports nativos → IL decompilado)

Clase C++ `SdkWrapperLibrary::SdkWrapperForNativeCode` (TypeDef `0x020001cc`):

| Export (ordinal/RVA) | Método | Firma | Semántica (IL) |
|---|---|---|---|
| 9 / 0x7030 | `Initialize` | `void()` | `GetGlobalOcLibraryHandle()` → `IntelOverclockingLibrary.Initialize()`; singleton lazy |
| 7 / 0x7040 | `GetMonitorValue(u4)` | `double` | `IMonitoringLibrary.GetValue(id)` → `Decimal.ToDouble` (IDs = XTU SDK) |
| 5 / 0x7050 | `GetCpuPhysicalCoreCount()` | `unsigned` | `IProcessorInfo.GetPhysicalCpuCoreCount()` |
| 10 / 0x7060 | `IsSupportOC()` | `bool` | `IProcessorInfo.IsOverclockSupported()` |
| 11 / 0x7070 | `Tune(u4 cmd, u4 value, bool&)` | `bool` | `ITuningLibrary.Tune(cmd, value, out applied)` — **paso directo sin transformación** |
| 4 / 0x7080 | `ApplyProfile(wchar_t*)` | `bool` | `ImportProfile(ruta,"")` → `GetProfile("")` → `UpdateProfile` → `ProposeProfile` (valida) → `ApplyProfile` |
| 8 / 0x7100 | `GetServiceVersion(u4, wchar_t*)` | `bool` | `IOverclockingServiceInfo.GetOverclockingServiceVersion()` → buffer |
| 6 / 0x7090 | `GetMaxTurboBoostCPUSpeed()` | `double` | `GetAvailableControls()` → busca `"Max Turbo Boost CPU Speed"` → `ActiveValue` |
| 1/2/3 | ctor/dtor/copia | — | triviales |

Enums `IdentifyOCLevel`/`OCCmd` existen en metadata **sin miembros emitidos** (C++ nativos); los valores los aporta el llamador (PSSvc) a `Tune`.

**Sin WMI, sin EC, sin DeviceIoControl** (imports: KERNEL32, VCRUNTIME140, crt, USER32 `UnregisterClassW`, OLEAUT32 ord#150/149/10, mscoree `_CorDllMain`).

## 2) Empaquetado del u64 de SetGamingFanSpeed — NEGATIVO con redirección ★

**No está en este binario.** Evidencia: 2928 strings ASCII + 71 UTF-16 — cero ocurrencias de `AcerGamingFunction`, `APGeAction`, `root\wmi`, `SetGaming*`, GUIDs `{...}` WMI. El wrapper es fachada exclusiva del Intel XTU OC SDK.

**Dónde vive realmente** (scan de strings sobre los 20 binarios extraídos): `SetGamingFanSpeed`/`AcerGamingFunction` aparecen SOLO en:
1. **`FILE_APP_ADMINAGENT_EXE`** — `SELECT * FROM AcerGamingFunction`, `SELECT * FROM APGeAction`, `SetGamingFanSpeed` ← **candidato #1 para el bit-packing del u64**
2. **`FILE_APP_CREATE_DEFAULT_PROFILE_EXE`** — `SELECT * FROM AcerGamingFunction`, `SELECT * FROM APGeAction` (cliente secundario)
3. `FILE_APP_SVC_EXE` — lado servidor (ya analizado)

→ Siguiente análisis objetivo: decompilar en ADMINAGENT (y CREATE_DEFAULT_PROFILE) las funciones que llaman a `SetGamingFanSpeed`; ahí están máscaras/shifts del u64 (grupo de fan, nivel 0-100, flag auto/custom) y del variant con param_id `0x15`/`0x0B`.

## 3) GUIDs y constantes encontradas (ninguno WMI)

| Valor | Dónde | Nota |
|---|---|---|
| `A7A0ABAF-B733-4F2F-B85D-F88901F6FDF7` | PROFILE_HELPER, `GuidAttribute` ComVisible | GUID COM del ensamblado |
| `cb2f6723-ab3a-11d2-9c40-00c04fa30a3e`, `cb2f6722-…`, `90f1a06c-…`, `90f1a06e-…` | SDK_WRAPPER, campos `_GUID_*` | ATL/mscoree internos |

## 4) FILE_APP_PROFILE_HELPER_DLL — hallazgos

ConfuserEx (nombres con bidi, cuerpos IL cifrados en reposo, `VirtualProtect` anti-tamper, `Assembly.Load(byte[])` + handler `AssemblyResolve`). API visible:

- `ProfileHelperModel.Profile.IProfilesModel`: `ExportToFile(string,string)`, `ImportFromFile(string,XmlReaderSettings)`, `VerifyHashFromXTU(string)`, `GetProfileName()`
- `ProfileModelData`: propiedades `byte[] Key/Vector/OldKey/OldVector` (AES key+IV con par "Old" legacy)
- Behavior inferido de member-refs: **AES** (AesCryptoServiceProvider, set_BlockSize/Key/IV, CreateEncryptor/Decryptor, CryptoStream) de XML `.xtu`, escritura a `%TEMP%`, **integridad SHA-256/MD5 con nodo hash en el XML**. Dado un perfil NitroSense en disco, lo descifra y produce el `.xtu` en claro para `ApplyProfile`.
- **Nota (discrepancia con informe 1)**: los valores de Key/Vector se computan en runtime por el desencriptador de constantes de ConfuserEx (0 literales #US legibles). Los literales `ye5r3uzjp9qe9hdzp6cks992`/`rjpkc345jhdytndir` del informe 1 requieren verificación con dump en runtime; no son legibles estáticamente desde este binario.

## 5) Limitaciones

1. C++/CLI mixto: 11 métodos managed decompilados; código nativo restante = ruido ATL/CRT.
2. ConfuserEx: no se ejecutó nada (riesgo anti-análisis); Key/Vector no extraíbles estáticamente.
3. Semántica de `nMonitorId`/`cmd` de `Tune` → definida por Intel XTU OC SDK (`FILE_APP_INTEL_OC_SDK_DLL`, enums `MonitorId`/`Tune`) — análisis separado pendiente.
4. Sin strings-injection detectadas.

## 6) Implicaciones para el porte a Linux

- **Fans/LED/modos**: SdkWrapper NO participa. El u64 se construye en los **clientes WMI** (ADMINAGENT + CREATE_DEFAULT_PROFILE) → decompilar esos = bit-packing completo sin necesidad de la UWP. Después: `wmi_evaluate_method()` sobre la misma clase WMI.
- **OC CPU**: fachada al Intel XTU (Windows) → en Linux: MSR rapl/PL1-PL2, `intel_uncore_frequency`, undervolt MSR 0x150, u omitir OC.
- **Perfiles**: XML `.xtu` cifrado AES + nodo hash SHA-256 → definir formato propio (o extraer clave en runtime).
- **Monitoreo**: `GetMonitorValue` = polling XTU → Linux: `/sys/class/hwmon` + MSR con mapeo propio.
