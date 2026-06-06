#!/usr/bin/env python3
"""
ser_diag.py — drive the ICOS BCM diag shell (bcmsh) over the SERIAL console and
capture command output, auto-feeding the FASTPATH pager. bcmsh is console-only
(telnet is denied), so this is the only way to reach the Broadcom diag shell.

Usage: ser_diag.py "<diag cmd>" ["<diag cmd>" ...]
Assumes ICOS is already logged in OR will log in (admin/blank). Auto-enters bcmsh.
Env: SERPORT (/dev/ttyUSB1).
Output per command delimited by '##### CMD: <c> #####'.
"""
import serial, time, sys, os

PORT = os.environ.get("SERPORT", "/dev/ttyUSB1")
s = serial.Serial(PORT, 115200, timeout=0.2)

PAGER = ("Hit SPACE to continue", "--More--", "(q)uit")

def feed_read(prompt, maxt=40):
    buf = b""; t0 = time.time(); last = time.time()
    while time.time() - t0 < maxt:
        d = s.read(4096)
        if d:
            buf += d; last = time.time()
            tail = buf[-80:].decode(errors="replace")
            if any(p in tail for p in PAGER):
                s.write(b" "); s.flush(); time.sleep(0.03); continue
            if tail.rstrip().endswith(prompt):
                break
        else:
            if time.time() - last > 0.8 and prompt in buf.decode(errors="replace"):
                break
            time.sleep(0.05)
    txt = buf.decode(errors="replace")
    for p in PAGER: txt = txt.replace(p, "")
    return txt

def drain(t=0.8):
    end = time.time() + t
    while time.time() < end:
        if s.read(4096): end = time.time() + t
        else: time.sleep(0.05)

def slow_write(text):
    for ch in text.encode():
        s.write(bytes([ch])); s.flush(); time.sleep(0.008)

def send(line, prompt, maxt=40):
    drain(0.5)                                   # consume any stale prompt/echo
    slow_write("\r"); time.sleep(0.2)            # sacrificial CR — absorbs first-char loss after a pause
    drain(0.3)
    slow_write(line + "\r\n")                    # pace input so the console doesn't drop chars
    time.sleep(0.4)                              # let the echo start before matching
    return feed_read(prompt, maxt)

def main():
    cmds = sys.argv[1:]
    # wake; figure out where we are (could be: login, CLI, or already in bcmsh)
    s.write(b"\r\n"); time.sleep(1.2); cur = s.read(16384).decode(errors="replace")
    if "BCM.0>" in cur:
        # already inside the diag shell -> just use it
        drain(0.8)
        send("", "BCM.0>", 5)                    # warm-up: absorb first-command race
        for c in cmds:
            sys.stdout.write("\n##### CMD: %s #####\n" % c)
            sys.stdout.write(send(c, "BCM.0>", 60)); sys.stdout.flush()
        return
    if "User:" in cur or "login:" in cur.lower():
        s.write(b"admin\r\n"); time.sleep(1.5); s.read(4096)
        s.write(b"\r\n"); time.sleep(1.5); s.read(4096)   # blank pw
    # ensure enable + no pager
    s.write(b"enable\r\n"); time.sleep(1.2); s.read(4096)
    s.write(b"\r\n"); time.sleep(0.8); s.read(4096)
    send("terminal length 0", "#", 6)
    # enter bcmsh
    s.write(b"bcmsh\r\n"); time.sleep(1.5)
    o = s.read(8192).decode(errors="replace")
    if "y/n" in o: s.write(b"y\r\n"); time.sleep(1)
    feed_read("BCM.0>", 8)
    drain(1.0)                                   # fully consume entry banner/prompt
    send("", "BCM.0>", 5)                        # warm-up: absorb the first-command race
    for c in cmds:
        sys.stdout.write("\n##### CMD: %s #####\n" % c)
        sys.stdout.write(send(c, "BCM.0>", 50)); sys.stdout.flush()
    # leave bcmsh cleanly
    s.write(b"exit\r\n"); time.sleep(1)
    s.close()

if __name__ == "__main__":
    main()
