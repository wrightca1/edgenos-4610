#!/usr/bin/env python3
# Send a command to the AS4610 serial console and print the response.
import serial, time, sys
dev='/dev/ttyUSB1'
cmd=sys.argv[1] if len(sys.argv)>1 else ''
wait=float(sys.argv[2]) if len(sys.argv)>2 else 3.0
s=serial.Serial(dev,115200,timeout=1)
s.reset_input_buffer()
s.write((cmd+'\n').encode())
time.sleep(wait)
out=b''
t=time.time()
while time.time()-t < wait+2:
    d=s.read(8192)
    if d: out+=d
    else:
        if time.time()-t>1: break
s.close()
sys.stdout.write(out.decode(errors='replace'))
