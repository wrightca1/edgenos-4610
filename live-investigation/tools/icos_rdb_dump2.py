import serial, time, sys, re
s = serial.Serial('/dev/ttyUSB1', 115200, timeout=0.3)
def drain(t=0.5):
    e=time.time()+t
    while time.time()<e:
        if s.read(4096): e=time.time()+t
        else: time.sleep(0.03)
def rd(t=0.8):
    o=b'';e=time.time()+t
    while time.time()<e:
        d=s.read(4096)
        if d:o+=d;e=time.time()+t
        else:time.sleep(0.03)
    return o.decode(errors='replace')
s.write(b'\r\n'); time.sleep(0.5); cur=rd(1.0)
if 'BCM.0>' not in cur:
    if 'User:' in cur or 'User:' in rd(0.4):
        s.write(b'admin\r\n'); time.sleep(1.0); o=rd(2)
        if 'Password' in o: s.write(b'\r\n'); time.sleep(1.2); rd(1.5)
    s.write(b'enable\r\n'); time.sleep(1.0); o=rd(2)
    if 'Password' in o: s.write(b'\r\n'); time.sleep(1.2); rd(1.5)
    s.write(b'terminal length 0\r\n'); time.sleep(0.8); rd(1)
    s.write(b'bcmsh\r\n'); time.sleep(1.5); o=rd(1.5)
    if 'y/n' in o: s.write(b'y\r\n'); time.sleep(1.2); rd(1)
drain(0.6)
rx = re.compile(r'Reg 0x001f:\s*(0x[0-9a-f]+)', re.I)
def rdb(addr):
    drain(0.15)
    s.write(('phy ge25 0x1e 0x%x\r\n'%addr).encode()); s.flush(); time.sleep(0.35); drain(0.2)
    s.write(b'phy ge25 0x1f\r\n'); s.flush(); time.sleep(0.1)
    buf=''; t=time.time()
    while time.time()-t<2.5:
        d=s.read(2048).decode(errors='replace')
        if d:
            buf+=d
            if buf.rstrip().endswith('BCM.0>'): break
        else: time.sleep(0.02)
    m=rx.search(buf)
    return m.group(1)[2:] if m else 'xxxx'
for lo,hi in [(0x000,0x040),(0x200,0x240),(0x300,0x310)]:
    a=lo
    while a<=hi:
        line='RDB '
        for j in range(8):
            if a+j>hi: break
            line += '%03x:%s '%(a+j, rdb(a+j))
        print(line); sys.stdout.flush()
        a+=8
s.close()
