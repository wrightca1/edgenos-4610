#!/usr/bin/env python3
"""
icos_capture.py — drive the ICOS (FASTPATH) CLI + BCM diag shell over telnet and
capture command output cleanly to stdout. Much more reliable than serial scraping.

Usage:
  icos_capture.py cli  "<cli cmd>" [more...]   -> run CLI commands
  icos_capture.py bcm  "<diag cmd>" [more...]  -> run diag (bcmsh) commands

Env: ICOS_HOST (10.1.1.209), ICOS_USER (admin), ICOS_PASS ("" blank).
Each command's output is delimited by '##### CMD: <cmd> #####' markers.
"""
import telnetlib, time, sys, os

HOST = os.environ.get("ICOS_HOST", "10.1.1.209")
USER = os.environ.get("ICOS_USER", "admin")
PASS = os.environ.get("ICOS_PASS", "")

PAGER_MARKS = ("Hit SPACE to continue", "--More--", "(q)uit", "Press <SPACE>")

def read_until_prompt(tn, prompt, maxt=60, quiet=0.6):
    """Poll read_very_eager until `prompt` seen at tail; auto-feed any pager."""
    buf = ""
    t0 = time.time(); last = time.time()
    while time.time() - t0 < maxt:
        chunk = tn.read_very_eager().decode(errors="replace")
        if chunk:
            buf += chunk; last = time.time()
            tail = buf[-80:]
            if any(m in tail for m in PAGER_MARKS):
                tn.write(b" ")          # advance pager one page
                time.sleep(0.05)
                continue
            if buf.rstrip().endswith(prompt):
                break
        else:
            if prompt in buf and time.time() - last > quiet:
                break
            time.sleep(0.1)
    # strip pager artifacts
    for m in PAGER_MARKS:
        buf = buf.replace(m, "")
    return buf

def cmd(tn, c, prompt, maxt=30):
    tn.read_very_eager()
    sys.stdout.write("\n##### CMD: %s #####\n" % c)
    tn.write(c.encode() + b"\n")
    out = read_until_prompt(tn, prompt, maxt)
    sys.stdout.write(out); sys.stdout.flush()
    return out

def main():
    mode = sys.argv[1]; cmds = sys.argv[2:]
    tn = telnetlib.Telnet(HOST, 23, timeout=15)
    read_until_prompt(tn, "User:", 12); tn.write(USER.encode() + b"\n")
    read_until_prompt(tn, "Password:", 8); tn.write(PASS.encode() + b"\n")
    time.sleep(1.5); tn.read_very_eager()
    tn.write(b"enable\n"); time.sleep(1.5)
    o = tn.read_very_eager().decode(errors="replace")
    if "Password" in o:
        tn.write(b"\n"); time.sleep(1)
    tn.read_very_eager()
    tn.write(b"terminal length 0\n"); read_until_prompt(tn, "#", 6)

    if mode == "cli":
        for c in cmds: cmd(tn, c, "#", 30)
    elif mode == "bcm":
        tn.write(b"bcmsh\n"); time.sleep(1.5)
        o = tn.read_very_eager().decode(errors="replace")
        if "y/n" in o: tn.write(b"y\n")
        read_until_prompt(tn, "BCM.0>", 8)
        for c in cmds: cmd(tn, c, "BCM.0>", 35)
        tn.write(b"exit\n"); read_until_prompt(tn, "#", 6)
    tn.write(b"logout\n"); time.sleep(0.5); tn.close()

if __name__ == "__main__":
    main()
