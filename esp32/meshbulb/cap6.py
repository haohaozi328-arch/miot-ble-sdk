"""复位 + 持续采集。开头用 RTS 脉冲硬复位 C3（和 esptool --after hard-reset 同一套
电平序列），这样启动横幅一定能抓到；之后掉线自动重开。"""
import serial, time

PORT = 'COM6'
LOG = 'serial_liveM.log'
DEADLINE = time.time() + 7200

f = open(LOG, 'a', encoding='utf8')
f.write('\n===== capture start %s =====\n' % time.strftime('%H:%M:%S'))
f.flush()

first = True
while time.time() < DEADLINE:
    try:
        s = serial.Serial(PORT, 115200, timeout=0.5)
    except Exception as e:
        f.write('[open fail: %s]\n' % e)
        f.flush()
        time.sleep(1)
        continue
    try:
        if first:
            first = False
            # hard reset: EN 拉低 -> 释放；boot 引脚保持高（正常启动）
            s.setDTR(False)
            s.setRTS(True)
            time.sleep(0.15)
            s.setRTS(False)
            time.sleep(0.05)
            f.write('[reset pulse sent]\n')
            f.flush()
        while time.time() < DEADLINE:
            d = s.read(4096)
            if d:
                f.write(d.decode('utf8', 'replace'))
                f.flush()
    except Exception as e:
        try:
            s.close()
        except Exception:
            pass
        f.write('[serial lost: %s]\n' % e)
        f.flush()
        time.sleep(1)
    else:
        break

f.close()
