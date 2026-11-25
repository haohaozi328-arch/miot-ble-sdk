"""给 ESP32 串口下发命令并回读输出。
用法：python cmd.py COM6 "pid 0x6ABC" "mac AA:BB:.." ...  (逐条发送，每条等 0.6s 收日志)
"""
import sys, time, serial

port = sys.argv[1]
cmds = sys.argv[2:]

s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3)
s.reset_input_buffer()

def drain(sec):
    end = time.time() + sec
    buf = b''
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
    return buf.decode('utf-8', 'replace')

# 先清掉启动残留
drain(1.0)

for c in cmds:
    s.write((c + '\r\n').encode())
    s.flush()
    print(f'>>> {c}')
    out = drain(1.2)
    for line in out.splitlines():
        if line.strip():
            print('   ', line.rstrip())
    print()

s.close()
