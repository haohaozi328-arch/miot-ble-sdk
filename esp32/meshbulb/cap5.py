"""串口日志采集（带自动重连）。C3 在复位/烧录时会重新枚举 USB，
单次 open 会抛 ClearCommError，所以外层循环里重开。"""
import serial, time

PORT = 'COM6'
LOG = 'serial_live5.log'
DEADLINE = time.time() + 1800

f = open(LOG, 'a', encoding='utf8')
f.write('\n===== capture start %s =====\n' % time.strftime('%H:%M:%S'))
f.flush()

while time.time() < DEADLINE:
    try:
        s = serial.Serial(PORT, 115200, timeout=1)
    except Exception:
        time.sleep(1)
        continue
    try:
        while time.time() < DEADLINE:
            d = s.read(4096)
            if d:
                f.write(d.decode('utf8', 'replace'))
                f.flush()
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        f.write('\n[serial lost, reopening]\n')
        f.flush()
        time.sleep(1)
    else:
        break

f.close()
