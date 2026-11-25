import serial, time
s = serial.Serial()
s.port = 'COM6'; s.baudrate = 115200; s.timeout = 0.5
s.dtr = False; s.rts = False
s.open()
time.sleep(0.2)
print('opened, dtr=%s rts=%s' % (s.dtr, s.rts))
tot = 0
t = time.time()
while time.time() - t < 3:
    d = s.read(4096)
    if d:
        tot += len(d)
        print(d.decode('utf8', 'replace'), end='')
s.close()
print('\n[bytes=%d]' % tot)
