#!/usr/bin/env python3
# Reboot the (kernel-alive) box via SysRq, then catch the U-Boot prompt despite the
# USB-serial re-enumerating on reset (retry-open loop + \r flood). Leaves uboot at prompt.
import serial, time, os, sys

PORT = os.environ.get("SERPORT", "/dev/ttyUSB1")
BAUD = 115200
PROMPT = b"accton_as4610"

def open_retry(timeout=25):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            return serial.Serial(PORT, BAUD, timeout=0.1)
        except Exception:
            time.sleep(0.2)
    return None

# 1) trigger reboot via SysRq (sync, then boot)
s = open_retry(8)
if not s:
    print("ERR: cannot open port to send SysRq"); sys.exit(1)
s.send_break(0.3); time.sleep(0.2); s.write(b's'); time.sleep(1.0)   # sync
s.send_break(0.3); time.sleep(0.2); s.write(b'b'); time.sleep(0.3)   # reboot
s.close()
print(">>> SysRq-b sent, box resetting; entering re-enum-tolerant catch <<<", flush=True)

# 2) catch uboot: reopen (device disappears on reset), spam \r, watch for prompt
buf = bytearray()
caught = False
t0 = time.time()
s = None
while time.time() - t0 < 45:
    if s is None:
        s = open_retry(5)
        if s is None:
            continue
    try:
        s.write(b'\r'); s.flush()
        d = s.read(2048)
    except Exception:
        try: s.close()
        except Exception: pass
        s = None
        continue
    if d:
        buf += d
        sys.stdout.buffer.write(d); sys.stdout.flush()
        tail = bytes(buf[-160:])
        if PROMPT in tail:
            caught = True
            # nudge a couple more times to settle at prompt, stop autoboot
            for _ in range(3):
                s.write(b'\r'); s.flush(); time.sleep(0.2); s.read(2048)
            break
    time.sleep(0.08)
if s: s.close()
sys.stderr.write("\n[UBOOT_CAUGHT]\n" if caught else "\n[NOT CAUGHT — box likely booted through]\n")
