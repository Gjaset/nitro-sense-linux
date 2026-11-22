#!/usr/bin/env python3
"""nitro-sense-gui — interfaz gráfica del port de NitroSense para Linux.

Cliente del protocolo del daemon (socket Unix, CMF/RMF — igual que la app
UWP de NitroSense hablaba con PSSvc por named pipe). Lecturas de sensores
directo a sysfs (sin privilegios); escrituras por el socket (daemon root).

Requisitos: PyGObject + GTK3 (incluidos de serie en CachyOS).

v3:
  - corrige el envío de argumentos (antes se pasaba un bytes suelto y
    proto_call lo iteraba como enteros → TypeError silencioso en GTK)
  - no fuerza modo Personalizado al abrir (carga inicial protegida)
  - slider con debounce: un solo comando al soltar, no uno por píxel
  - diseño espacioso: tarjetas, márgenes amplios, controles grandes
"""
import glob
import socket
import struct

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib, Gdk

SOCK = "/run/nitro-sense/nitrosense.sock"
SYSFS_GLOB = "/sys/bus/wmi/devices/7A4DDFE7-*/nitro_wmi"

CMD_SET_FAN_BEHAVIOR = 0x0F
CMD_SET_FAN_SPEED = 0x10
CMD_OP_MODE = 0x1E
CMD_SET_RGB_KB = 0x1C
CMD_SET_KB_BACKLIGHT = 0x1B

FAN_AUTO = 0x410009
FAN_TURBO = 0x820009
FAN_CUSTOM = 0xC30009

SLIDER_DEBOUNCE_MS = 180

CSS = b"""
.card {
    background-color: rgba(128, 128, 128, 0.10);
    border-radius: 14px;
    padding: 18px;
}
.section-title {
    font-weight: bold;
    font-size: 105%;
    opacity: 0.85;
}
.metric-value {
    font-size: 180%;
    font-weight: bold;
}
.metric-caption {
    font-size: 85%;
    opacity: 0.55;
}
.statusbar {
    padding: 12px 24px;
    font-size: 90%;
    opacity: 0.75;
}
.swatch {
    border: 1px solid rgba(128, 128, 128, 0.45);
    border-radius: 10px;
}
"""


def to_rgba(r, g, b):
    c = Gdk.RGBA()
    c.red, c.green, c.blue, c.alpha = r / 255.0, g / 255.0, b / 255.0, 1.0
    return c


def sysfs_dir():
    d = glob.glob(SYSFS_GLOB)
    return d[0] if d else None


def sysfs_read(attr):
    d = sysfs_dir()
    if not d:
        return None
    try:
        with open(f"{d}/{attr}") as f:
            return f.read().strip()
    except OSError:
        return None


def proto_call(sock_path, cmd, args):
    """Llamada CMF/RMF al daemon → (status, [resultados]).

    `args` es una lista de bytes (un elemento por argumento del protocolo).
    """
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect(sock_path)
        msg = struct.pack("<HB", cmd, len(args))
        for a in args:
            msg += struct.pack("<I", len(a)) + a
        s.sendall(msg)

        def recv_n(n):
            buf = b""
            while len(buf) < n:
                chunk = s.recv(n - len(buf))
                if not chunk:
                    raise ConnectionError("el daemon cerró la conexión")
                buf += chunk
            return buf

        nargs = recv_n(1)[0]
        results, status = [], 0xFF
        for i in range(nargs):
            sz = struct.unpack("<I", recv_n(4))[0]
            data = recv_n(sz) if sz else b""
            if i == 0:
                status = struct.unpack("<I", data[:4])[0] if sz >= 4 else 0xFF
            else:
                results.append(data)
        return status, results
    finally:
        s.close()


