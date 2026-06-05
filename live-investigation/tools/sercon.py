#!/usr/bin/env python3
"""
sercon.py — minimal scriptable serial-console helper for the AS4610 investigation.

Each invocation reopens the port; the *device-side* shell session persists, so we
drive it one command at a time and capture output. Reading stops when the line
goes idle (no new bytes for --idle seconds) or --timeout is hit.

Usage:
  sercon.py read [--timeout N] [--idle S]            # passive read only
  sercon.py send "CMD" [--timeout N] [--idle S]      # send CMD + CR, capture
  sercon.py raw  "BYTES" [--no-cr] [--timeout N]     # send raw (e.g. Ctrl-C = $'\\x03')
  sercon.py key  ENTER|CTRL_C|CTRL_M ...             # send control keys

Env: SERPORT (default /dev/ttyUSB1), SERBAUD (default 115200).
DTR/RTS are left untouched (no device reset on open).
"""
import os, sys, time, argparse, serial

PORT = os.environ.get("SERPORT", "/dev/ttyUSB1")
BAUD = int(os.environ.get("SERBAUD", "115200"))

def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = BAUD
    s.bytesize = serial.EIGHTBITS
    s.parity = serial.PARITY_NONE
    s.stopbits = serial.STOPBITS_ONE
    s.timeout = 0.2
    s.dsrdtr = False
    s.rtscts = False
    s.xonxoff = False
    s.open()
    return s

def drain(s, timeout, idle):
    """Read until `idle` seconds of silence, or `timeout` total."""
    buf = bytearray()
    start = time.time()
    last = start
    while True:
        chunk = s.read(4096)
        now = time.time()
        if chunk:
            buf += chunk
            last = now
        if now - start > timeout:
            break
        if now - last > idle and len(buf) > 0:
            break
        if not chunk and now - start > idle and len(buf) == 0:
            # nothing at all came; give up after one idle window
            if now - start > max(idle, 1.5):
                break
    return bytes(buf)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["read", "send", "raw", "key"])
    ap.add_argument("arg", nargs="?", default="")
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--idle", type=float, default=0.8)
    ap.add_argument("--no-cr", action="store_true")
    a = ap.parse_args()

    s = open_port()
    try:
        s.reset_input_buffer()
        if a.mode == "send":
            payload = a.arg.encode() + (b"" if a.no_cr else b"\r")
            s.write(payload); s.flush()
        elif a.mode == "raw":
            data = a.arg.encode().decode("unicode_escape").encode("latin-1")
            if not a.no_cr:
                data += b"\r"
            s.write(data); s.flush()
        elif a.mode == "key":
            keymap = {"ENTER": b"\r", "CTRL_C": b"\x03", "CTRL_M": b"\r",
                      "CTRL_D": b"\x04", "SPACE": b" ", "ESC": b"\x1b"}
            for k in a.arg.split():
                s.write(keymap.get(k, b"")); s.flush(); time.sleep(0.1)
        out = drain(s, a.timeout, a.idle)
        sys.stdout.buffer.write(out)
        sys.stdout.flush()
    finally:
        s.close()

if __name__ == "__main__":
    main()
