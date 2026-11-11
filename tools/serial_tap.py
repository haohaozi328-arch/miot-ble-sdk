#!/usr/bin/env python3
"""常驻串口 tap：一次打开，持续落盘，顺便支持发命令。

用法：
    python tools/serial_tap.py [PORT] [LOGFILE] [CMDFILE]

为什么需要它：ESP32-C3 走 USB-Serial-JTAG，**每次 open() 都会复位芯片**。
现场测试必须只开一次、一直不关，否则 App 连着连着就断。
发命令不重新开串口 —— 往 CMDFILE 写一行，这个进程替你写进串口。
"""
import os
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
logf = sys.argv[2] if len(sys.argv) > 2 else "_serial.log"
cmdf = sys.argv[3] if len(sys.argv) > 3 else "_serial_cmd.txt"

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
s.reset_input_buffer()

f = open(logf, "ab", buffering=0)
f.write(("\n===== tap start %s =====\n" % time.strftime("%H:%M:%S")).encode())

try:
    while True:
        data = s.read(4096)
        if data:
            f.write(data)
        if os.path.exists(cmdf):
            try:
                txt = open(cmdf, encoding="utf-8").read().strip()
            except OSError:
                txt = ""
            if txt:
                s.write((txt + "\n").encode())
                s.flush()
                f.write(("\n>>> %s\n" % txt).encode())
            os.remove(cmdf)
except KeyboardInterrupt:
    pass
finally:
    f.write(("\n===== tap stop %s =====\n" % time.strftime("%H:%M:%S")).encode())
    f.close()
    s.close()
