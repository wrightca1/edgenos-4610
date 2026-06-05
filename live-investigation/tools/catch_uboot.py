#!/usr/bin/env python3
"""
catch_uboot.py — spam a key during reboot to interrupt U-Boot autoboot, then
stop at the U-Boot prompt. Captures all boot output to stdout.

Usage: catch_uboot.py [duration_s] [key]
  key default = space. Stops early when a U-Boot prompt is detected.
Env: SERPORT (default /dev/ttyUSB1), SERBAUD (115200).
"""
import serial, time, sys, os

PORT = os.environ.get("SERPORT", "/dev/ttyUSB1")
BAUD = int(os.environ.get("SERBAUD", "115200"))
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
key = (sys.argv[2] if len(sys.argv) > 2 else " ").encode().decode("unicode_escape").encode("latin-1")

# U-Boot prompt candidates (line-leading)
PROMPTS = [b"=>", b"u-boot>", b"Marvell>>", b"BCM.", b"ar7240>"]

s = serial.Serial(PORT, BAUD, timeout=0.1)
buf = bytearray()
start = time.time()
last_send = 0.0
seen = False
try:
    while time.time() - start < dur:
        data = s.read(4096)
        if data:
            buf += data
            sys.stdout.buffer.write(data); sys.stdout.flush()
        tail = bytes(buf[-120:])
        # once we see the autoboot prompt appear, a couple more keys then stop
        for p in PROMPTS:
            # prompt usually preceded by newline and followed by space
            if (b"\n" + p) in tail or tail.strip().endswith(p):
                seen = True
                break
        if seen:
            # nudge once and drain a little to settle at the prompt
            s.write(key); s.flush()
            time.sleep(0.4)
            buf += s.read(4096)
            break
        now = time.time()
        if now - last_send > 0.15:
            s.write(key); s.flush(); last_send = now
finally:
    s.close()
sys.stderr.write("\n[UBOOT_PROMPT]\n" if seen else "\n[TIMEOUT — no prompt detected]\n")
