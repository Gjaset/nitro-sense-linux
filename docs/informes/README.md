# Documentación de ingeniería inversa

Informes producidos durante el port, en orden cronológico. Cada dato del
protocolo se contrastó con al menos tres fuentes independientes (binarios de
NitroSense, MOF del BIOS, AML del firmware y prior art comunitario).

| Informe | Contenido |
|---|---|
| [1 — hardware](informe_1_hardware_SdkWrapper_ProfileHelper.md) | SdkWrapper, ProfileHelper y modelo de hardware |
| [1b — validación cruzada](informe_1b_validacion_cruzada_SdkWrapper.md) | Contraste del informe 1 contra los demás binarios |
| [2 — procesos](informe_2_procesos_PSSvc_PSAgent.md) | PSSvc/PSAgent: protocolo CMF/RMF, comandos, registro |
| [3 — configuraciones](informe_3_configs_TsDotNetLib_strings.md) | TsDotNetLib, cadenas y configuración |
| [4 — prior art](informe_4_prior_art_WMI_GUID.md) | GUIDs y métodos en drivers comunitarios y mainline |
| [5 — firmware](informe_5_firmware_WMBH_AN515-54.md) | WMBH del SSDT9: layout del u64 y validaciones |
| [6 — bitpacking](informe_6_ADMINAGENT_bitpacking.md) | Empaquetado de argumentos en PSAdminAgent |

Los volcados ACPI usados como evidencia están en [`../acpi/`](../acpi/).
