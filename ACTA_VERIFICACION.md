# ACTA DE VERIFICACIÓN EN HARDWARE — Port de NitroSense a Linux (nitro-wmi)

**Fecha de cierre**: 22-oct-2026
**Máquina**: Acer Nitro AN515-54 (board Octavia_CFS, familia Nitro 5) — la propia estación de trabajo
**Sistema**: CachyOS, kernel 7.3.0-3-cachyos (clang), OpenCode + REA/Ghidra 12.1.4
**Objetivo**: replicar en Linux el control de ventiladores/modos de NitroSense 3.01.3056 (Acer) mediante RE del paquete original y del firmware del equipo.

---

## 1. Metodología

Ingeniería inversa en 3 capas, con verificación cruzada de cada dato en ≥3 fuentes independientes:

| Capa | Fuente | Herramienta |
|---|---|---|
| Cliente/servidor Windows | NitroSense.msi (21 binarios: PSSvc, PSAgent, PSAdminAgent, SdkWrapper, TsDotNetLib…) | REA+Ghidra, subagentes paralelos |
| Firmware (MOF) | bmof del BIOS (3 blobs) | bmfdec (compilado del source) |
| Firmware (AML) | DSDT + 19 SSDTs del equipo | acpidump + iasl |
| Prior art | JafarAkhondali/acer-predator-turbo…-module, acer-wmi.c mainline | clonado + diff |
| Hardware | el propio laptop | módulo nitro-wmi + sysfs |

Informes completos (1-6): `../programas/NitroSense_informes/`.

## 2. Resultados verificados EN VIVO (22-oct-2026)

| # | Prueba | Resultado | Fuente(s) que lo predijeron |
|---|---|---|---|
| 1 | GUID WMI gaming presente en el kernel | ✓ `/sys/bus/wmi/devices/7A4DDFE7-…-6` | facer.c, bmof, _WDG |
| 2 | Lectura de sensores (method 5) | ✓ Temp CPU 36-48°C, RPM CPU ~4400-5000, RPM GPU ~4200-4700 | informe 5 (GGSI: CTMP/C1FR/G1FR), facer.c (0x0201/0x0601, bits 8-20) |
| 3 | `SetGamingFanSpeed` (method 16, u64 = selector\|pct<<8) | ✓ escritura aceptada; CPUF leído de vuelta (80) | informe 6 (ADMINAGENT asm), informe 5 (WMBH) |
| 4 | % aplicado requiere modo CUSTOM (behavior 14 = 0x430009/0xC30009) | ✓ confirmado: en auto el % no aplica; con CUSTOM sí (fan obedece 40/100%) | informe 6 (flujo ADMINAGENT) — **corrección v0.3 del port** |
| 5 | `SetGamingFanBehavior` (method 14, 0x9 \| modos<<16/<<22) | ✓ aceptado por el VM FOMB del EC (status 0x00); modos auto/turbo operativos | informe 6, facer.c |
| 6 | `GetGamingFanBehavior` (method 15) | stub: status 0 con salida en ceros → el port cachea el estado (igual que NitroSense en el registro) | informe 5 (FOMB), informe 6 |
| 7 | `SetGamingMiscSetting` (22) índice 1 → TKST turbo | ✓ efecto audible en fans | informe 5 (WMBH índices 1/2/5) |
| 8 | `GetGamingMiscSetting` (23) | stub en esta generación de BIOS (su rama AML nunca lee el índice → status 3) | informe 5 — el port cachea |
| 9 | `SetGamingFanTable` (18) valor 1-3 → CPOC | ✓ aceptado (tabla Normal/Fast/Turbo) | informe 5 + informe 2 (niveles OC→tablas) |
| 10 | Validaciones del firmware | ✓ fan >100 → error 2; tabla fuera de 1-3 → error 2; índice misc inválido → error 3 (mapeados a -EINVAL/-EIO) | informe 5 |

