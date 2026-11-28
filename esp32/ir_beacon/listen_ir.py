import sys, serial
port = sys.argv[1] if len(sys.argv) > 1 else 'COM6'
s = serial.Serial(port, 115200, timeout=0.5)
print(f'[listen] {port} @115200', flush=True)
while True:
    data = s.readline()
    if data:
        sys.stdout.write(data.decode('utf-8', 'replace'))
        sys.stdout.flush()
