#!/usr/bin/env python3
"""扫孔：用 reboot 探测哪个孔是模组的 UART RX。

原理：模组收到 reboot 会打印 Reset by Software 并把 uptime 归零。
      某个孔接对了，uptime 就会突然从几百跳到 0 —— 无法伪造的证据。

用法:
    python tools/mesh/pin_sweep.py COM5

操作：把 TTL 的 TX 线依次插到每个候选孔，每个孔停 8 秒以上。
      脚本会在正确的孔上立刻报 「★ 这个孔是 RX ★」。
"""
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺 pyserial：pip install pyserial")

PORT = next((a for a in sys.argv[1:] if not a.startswith("--")), "COM5")
sio = serial.Serial()
sio.port = PORT
sio.baudrate = 115200
sio.timeout = 0.05
sio.dtr = False
sio.rts = False
sio.open()

UP = re.compile(r"\[\s*(\d+)\.\d+\]")
buf = bytearray()
last_up = None
found = False
t0 = time.time()

print("== 扫孔中：把 TTL 的 TX 线逐孔试，每孔停留 8 秒 ==")
print("== 正确的那一个孔会让模组重启，我这边会立刻叫出来 ==\n")

while True:
    try:
        sio.write(b"reboot\r")
    except serial.SerialException:
        time.sleep(0.3)
        try:
            sio.close()
            sio.open()
        except Exception:
            pass

    # 每 8 秒报一次本孔结论
    seg_start = time.time()
    seg_first = None
    while time.time() - seg_start < 8:
        d = sio.read(256)
        if not d:
            continue
        buf.extend(d)
        while b"\n" in buf:
            raw, _, rest = buf.partition(b"\n")
            buf = bytearray(rest)
            text = raw.decode("utf-8", "replace").strip()
            m = UP.search(text)
            if not m:
                continue
            up = int(m.group(1))
            if seg_first is None:
                seg_first = up
            if last_up is not None and up + 2 < last_up:
                print("\n★ 这个孔是 RX ★   uptime %d -> %d  模组重启了！" % (last_up, up), flush=True)
                found = True
            last_up = up
            if "Reset by" in text or text.endswith("start"):
                print("   （看到重启标志: %s）" % text, flush=True)

    if found:
        break
    if seg_first is not None:
        print("[%5.1fs] 本孔 8 秒：uptime %s -> %s，未重启（不是这个孔）"
              % (time.time() - t0, seg_first, last_up), flush=True)
    else:
        print("[%5.1fs] 本孔 8 秒：完全没收到日志（TX 线可能也掉了）"
              % (time.time() - t0), flush=True)
    print("        -> 换个孔插好，我等下一个 8 秒\n", flush=True)

sio.close()
print("\n结论：找到了，把 TTL 的 TX 固定在这个孔上。")
