# Evidencia ACPI — Acer Nitro AN515-54 (Octavia_CFS)

Volcados del firmware de la máquina de referencia, descompilados con `iasl`.
Se incluyen solo los artefactos citados por el código y los informes:

| Archivo | Qué contiene |
|---|---|
| `dsdt.dsl` | DSDT completo: `_Q41` (hotkey NitroSense `01 8A 01`), campo ECMM con `LMEN`, `_Q45`/`_Q46` |
| `ssdt9.dsl` | SSDT9: `WMBH` (métodos 1-23 de `AcerGamingFunction`), RGB (6/7), backlight (20) y `_WED` |
| `bmof_10.mof` | MOF del BIOS: clase `AcerGamingFunction` y firmas de los 23 métodos |

Los 19 SSDTs restantes se omiten por tamaño; no intervienen en el camino
ventiladores/RGB. Para regenerarlos en tu equipo:

```fish
sudo acpidump -o acpi.dat
acpixtract -a acpi.dat
iasl -d *.dat
```

> Los volcados son específicos de este modelo y revisión de BIOS. No los uses
> como sustituto del firmware de tu propia máquina.
