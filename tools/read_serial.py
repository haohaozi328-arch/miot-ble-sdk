#!/usr/bin/env python3
"""读一段串口日志（默认 COM6，20 秒）。

用法：
    python tools/read_serial.py [PORT] [SECONDS] [BAUD]

★ESP32-C3 走 USB-Serial-JTAG：**每次打开串口都会把芯片复位**。
  所以不要边读边发命令，也不要频繁开关——设备会不断重启，App 连接会掉。
  需要「读日志 + 发命令」同时进行，就保持一个进程常驻，别反复 open/close。
"""
import sys, time, serial
port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200
s = serial.Serial(port, baud, timeout=0.3)
s.reset_input_buffer()
t0 = time.time()
buf = b""
while time.time() - t0 < dur:
    buf += s.read(4096)
s.close()
txt = buf.decode("utf-8", "replace")
print(txt)
print("=== bytes:", len(buf), "chars:", len(txt))
