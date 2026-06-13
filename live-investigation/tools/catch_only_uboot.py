#!/usr/bin/env python3
# Catch uboot after a USER power-cycle: retry-open ttyUSB1 (it re-enumerates on
# power-on), flood \r, stop at the accton_as4610 prompt. No SysRq (box is off).
import serial, time, sys
PORT='/dev/ttyUSB1'; PROMPT=b'accton_as4610'
def opn(t=5):
    t0=time.time()
    while time.time()-t0<t:
        try: return serial.Serial(PORT,115200,timeout=0.1)
        except Exception: time.sleep(0.2)
    return None
buf=bytearray(); t0=time.time(); s=None; caught=False
while time.time()-t0<240:
    if s is None:
        s=opn(5)
        if s is None: continue
    try:
        s.write(b'\r'); s.flush(); d=s.read(2048)
    except Exception:
        try: s.close()
        except Exception: pass
        s=None; continue
    if d:
        buf+=d
        sys.stdout.buffer.write(d); sys.stdout.flush()
        if PROMPT in bytes(buf[-160:]):
            caught=True
            for _ in range(3): s.write(b'\r'); time.sleep(0.2); s.read(2048)
            break
    time.sleep(0.07)
if s: s.close()
sys.stderr.write("\n[UBOOT_CAUGHT]\n" if caught else "\n[NOT CAUGHT in 240s]\n")