def install_css():
    provider = Gtk.CssProvider()
    try:
        provider.load_from_data(CSS)
    except GLib.Error:
        return
    screen = Gdk.Screen.get_default()
    if screen:
        Gtk.StyleContext.add_provider_for_screen(
            screen, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


def section(title):
    """Cabecera de sección con aire arriba y abajo."""
    lbl = Gtk.Label()
    lbl.set_markup(f"<b>{GLib.markup_escape_text(title)}</b>")
    lbl.set_halign(Gtk.Align.START)
    lbl.set_margin_top(20)
    lbl.set_margin_bottom(8)
    lbl.get_style_context().add_class("section-title")
    return lbl


def metric_card(caption):
    """Tarjeta de métrica: número grande + pie descriptivo."""
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
    box.get_style_context().add_class("card")
    box.set_hexpand(True)

    value = Gtk.Label(label="—")
    value.get_style_context().add_class("metric-value")
    value.set_halign(Gtk.Align.CENTER)

    cap = Gtk.Label(label=caption)
    cap.get_style_context().add_class("metric-caption")
    cap.set_halign(Gtk.Align.CENTER)

    box.pack_start(value, False, False, 0)
    box.pack_start(cap, False, False, 0)
    return box, value


class NitroSenseGUI(Gtk.Window):
    def __init__(self):
        super().__init__(title="NitroSense para Linux")
        self.set_default_size(760, 880)
        self.set_size_request(620, 620)
        self.set_border_width(0)

        self.daemon_ok = False
        self._loading = True
        self._syncing_mode = False
        self._ticks = 0
        self._pending = {}
        self._pending_timeout = None

        install_css()

        # ---- raíz: contenido scrolleable + barra de estado fija ----
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.add(root)

        scroll = Gtk.ScrolledWindow()
        scroll.set_hexpand(True)
        scroll.set_vexpand(True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        root.pack_start(scroll, True, True, 0)

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        content.set_margin_start(28)
        content.set_margin_end(28)
        content.set_margin_top(22)
        content.set_margin_bottom(26)
        scroll.add(content)

        # ---- cabecera ----
        title = Gtk.Label()
        title.set_markup("<span size='xx-large' weight='bold'>NitroSense</span>")
        title.set_halign(Gtk.Align.START)
        content.pack_start(title, False, False, 0)

        subtitle = Gtk.Label(label="Acer Nitro AN515-54 · ventiladores, rendimiento y RGB")
        subtitle.set_halign(Gtk.Align.START)
        subtitle.set_margin_bottom(10)
        subtitle.get_style_context().add_class("metric-caption")
        content.pack_start(subtitle, False, False, 0)

        # ---- monitor en vivo (tarjetas) ----
        mon = Gtk.Box(spacing=14, homogeneous=True)
        mon.set_margin_top(4)
        card_t, self.lbl_temp = metric_card("Temperatura CPU (°C)")
        card_c, self.lbl_cpu = metric_card("RPM CPU")
        card_g, self.lbl_gpu = metric_card("RPM GPU")
        for c in (card_t, card_c, card_g):
            mon.pack_start(c, True, True, 0)
        content.pack_start(mon, False, False, 0)

        # ---- modo de fans ----
        content.pack_start(section("Modo de ventiladores"), False, False, 0)
        mode_card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        mode_card.get_style_context().add_class("card")
        mode_hint = Gtk.Label(
            label="Auto sigue la curva del EC · CoolBoost fuerza turbo · "
                  "Personalizado usa los sliders")
        mode_hint.set_halign(Gtk.Align.START)
        mode_hint.set_line_wrap(True)
        mode_hint.get_style_context().add_class("metric-caption")
        self.mode_combo = Gtk.ComboBoxText()
        for name in ("Auto", "CoolBoost (turbo)", "Personalizado"):
            self.mode_combo.append_text(name)
        self.mode_combo.set_active(0)
        self.mode_combo.set_size_request(-1, 42)
        self.mode_combo.set_hexpand(True)
        mode_card.pack_start(self.mode_combo, False, False, 0)
        mode_card.pack_start(mode_hint, False, False, 0)
        content.pack_start(mode_card, False, False, 0)
        self.mode_combo.connect("changed", self.on_mode_changed)

        # ---- sliders ----
        content.pack_start(section("Velocidad manual"), False, False, 0)
        sliders_card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        sliders_card.get_style_context().add_class("card")

        self.scales = {}
        self.value_labels = {}
        for key, label in (("cpu", "Fan CPU"), ("gpu", "Fan GPU")):
            row = Gtk.Box(spacing=14)
            lbl = Gtk.Label(label=label)
            lbl.set_halign(Gtk.Align.START)
            lbl.set_size_request(72, -1)

            scale = Gtk.Scale.new_with_range(
                Gtk.Orientation.HORIZONTAL, 0, 100, 1)
            scale.set_hexpand(True)
            scale.set_draw_value(False)
            scale.set_size_request(-1, 46)
            for mark in (0, 25, 50, 75, 100):
                scale.add_mark(mark, Gtk.PositionType.BOTTOM, None)

            value = Gtk.Label()
            value.set_halign(Gtk.Align.END)
            value.set_size_request(64, -1)
            value.set_markup("<b>—</b>")

            row.pack_start(lbl, False, False, 0)
            row.pack_start(scale, True, True, 0)
            row.pack_start(value, False, False, 0)
            sliders_card.pack_start(row, False, False, 0)

            scale.connect("value-changed", self.on_slider,
                          1 if key == "cpu" else 4)
            self.scales[key] = scale
            self.value_labels[key] = value

        sliders_hint = Gtk.Label(
            label="Mover un slider conmuta los ventiladores a modo Personalizado.")
        sliders_hint.set_halign(Gtk.Align.START)
        sliders_hint.set_line_wrap(True)
        sliders_hint.get_style_context().add_class("metric-caption")
        sliders_card.pack_start(sliders_hint, False, False, 0)
        content.pack_start(sliders_card, False, False, 0)

        # ---- rendimiento ----
        content.pack_start(section("Rendimiento"), False, False, 0)
        perf_card = Gtk.Box(spacing=12)
        perf_card.get_style_context().add_class("card")
        t_lbl = Gtk.Label(label="Modo turbo del sistema")
        t_lbl.set_halign(Gtk.Align.START)
        t_lbl.set_hexpand(True)
        self.turbo_switch = Gtk.Switch()
        self.turbo_switch.set_valign(Gtk.Align.CENTER)
        self.turbo_switch.connect("notify::active", self.on_turbo)
        perf_card.pack_start(t_lbl, True, True, 0)
        perf_card.pack_start(self.turbo_switch, False, False, 0)
        content.pack_start(perf_card, False, False, 0)

        # ---- RGB ----
        content.pack_start(section("RGB del teclado (4 zonas)"), False, False, 0)
        rgb_card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        rgb_card.get_style_context().add_class("card")
        rgb_box = Gtk.Box(spacing=14, homogeneous=True)
        self.rgb_btns = []
        for i in range(4):
            cell = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            btn = Gtk.ColorButton()
            btn.get_style_context().add_class("swatch")
            btn.set_rgba(to_rgba(0xFF, 0, 0))
            btn.set_size_request(-1, 52)
            btn.set_title(f"Zona {i+1}")
            btn.set_tooltip_text(f"Zona {i+1} — clic para elegir color")
            btn.connect("color-set", self.on_rgb, 1 << i)
            zone = Gtk.Label(label=f"Zona {i+1}")
            zone.set_halign(Gtk.Align.CENTER)
            zone.get_style_context().add_class("metric-caption")
            cell.pack_start(btn, False, False, 0)
            cell.pack_start(zone, False, False, 0)
            rgb_box.pack_start(cell, True, True, 0)
            self.rgb_btns.append(btn)
        rgb_card.pack_start(rgb_box, False, False, 0)
        content.pack_start(rgb_card, False, False, 0)

        # ---- brillo ----
        bright_card = Gtk.Box(spacing=12)
        bright_card.get_style_context().add_class("card")
        bright_card.set_margin_top(10)
        b_lbl = Gtk.Label(label="Brillo del teclado")
        b_lbl.set_halign(Gtk.Align.START)
        b_lbl.set_hexpand(True)
        self.bright_combo = Gtk.ComboBoxText()
        for lvl in ("Nivel 1 · bajo", "Nivel 2", "Nivel 3", "Nivel 4 · alto"):
            self.bright_combo.append_text(lvl)
        self.bright_combo.set_active(1)
        self.bright_combo.set_size_request(180, 40)
        self.bright_combo.connect("changed", self.on_brightness)
        bright_card.pack_start(b_lbl, True, True, 0)
        bright_card.pack_start(self.bright_combo, False, False, 0)
        content.pack_start(bright_card, False, False, 0)

        # ---- barra de estado fija ----
        sep = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
        root.pack_end(self._status_bar(sep), False, False, 0)

        self.check_daemon()
        self.load_initial_state()
        self._loading = False
        GLib.timeout_add_seconds(1, self.update_sensors)
        self.update_sensors()

    # ---------- helpers ----------

    def _status_bar(self, sep):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        box.pack_start(sep, False, False, 0)
        self.status = Gtk.Label()
        self.status.set_hexpand(True)
        self.status.set_halign(Gtk.Align.START)
        self.status.set_ellipsize(3)  # Pango.EllipsizeMode.END
        self.status.get_style_context().add_class("statusbar")
        box.pack_start(self.status, False, False, 0)
        return box

    def call(self, cmd, args=b""):
        """Envía un comando al daemon.

        Acepta `args` como bytes (un único argumento) o como lista de bytes.
        """
        if isinstance(args, (bytes, bytearray)):
            args = [bytes(args)] if len(args) else []
        try:
            st, res = proto_call(SOCK, cmd, args)
            if not self.daemon_ok:
                self.daemon_ok = True
                self.set_status("Daemon conectado ✔")
            return st, res
        except (OSError, ConnectionError) as e:
            self.daemon_ok = False
            self.set_status(f"⚠ Sin daemon: {e}")
        except Exception as e:  # nunca dejar caer un callback GTK
            self.set_status(f"error: {e}")
        return 0xFF, []

    def set_status(self, text):
        self.status.set_markup(
            f"<small>{GLib.markup_escape_text(text)}</small>")

    def check_daemon(self):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(1)
            s.connect(SOCK)
            s.close()
            self.daemon_ok = True
        except OSError:
            self.daemon_ok = False
        if self.daemon_ok:
            self.set_status("Daemon conectado ✔")
        else:
            self.set_status(
                "⚠ Sin daemon (systemctl start nitro-sense) — solo lecturas")

    def _sync_mode(self, idx):
        if self.mode_combo.get_active() == idx:
            return
        self._syncing_mode = True
        self.mode_combo.set_active(idx)
        self._syncing_mode = False

    def _cancel_pending(self):
        if self._pending_timeout is not None:
            GLib.source_remove(self._pending_timeout)
            self._pending_timeout = None
        self._pending.clear()

    def load_initial_state(self):
        """Refleja el estado real del EC sin enviar comandos."""
        self._loading = True
        for key, attr in (("cpu", "fan_cpu_percentage"),
                          ("gpu", "fan_gpu_percentage")):
            v = sysfs_read(attr)
            if v and v.isdigit():
                self.scales[key].set_value(int(v))
                self.value_labels[key].set_markup(f"<b>{int(v)}%</b>")
        mode = sysfs_read("fan_mode")
        if mode in ("0", "1", "2"):
            self._sync_mode(int(mode))
        for i, btn in enumerate(self.rgb_btns):
            v = sysfs_read(f"kb_rgb_zone{i+1}")
            if v:
                try:
                    rgb = int(v, 16)
                    btn.set_rgba(to_rgba((rgb >> 16) & 0xFF,
                                         (rgb >> 8) & 0xFF,
                                         rgb & 0xFF))
                except ValueError:
                    pass
        self._loading = False

    def update_sensors(self):
        self.lbl_temp.set_text(sysfs_read("cpu_temp") or "—")
        self.lbl_cpu.set_text(sysfs_read("cpu_fan_rpm") or "—")
        self.lbl_gpu.set_text(sysfs_read("gpu_fan_rpm") or "—")
        self._ticks += 1
        if not self.daemon_ok and self._ticks % 5 == 0:
            self.check_daemon()
        return True

    # ---------- callbacks ----------

    def on_mode_changed(self, combo):
        if self._loading or self._syncing_mode:
            return
        self._cancel_pending()
        idx = combo.get_active()
        if idx == 0:
            st, _ = self.call(CMD_SET_FAN_BEHAVIOR,
                              struct.pack("<Q", FAN_AUTO))
        elif idx == 1:
            st, _ = self.call(CMD_SET_FAN_BEHAVIOR,
                              struct.pack("<Q", FAN_TURBO))
        else:
            for fan, key in ((1, "cpu"), (4, "gpu")):
                pct = int(self.scales[key].get_value())
                self.call(CMD_SET_FAN_SPEED,
                          struct.pack("<Q", (pct << 8) | fan))
            st, _ = self.call(CMD_SET_FAN_BEHAVIOR,
                              struct.pack("<Q", FAN_CUSTOM))
        self.set_status(f"Modo de ventiladores → status 0x{st:02x}")

    def on_slider(self, scale, fan_id):
        if self._loading:
            return
        key = "cpu" if fan_id == 1 else "gpu"
        pct = int(round(scale.get_value()))
        self.value_labels[key].set_markup(f"<b>{pct}%</b>")
        self._pending[key] = pct
        self._sync_mode(2)
        if self._pending_timeout is None:
            self._pending_timeout = GLib.timeout_add(
                SLIDER_DEBOUNCE_MS, self._flush_sliders)

    def _flush_sliders(self):
        self._pending_timeout = None
        pending, self._pending = self._pending, {}
        if not pending:
            return False
        st = 0
        for key, pct in pending.items():
            fan = 1 if key == "cpu" else 4
            st, _ = self.call(CMD_SET_FAN_SPEED,
                              struct.pack("<Q", (pct << 8) | fan))
        self.call(CMD_SET_FAN_BEHAVIOR, struct.pack("<Q", FAN_CUSTOM))
        parts = " · ".join(
            f"{'CPU' if k == 'cpu' else 'GPU'} {v}%" for k, v in pending.items())
        self.set_status(f"{parts} → status 0x{st:02x}")
        return False

    def on_turbo(self, switch, _param):
        if self._loading:
            return
        val = 1 if switch.get_active() else 0
        st, _ = self.call(CMD_OP_MODE, struct.pack("<I", val))
        self.set_status(f"Modo turbo {val} → status 0x{st:02x}")

    def on_rgb(self, btn, zone_bit):
        if self._loading:
            return
        rgba = btn.get_rgba()
        data = struct.pack("<BBBB", zone_bit,
                           int(rgba.red * 255),
                           int(rgba.green * 255),
                           int(rgba.blue * 255))
        st, _ = self.call(CMD_SET_RGB_KB, data)
        self.set_status(f"Zona RGB → status 0x{st:02x}")

    def on_brightness(self, combo):
        if self._loading:
            return
        level = combo.get_active() + 1
        data = bytes([0, 0, (level - 1) * 25] + [0] * 13)
        st, _ = self.call(CMD_SET_KB_BACKLIGHT, data)
        self.set_status(f"Brillo KB nivel {level} → status 0x{st:02x}")


def main():
    win = NitroSenseGUI()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
