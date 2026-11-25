#!/usr/bin/env python3
"""持续抓串口日志（自动重连、不拉 DTR/RTS），用于长时间观察固件事件。

用法: python tools/serial_tail.py COM6 1500 logs/selfid2.log
      python tools/serial_tail.py COM6 1500            # 直接打屏
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600
out = open(sys.argv[3], "w", encoding="utf-8") if len(sys.argv) > 3 else sys.stdout

t0 = time.time()
while time.time() - t0 < secs:
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.3
        s.dtr = False
        s.rts = False
        s.open()
        while time.time() - t0 < secs:
            d = s.read(2048)
            if d:
                out.write(d.decode("utf-8", "replace").replace("\r", ""))
                out.flush()
    except Exception:
        time.sleep(0.3)
out.flush()
