#!/usr/bin/env python3
# Flip boot_slot=B (one-shot 419e clock-fix kernel), reboot, and capture the full
# boot stream over serial. The i2c "Error in transaction" storm prints to the kernel
# console at boot, so we can validate the fix straight from this capture (no login).
import serial, time, sys

dev = '/dev/ttyUSB1'
cap_secs = float(sys.argv[1]) if len(sys.argv) > 1 else 130.0
logf = '/tmp/boot419e.log'

s = serial.Serial(dev, 115200, timeout=1)

def send(cmd, wait=2.0):
    s.reset_input_buffer()
    s.write((cmd + '\n').encode())
    time.sleep(wait)
    return s.read(65536).decode(errors='replace')

# 1) arm slot B (one-shot) and confirm
print(send('fw_setenv boot_slot B', 2.0), end='')
print(send('fw_printenv boot_slot', 2.0), end='')

# 2) reboot
print('>>> issuing reboot <<<')
s.write(b'sync; reboot\n')

# 3) capture the boot
out = []
t0 = time.time()
markers = {}
with open(logf, 'wb') as f:
    while time.time() - t0 < cap_secs:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
            out.append(d)
            txt = d.decode(errors='replace')
            for m in ('Starting kernel', 'Linux version', 'Kernel panic',
                      'login:', 'Welcome to', 'Error in transaction',
                      'Unable to handle', 'datapath UP'):
                if m in txt and m not in markers:
                    markers[m] = round(time.time() - t0, 1)
s.close()

blob = b''.join(out).decode(errors='replace')
nerr = blob.count('Error in transaction')
print('\n==== CAPTURE SUMMARY ====')
print('elapsed:', round(time.time() - t0, 1), 's   log:', logf)
print('Error-in-transaction count this boot:', nerr)
print('milestones (sec):', markers)
