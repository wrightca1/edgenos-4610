import serial, time, sys, re
s = serial.Serial('/dev/ttyUSB1', 115200, timeout=0.3)
def rd(t=0.8):
    o=b'';e=time.time()+t
    while time.time()<e:
        d=s.read(4096)
        if d:o+=d;e=time.time()+t
        else:time.sleep(0.03)
    return o.decode(errors='replace')
# wake / ensure bcmsh
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
rd(0.6)
rx = re.compile(r'Reg 0x001f: (0x[0-9a-f]+)', re.I)
def rdb(addr):
    s.write(('phy ge25 0x1e 0x%x\r\n'%addr).encode()); s.flush(); rd(0.25)
    s.write(b'phy ge25 0x1f\r\n'); s.flush()
    buf=''; t=time.time()
    while time.time()-t<2:
        d=s.read(2048).decode(errors='replace')
        if d:
            buf+=d
            if 'BCM.0>' in buf[-12:]: break
        else: time.sleep(0.02)
    m=rx.search(buf)
    return m.group(1) if m else '????'
ranges=[(0x000,0x040),(0x200,0x240),(0x300,0x310)]
for lo,hi in ranges:
    a=lo
    while a<=hi:
        line='RDB '
        for j in range(8):
            if a+j>hi: break
            line += '%03x:%s '%(a+j, rdb(a+j).replace('0x',''))
        print(line); sys.stdout.flush()
        a+=8
s.close()
