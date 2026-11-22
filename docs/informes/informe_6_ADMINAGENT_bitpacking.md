# Informe 6 — Lado cliente: bit-packing exacto del u64 (ADMINAGENT + CREATE_DEFAULT_PROFILE)

> Subagente ses_e9e6aec50ffeVydIJftwnfNx1K, 22-oct-2026. REA/Ghidra + CIL + validación cruzada con acer-wmi.c.
> Binarios: FILE_APP_ADMINAGENT_EXE (PDB PSAdminAgent.pdb) y FILE_APP_CREATE_DEFAULT_PROFILE_EXE (PSCreateDefaultProfile.exe).

## 1. SetGamingFanSpeed — bit-packing EXACTO (FUN_140013a80 → wrapper FUN_14001ce20, VARIANT vt=0x15, ExecMethod "AcerGamingFunction","SetGamingFanSpeed")

Ensamblador verificado:
```asm
; CPU: SHL RCX,8; OR RCX,0x1    (pct<<8)|1
; GPU1: SHL RCX,8; OR RCX,0x4   (pct<<8)|4
```

| Bits | Campo | Valores |
|---|---|---|
| [7:0] | ID fan | 0x01=CPU, 0x04=GPU1 |
| [15:8] | Velocidad % | 0-100 (DWORD registro, sin clamp en ADMINAGENT) |
| [63:16] | no usados | 0 |

`= ((u64)percent << 8) | fan_id` — **idéntico al firmware WMBH (informe 5)** ✓

## 2. SetGamingFanBehavior — bit-packing EXACTO (wrapper FUN_14001cc80)

```asm
; CurrentFanMode 0 → EBX=0x00410009 ; 1 → 0x00820009
; modo 2 (custom): por fan CustomAuto==1→AUTO(1) else CUSTOM(3), OR al bitmap<<16, OR 0x9
```

| Bits | Campo | Valores |
|---|---|---|
| [0] | bitmap fan CPU | 1 |
| [3] | bitmap fan GPU | 1 |
| [15:0] | bitmap fans afectados | 0x0009 = CPU+GPU |
| [17:16] | Modo CPU (2 bits) | 01=AUTO, 10=TURBO, 11=CUSTOM |
| [23:22] | Modo GPU (2 bits) | igual |

```c
u64 fan_behavior(cpu_mode, gpu_mode) = 0x9 | (cpu_mode << 16) | (gpu_mode << 22);
// NitroSense: modo 0 (Auto) = 0x00410009; modo 1 (CoolBoost/Turbo) = 0x00820009;
//             modo 2 (Custom) = 0x00C30009 etc. (percentages aplicados ANTES vía SetGamingFanSpeed)
```
GetGamingFanBehavior (15): in = bitmap u32; out: cpu=[9:8], gpu=[15:14], status=[7:0].

## 3. Resto de métodos WMI de ADMINAGENT

| Método | gmInput |
|---|---|
| SetGamingProfile(1) | `2` o `0x1000002` (bit 24 = WinAndMenuKey) |
| GetGamingSysInfo(5) | `2` (constante) |
| SetGamingKBBacklight(20) | `((brightness−1)*25) << 16` |
| SetGamingLED(2) | `(4 flags de 1 bit << 40) \| 8` |
| SetGamingRgbKb(6) | RGB en bytes [23:16]/[31:24]/[39:32] + zona en [3:0] |
| APGeAction.GetFunction | `(BK_Hotkey_Number << 8) \| 0x80001` |

- APGeAction.SetFunction NO existe en ADMINAGENT; SetGamingMiscSetting tampoco (lo manda UWP/PSSvc — consistente con informe 2).

## 4. CREATE_DEFAULT_PROFILE (.NET)

- `WMIAcerGamingFunction(method, input)`: \root\wmi → SELECT * FROM AcerGamingFunction → `inParams["gmInput"] = (ulong)input` → InvokeMethod → `outParams["gmOutput"]`. **Confirma nombres de parámetros WMI: gmInput/gmOutput.**
- `GetGamingProfile(0)` → LCD_Overdrive_support; `GetGamingMiscSetting(9)` → dgpu_switchable (índice 9 no documentado en kernel).
- Defaults del registro (SetInitialReg): `FanControl: CurrentFanMode=0, CPUFanCustomAuto=1, GPU1FanCustomAuto=1, CPUFanPercentage=50, GPU1FanPercentage=50, CoolBoostMode=0`; `AdvanceSettings: Degree=0, StickyKey=0, WinAndMenuKey=1`; `Overclock: CurrentCPUMode=0, CurrentGPUMode=0, CPU_OC_Support=0, GPU_OC_Support=0, CurrentOperationMode=1`.
- Arranque por defecto = CurrentFanMode 0 → SetGamingFanBehavior(0x00410009) con 50/50 pre-enviados.

## 5. Validación triple (binario + kernel + firmware)

acer-wmi.c mainline (WMID_GUID4): method 14/15/16/17/22/23; CPU_FAN=0x01, GPU_FAN=0x04; AUTO=1, TURBO=2, CUSTOM=3; máscaras FAN_BEHAVIOR set_cpu=[17:16], set_gpu=[23:22]; FAN_SPEED value=[15:8]; MISC index=[7:0], value=[15:8], index 0x0B=platform-profile ✓. **Todo coherente con informes 2 y 5 al 100%.**

## 6. Limitaciones

1. Semántica UI de modos 0/1 inferida (Auto/CoolBoost) — verificar en vivo.
2. "Custom auto" (CustomAuto=1 → AUTO) indistinguible por el u64; si hay curva custom, vive en la app (polling) o en el EC.
3. Sin clamp 0-100 en ADMINAGENT (Linux/kernel ya clampa).
4. Behavior con bitmap sin bits de modo (`0x9`) — comportamiento EC no verificado.
5. Índice 9 de GetMiscSetting (dGPU switchable) — función exacta sin confirmar.
6. Solo fans CPU y GPU1 en este paquete.

## 7. Pseudocódigo para el driver (listo)

Ver sección 7 del informe original — coincide con lo implementado en nitro_wmi.c v0.2:
`agf_fan_speed_in(fan,pct) = (pct<<8)|fan`; `agf_fan_behavior_in(cpu,gpu,cm,gm) = bitmap | cm<<16 | gm<<22`.
