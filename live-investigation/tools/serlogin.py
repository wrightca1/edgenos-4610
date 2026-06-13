#!/usr/bin/env python3
# Expect-style serial login + run a command, robust to slow/laggy console.
import serial, time, sys

dev = '/dev/ttyUSB1'
cmd = sys.argv[1] if len(sys.argv) > 1 else 'uname -r'
s = serial.Serial(dev, 115200, timeout=1)

def read_until(tokens, timeout=20):
    buf = ''
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = s.read(4096).decode(errors='replace')
        if d:
            buf += d
            for tok in tokens:
                if tok in buf:
                    return tok, buf
    return None, buf

# wake the console
s.write(b'\r'); time.sleep(1)
# we might already be logged in, or at a login/password prompt
tok, buf = read_until(['login:', 'Password:', '#', '$'], timeout=8)
if tok in ('#', '$'):
    pass  # already a shell
else:
    if tok != 'login:':
        s.write(b'\r'); time.sleep(1)
        tok, buf = read_until(['login:', 'Password:', '#'], timeout=8)
    if tok == 'login:':
        s.write(b'root\r'); time.sleep(1)
        tok, buf = read_until(['Password:', '#'], timeout=8)
    if tok == 'Password:':
        s.write(b'onl\r'); time.sleep(1)
        tok, buf = read_until(['#', '$', 'incorrect', 'Login incorrect'], timeout=10)

# run the command bracketed by markers so we can extract clean output
s.reset_input_buffer()
s.write(b'echo MK1; ' + cmd.encode() + b'; echo MK2\r')
time.sleep(2)
out = ''
t0 = time.time()
while time.time() - t0 < 18:
    d = s.read(8192).decode(errors='replace')
    if d:
        out += d
        if 'MK2' in out and out.count('MK2') >= 1 and 'MK1' in out:
            # ensure we have the trailing output after MK1
            if out.rfind('MK2') > out.find('MK1'):
                break
s.close()
# extract between first MK1 and last MK2
try:
    a = out.index('MK1') + 3
    b = out.rindex('MK2')
    print(out[a:b].strip())
except ValueError:
    print('[no markers] raw tail:\n' + out[-1200:])
