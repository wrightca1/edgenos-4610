#!/usr/bin/env python3
"""
icos_serial_capture.py — log into ICOS over the SERIAL console, enter the BCM diag
shell (bcmsh), and run a sequence of diag commands, capturing output cleanly.
bcmsh is console-only on this build (telnet denied), so register capture must be serial.

Usage: icos_serial_capture.py <cmdfile>
  <cmdfile>: one diag command per line (run at the BCM.0> prompt). '#' lines skipped.
Env: SERPORT(/dev/ttyUSB1).
"""
import serial, time, sys, os

PORT = os.environ.get("SERPORT", "/dev/ttyUSB1")
s = serial.Serial(PORT, 115200, timeout=0.3)

def rd(t=1.0):
    out = b''; e = time.time() + t
    while time.time() < e:
        d = s.read(4096)
        if d: out += d; e = time.time() + t
        else: time.sleep(0.05)
    return out.decode(errors='replace')

def send(line, t=1.0):
    s.write((line + "\r\n").encode()); s.flush()
    return rd(t)

def wait_for(subs, maxt=20):
    """read until any of subs appears; return accumulated text."""
    out = ''; e = time.time() + maxt
    while time.time() < e:
        out += rd(0.6)
        if any(x in out for x in subs): break
    return out

# --- wake + login ---
s.write(b"\r\n"); time.sleep(0.5)
cur = rd(1.5)
# if already at a CLI/BCM prompt, skip login
if 'BCM.0>' not in cur:
    if 'User:' not in cur and ')#' not in cur and ')>' not in cur:
        cur += send("", 1.0)
    if 'User:' in cur or 'User:' in rd(0.5):
        s.write(b"admin\r\n"); time.sleep(0.8)
        o = wait_for(['Password:', ')>', ')#'], 8)
        if 'Password:' in o:
            s.write(b"\r\n"); time.sleep(1.2)
            wait_for([')>', ')#'], 8)
    # enable
    s.write(b"enable\r\n"); time.sleep(1.0)
    o = wait_for(['Password:', ')#'], 6)
    if 'Password:' in o:
        s.write(b"\r\n"); time.sleep(1.2); wait_for([')#'], 6)
    send("terminal length 0", 1.2)
    # enter bcmsh
    s.write(b"bcmsh\r\n"); time.sleep(1.5)
    o = rd(1.5)
    if 'y/n' in o or '(y/n)' in o:
        s.write(b"y\r\n"); time.sleep(1.5); o += rd(1.5)
    sys.stderr.write("[entered bcmsh]\n")

rd(0.8)
# warm-up throwaway so first real cmd isn't eaten
s.write(b"\r\n"); rd(0.6)

cmds = []
with open(sys.argv[1]) as f:
    for ln in f:
        ln = ln.rstrip("\n")
        if ln.strip() and not ln.strip().startswith('#'):
            cmds.append(ln)

for c in cmds:
    sys.stdout.write("\n##### CMD: %s #####\n" % c)
    s.write((c + "\r\n").encode()); s.flush()
    buf = ''; t0 = time.time()
    while time.time() - t0 < 6:
        d = s.read(4096).decode(errors='replace')
        if d:
            buf += d
            if buf.rstrip().endswith('BCM.0>') or buf.rstrip().endswith(')#'):
                break
        else:
            time.sleep(0.03)
    sys.stdout.write(buf); sys.stdout.flush()

s.close()
