#!/usr/bin/env python3
"""key-capture — captura los eventos de la tecla dedicada (Acer WMI hotkeys).

Ejecutar con sudo, pulsar la tecla NitroSense y Ctrl-C para salir:
    sudo python3 key-capture.py
"""
import glob
import re
import struct
import sys

TARGET = "Acer WMI hotkeys"
NAMES = {
    14: "KEY_POWER?", 113: "KEY_MUTE", 114: "KEY_VOLUMEUP", 115: "KEY_VOLUMEDOWN",
    116: "KEY_116", 138: "KEY_HELP", 148: "KEY_PROG1", 149: "KEY_PROG2",
    150: "KEY_WWW", 152: "KEY_SCREENLOCK", 158: "KEY_BACK", 159: "KEY_FORWARD",
    160: "KEY_NEXTSONG", 161: "KEY_PLAYPAUSE", 162: "KEY_PREVIOUSSONG",
    163: "KEY_STOPCD", 164: "KEY_EJECTCD", 165: "KEY_REWIND", 166: "KEY_FASTFORWARD",
    178: "KEY_SCROLLUP", 183: "KEY_F13", 184: "KEY_F14", 185: "KEY_F15",
    200: "KEY_PLAYCD", 201: "KEY_PAUSECD", 202: "KEY_PROG3", 203: "KEY_PROG4",
    210: "KEY_TOUCHPAD_TOGGLE", 224: "KEY_BRIGHTNESSDOWN", 225: "KEY_BRIGHTNESSUP",
    226: "KEY_MEDIA", 227: "KEY_SWITCHVIDEOMODE", 228: "KEY_KBDILLUMTOGGLE",
    234: "KEY_SAVE", 237: "KEY_BATTERY", 238: "KEY_BLUETOOTH", 240: "KEY_UNKNOWN",
}


def find_event():
    data = open("/proc/bus/input/devices").read()
    for block in data.split("\n\n"):
        if TARGET in block:
            m = re.search(r"Handlers=\s*\S*\s*(event\d+)", block)
            if m:
                return m.group(1)
    return None


def main():
    ev = find_event()
    if not ev:
        print(f"no se encontró el dispositivo '{TARGET}'")
        sys.exit(1)
    path = f"/dev/input/{ev}"
    print(f"Leyendo {path} — presiona la tecla NitroSense ahora (Ctrl-C para salir)\n")
    try:
        with open(path, "rb") as f:
            while True:
                raw = f.read(24)  # struct input_event (64-bit)
                if len(raw) < 24:
                    break
                _sec, _usec, etype, code, value = struct.unpack("<qqHHi", raw)
                if etype == 0:
                    continue
                if etype == 4:
                    print(f"MSC_SCAN        scancode = {value:#04x}")
                elif etype == 1:
                    state = "pressed " if value == 1 else \
                            "held    " if value == 2 else "released"
                    print(f"EV_KEY          keycode  = {code:3d} "
                          f"({NAMES.get(code, 'KEY_?')})  {state}")
    except KeyboardInterrupt:
        print("\nfin.")


if __name__ == "__main__":
    main()