## 3. Producto final

**`nitro-wmi` v0.3.0** (`/home/znamreg/ports/nitro-wmi/`):
- Módulo kernel C (GPL) sobre `wmidev_evaluate_method(GUID 7A4DDFE7…)` — 10 atributos sysfs
- CLI `nitro` (C): status/fan/mode/rpm/temp/turbo/table/gpuoc/monitor/raw
- Escritura del % con conmutación automática a CUSTOM (flujo exacto de NitroSense)
- Detección de stubs del firmware con degradación por cache (mismo patrón que Acer)
- Persistencia: DKMS (auto-recompilación en actualizaciones de kernel) + modules-load.d + CLI en /usr/local/bin

## 4. Limitaciones conocidas y documentadas

1. BIOS de esta generación (MOF v2.63, 2019): sin LED methods (1-13) directos en AML; RGB KB no verificado en vivo
2. `GetGamingFanBehavior`/`GetGamingMiscSetting` = stubs → estado por cache
3. Los modos TKST ≠ `SetGamingMiscSetting` índice 0x0B de BIOS nuevas (PSSvc usa 0x0B; esta BIOS usa índice 1) — el port fija índice 1 para esta máquina
4. Fan % CUSTOM de NitroSense no distingue "custom auto" de "auto" por el u64 (curvas custom viven en la app de Windows, si existen)

## 5. Veredicto

**VERIFICADO**: el port controla el hardware del AN515-54 por la misma vía exacta que
NitroSense (WMI → EC), con validaciones del firmware intactas y degradación por cache
idéntica a la del software original. Cierre de la fase de análisis + port funcional.

## 6. Persistencia (instalada 22-oct, 22:10)

- **DKMS 3.4.4**: `nitro-wmi/0.3.0` instalado y firmado en **6.18.55-2-cachyos-lts (GCC)** y **7.3.0-3-cachyos (Clang)** — recompila automáticamente en cada actualización de kernel (AUTOINSTALL + hook pacman)
- **Auto-carga**: `/etc/modules-load.d/nitro-wmi.conf` (se aplicará desde el próximo arranque; el módulo quedó cargado en la sesión actual vía el modprobe del instalador)
- **CLI**: `/usr/local/bin/nitro` (global)
- Lecciones del proceso: DKMS espera el .ko en su build/ interno (no hardcodear M=); su make por defecto auto-detecta Clang (LLVM=1) vs GCC por kernel — la solución fue *quitar* configuración custom
- Bonus observado: `GetGamingFanBehavior` (15) con entrada u32 responde el modo real del EC (mostró "auto" tras instalar) — con entrada u64 devolvía ceros; el GET no es un stub completo, es sensible al tamaño de entrada

---

## 7. Etapa 2 — daemon del protocolo (VERIFICADA 22-oct, 23:xx)

**SELFTEST 13/13** contra el daemon root (`nsclient /run/nitro-sense/nitrosense.sock selftest`):
framing CMF/RMF byte-exacto · registro emulado con rechazo de scope (0x13) ·
pasarela WMI con sensores reales (temp 43°C, RPM CPU 4411, RPM GPU 4109) ·
fan behavior auto (0x410009) · modo de operación dual (0x0B→TKST) · GPU count=2.

### Quirks de firmware adicionales descubiertos en la verificación
1. **Eco del método 22** (WMBH rama 0x16): la respuesta es el buffer de entrada —
   byte[0] = índice en escrituras válidas (TKST→status 1), 3 = índice inválido.
   BIOS nuevas devuelven status reales (0=OK). El módulo acepta ambos (v0.3.2).
2. **Sensibilidad al tamaño de entrada del GET behavior (15)**: con buffer u32
   devuelve el modo real del EC; con u64 devolvía ceros.
3. La codificación de sensores confirmada: u32 `[grupo=01][sensor 01/02/06]`.

