#!/usr/bin/env bash
# nitro-boot-check — verifica que TODO el stack de nitro-wmi arrancó solo tras un reboot
# Ejecutar tras reiniciar (sin sudo): nitro-boot-check
set -u

G="\033[32m"; R="\033[31m"; Y="\033[33m"; N="\033[0m"
pass=0; fail=0

ok()   { echo -e "  ${G}✓$N $1"; pass=$((pass+1)); }
bad()  { echo -e "  ${R}✗$N $1"; fail=$((fail+1)); }
info() { echo -e "  ${Y}·$N $1"; }

echo "== nitro-boot-check: stack completo tras arranque =="
echo "kernel: $(uname -r)"

echo "1) módulo kernel"
if lsmod | grep -q "^nitro_wmi"; then ok "nitro_wmi cargado"; else bad "nitro_wmi NO cargado"; fi
echo "2) DKMS"
# Formato real: "nitro-wmi/0.4.4, 7.3.5-2-cachyos, x86_64: installed"
if dkms status 2>/dev/null | grep -q "nitro-wmi.*, $(uname -r), .*installed"; then
	ok "DKMS instalado para este kernel"
else
	bad "DKMS no instalado para $(uname -r)"
	dkms status 2>/dev/null | grep -i nitro | sed 's/^/      /'
fi
echo "3) sysfs del módulo"
if ls /sys/bus/wmi/devices/7A4DDFE7-*/nitro_wmi >/dev/null 2>&1; then
	ok "atributos sysfs presentes"
else
	bad "sin atributos sysfs"
fi
echo "4) daemon"
if systemctl is-active --quiet nitro-sense; then ok "nitro-sense.service activo"; else bad "nitro-sense.service inactivo"; fi
if [ -S /run/nitro-sense/nitrosense.sock ]; then ok "socket presente"; else bad "sin socket"; fi
echo "5) platform_profile"
if [ -r /sys/firmware/acpi/platform_profile ]; then
	ok "perfil actual: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null)"
else
	info "no registrado (deshabilitado en v0.4.4; vuelve en v0.5)"
fi
echo "6) sensores (lectura en vivo)"
t=$(cat /sys/bus/wmi/devices/7A4DDFE7-*/nitro_wmi/cpu_temp 2>/dev/null)
rc=$(cat /sys/bus/wmi/devices/7A4DDFE7-*/nitro_wmi/cpu_fan_rpm 2>/dev/null)
if [ -n "$t" ] && [ -n "$rc" ]; then ok "temp ${t}°C · RPM CPU ${rc}"; else bad "sensores no responden"; fi
echo "7) selftest del protocolo (13 pruebas)"
DIR="$(dirname "$(readlink -f "$0")")"
NSC=$(command -v nsclient || echo "$DIR/nsclient")
if [ -x "$NSC" ]; then
	out=$("$NSC" /run/nitro-sense/nitrosense.sock selftest 2>&1 | tail -1)
	case "$out" in
		*"13/13"*) ok "selftest 13/13" ;;
		*) bad "selftest: $out" ;;
	esac
else
	info "nsclient no encontrado — se omite (instalado en /usr/local/bin)"
fi
echo "8) GUI"
[ -x /usr/local/bin/nitro-sense-gui ] && ok "nitro-sense-gui instalada" || info "GUI no instalada (install.sh)"

echo
if [ "$fail" -eq 0 ]; then
	echo -e "${G}ARRANQUE VERIFICADO: $pass/$pass pruebas OK$N"
	echo "El port completo funciona tras el reinicio sin intervención."
else
	echo -e "${R}$fail fallos$N — revisar: journalctl -u nitro-sense -b"
fi
exit $fail
