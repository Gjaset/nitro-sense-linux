#!/usr/bin/env bash
# uninstall.sh — elimina la instalación persistente de nitro-wmi
set -euo pipefail

NAME="nitro-wmi"

[ "$(id -u)" = 0 ] || { echo "error: ejecutar con sudo"; exit 1; }

echo "==> Deteniendo y deshabilitando servicio"
systemctl disable --now nitro-sense.service 2>/dev/null || true
rm -f /etc/systemd/system/nitro-sense.service
systemctl daemon-reload

echo "==> Descargando módulo"
rmmod nitro_wmi 2>/dev/null || true

echo "==> Quitando de DKMS (todas las versiones registradas)"
for ver in $(dkms status 2>/dev/null | sed -n "s|^${NAME}/\([^,]*\),.*|\1|p" | sort -u); do
	echo "    - retirando ${NAME}/${ver}"
	dkms remove -m "$NAME" -v "$ver" --all >/dev/null 2>&1 || true
	rm -rf "/usr/src/${NAME}-${ver}" "/var/lib/dkms/${NAME}/${ver}"
done

echo "==> Borrando fuentes, binarios y configuración"
rm -rf /usr/src/${NAME}-*
rm -f /etc/modules-load.d/nitro-wmi.conf
rm -f /usr/local/bin/nitro /usr/local/bin/nitro-daemon
rm -f /usr/local/bin/nsclient /usr/local/bin/nitro-sense-gui
rm -f /usr/local/bin/nitro-boot-check
rm -f /usr/local/share/applications/nitro-sense.desktop

echo "OK: nitro-wmi eliminado por completo."
