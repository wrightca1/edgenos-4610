#!/usr/bin/env python3
"""
phy_dump.py — fast bulk dump of 84758 PHY registers on a locked ICOS port via
bcmsh `phy <port> 0x<DEVAD><REG>`. Assumes ICOS already in bcmsh OR logs in.
Usage: phy_dump.py <port> <devad-hex> <start-hex> <end-hex>
  e.g. phy_dump.py xe0 1 c800 c8ff   -> dumps 1.0xc800..1.0xc8ff
Prints "<reg>: <val>" lines for the requested devad only (fast, parsed).
"""
import serial, time, sys, re

port = sys.argv[1]; dev = int(sys.argv[2], 16)
start = int(sys.argv[3], 16); end = int(sys.argv[4], 16)
s = serial.Serial('/dev/ttyUSB1', 115200, timeout=0.2)

def slow(t):
    for ch in t.encode(): s.write(bytes([ch])); s.flush(); time.sleep(0.006)

def drain(t=0.5):
    e = time.time() + t
    while time.time() < e:
        if s.read(4096): e = time.time() + t
        else: time.sleep(0.03)

# ensure in bcmsh
s.write(b'\r\n'); time.sleep(1.2); cur = s.read(8192).decode(errors='replace')
if 'BCM.0>' not in cur:
    if 'User:' in cur:
        slow('admin\r\n'); time.sleep(1.5); slow('\r\n'); time.sleep(1.5); s.read(4096)
    slow('enable\r\n'); time.sleep(1.2); s.read(4096); slow('\r\n'); time.sleep(0.8); s.read(4096)
    slow('terminal length 0\r\n'); time.sleep(1); s.read(4096)
    slow('bcmsh\r\n'); time.sleep(1.5); o = s.read(8192).decode(errors='replace')
    if 'y/n' in o: slow('y\r\n'); time.sleep(1.2)
drain(1.0)
# warm-up: a throwaway read so the first real reg isn't lost
slow('phy %s 0x10000\r\n' % port); time.sleep(1.0); drain(0.6)

rx = re.compile(r'DevAd %d\(.*Reg 0x[0-9a-f]+: (0x[0-9a-f]+)' % dev)
for reg in range(start, end + 1):
    cmd = 'phy %s 0x%x%04x' % (port, dev, reg)
    s.write((cmd + '\r\n').encode()); s.flush()
    buf = ''; t = time.time()
    while time.time() - t < 3:
        d = s.read(4096).decode(errors='replace')
        if d:
            buf += d
            if buf.rstrip().endswith('BCM.0>'): break
        else:
            time.sleep(0.02)
    m = rx.search(buf)
    sys.stdout.write('%04x: %s\n' % (reg, m.group(1) if m else '????'))
    sys.stdout.flush()
s.close()