### Stack final del port (todo C, persistente)
```
CLI nitro ──► sysfs (nitro_wmi v0.3.2, DKMS 2 kernels, auto-arranque)
nsclient ──► socket /run/nitro-sense/nitrosense.sock (SO_PEERCRED, 0666)
                  └─ nitro-daemon (systemd nitro-sense.service, endurecido)
                         ├─ 35 comandos CMF/RMF (TsDotNetLib-compatible)
                         ├─ registro emulado acotado a SOFTWARE\OEM\NitroSense
                         └─ pasarela raw_call → wmi_evaluate_method → EC
```

*Firmado por el pipeline: 6 informes de subagentes, 2 herramientas compiladas del source
(bmfdec, nitro-wmi), 1 firmware descompilado y 1 laptop que ahora obedece a Linux.* 🐧

---

## Incidente DKMS en actualización de kernel (nov 2026)

**Síntoma**: `dkms autoinstall` del update falla con
`Module version X is not newer than what is already found in kernel Y`.

**Causa**: cada `install.sh` registraba una versión nueva de nitro-wmi en DKMS
pero dejaba las anteriores registradas (0.3.0, 0.3.2, 0.4.0, 0.4.3, 0.4.4).
El hook de kernel intenta instalar todas y DKMS rechaza "bajar" de versión.
Las versiones nuevas sí quedaban instaladas: el error era ruido, no fallo del port.

**Fix (install.sh)**: antes de registrar la versión actual, el script recorre
`dkms status`, retira con `dkms remove --all` toda versión distinta de `$VER`
y borra `/usr/src/nitro-wmi-<old>` y `/var/lib/dkms/nitro-wmi/<old>`.
Así el hook de kernel solo ve una versión y no puede fallar por comparación.

**Lección**: un install script de DKMS debe ser *idempotente y exclusivo*:
una sola versión registrada por módulo. DKMS no está diseñado para coexistencia
de versiones del mismo módulo en el mismo kernel.

---

## Actualización — 21-nov-2026: GUI v3 y prueba de esfuerzo del EC

**Síntoma**: los sliders de la GUI se movían, pero el porcentaje del EC no
cambiaba. El daemon, invocado directamente, sí funcionaba.

**Causa**: `proto_call()` recorre `args` esperando una **lista de bytes**, pero
los callbacks le pasaban un `bytes` suelto. Al iterarlo, Python devuelve enteros
y `len(a)` lanza `TypeError`. GTK captura la excepción del callback y la descarta:
la GUI seguía viva y el comando nunca salía. Los sensores (lectura directa de
sysfs) funcionaban, lo que ocultaba el fallo.

**Fix (GUI v3)**:
- `call()` normaliza `bytes` → `[bytes]` antes de `proto_call()`.
- La conexión se reintenta en cada escritura; un fallo se muestra en la barra
  de estado en vez de silenciarse.
- Debounce de 180 ms: un comando al soltar el slider, no uno por píxel.
- La carga inicial no dispara callbacks (no fuerza modo Personalizado al abrir).

**Prueba de esfuerzo del EC** (daemon directo, sin GUI):

| Acción | CPU % | RPM CPU | RPM GPU | fan_mode |
|---|---|---|---|---|
| inicio | 55 | 4477 | 4166 | 0 (auto) |
| set 100 % + behavior custom | 100 | 6122 | 6000 | 2 (custom) |
| restaurar auto | 100* | 6122 | 5769 | 0 (auto) |

\* el porcentaje queda como último valor escrito; el EC vuelve a su curva.

**Prueba de la GUI en Xvfb**: slider a 77 % → `fan_cpu_percentage=77`,
`fan_mode=2`, status `0x00`. Turbo, brillo, RGB y modo Auto también devuelven
`0x00` por el socket.

**Pendiente**: `sudo ./install.sh` para sustituir la GUI instalada (la copia en
`/usr/local/bin` aún es la v2) y limpiar las versiones antiguas de DKMS.

