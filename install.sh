#!/usr/bin/env bash
# install.sh — instala nitro-wmi con persistencia total (ejecutar con sudo)
#  - módulo DKMS (recompila solo en cada actualización de kernel)
#  - auto-carga en el arranque (systemd modules-load)
#  - CLI /usr/local/bin/nitro
set -euo pipefail

NAME="nitro-wmi"
VER="0.4.4"
SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="/usr/src/${NAME}-${VER}"

[ "$(id -u)" = 0 ] || { echo "error: ejecutar con sudo"; exit 1; }

echo "==> Compilando CLI, daemon y cliente (el módulo lo compila DKMS)"
if command -v make >/dev/null 2>&1; then
	make -C "$SRC" cli daemon client
else
	echo "    (make no disponible: se usan los binarios ya compilados)"
fi

echo "==> Copiando fuentes a ${DEST}"
rm -rf "$DEST"
mkdir -p "$DEST"
cp "$SRC/nitro_wmi.c" "$SRC/Makefile" "$SRC/dkms.conf" "$DEST/"

echo "==> Instalando CLI, daemon, cliente y GUI en /usr/local/bin"
install -m 755 "$SRC/nitro" /usr/local/bin/nitro
install -m 755 "$SRC/nitro-daemon" /usr/local/bin/nitro-daemon
install -m 755 "$SRC/nsclient" /usr/local/bin/nsclient
install -m 755 "$SRC/nitro-sense-gui.py" /usr/local/bin/nitro-sense-gui
install -m 755 "$SRC/boot-check.sh" /usr/local/bin/nitro-boot-check
mkdir -p /usr/local/share/applications
install -m 644 "$SRC/nitro-sense.desktop" /usr/local/share/applications/nitro-sense.desktop

echo "==> Limpiando versiones antiguas de DKMS (evita errores en updates)"
for old in $(dkms status 2>/dev/null | sed -n "s|^${NAME}/\([^,]*\),.*|\1|p" | sort -u); do
	[ "$old" = "$VER" ] && continue
	echo "    - retirando ${NAME}/${old}"
	dkms remove -m "$NAME" -v "$old" --all >/dev/null 2>&1 || true
	rm -rf "/usr/src/${NAME}-${old}" "/var/lib/dkms/${NAME}/${old}"
done

echo "==> Registrando en DKMS (reregistro limpio)"
dkms remove -m "$NAME" -v "$VER" --all >/dev/null 2>&1 || true
dkms add -m "$NAME" -v "$VER"

echo "==> Compilando e instalando para todos los kernels con headers:"
for kdir in /lib/modules/*/build; do
	kver="$(basename "$(dirname "$kdir")")"
	echo "    - kernel ${kver}"
	dkms install -m "$NAME" -v "$VER" -k "$kver" || echo "    (omitido: $kver)"
done

echo "==> Auto-carga en el arranque"
printf 'nitro_wmi\n' > /etc/modules-load.d/nitro-wmi.conf

echo "==> Servicio systemd del daemon (protocolo NitroSense)"
install -m 644 "$SRC/nitro-sense.service" /etc/systemd/system/nitro-sense.service
systemctl daemon-reload
systemctl enable --now nitro-sense.service

echo "==> Descargando módulo manual (si cargado) y probando el de DKMS"
if timeout 5 rmmod nitro_wmi 2>/dev/null; then
	modprobe nitro_wmi 2>/dev/null || true
else
	echo "    (rmmod no completó: módulo ocupado/pegado — se limpia en el próximo arranque)"
fi

# El daemon recupera el sysfs solo, pero reiniciamos para un estado limpio
systemctl restart nitro-sense.service

echo
echo "OK: nitro-wmi ${VER} persistente."
echo "    - módulo:  DKMS recompila solo tras actualizar kernel (hook de pacman)"
echo "    - daemon:  systemctl status nitro-sense"
echo "    - prueba:  nsclient /run/nitro-sense/nitrosense.sock selftest"
