#!/usr/bin/env python3
"""MHCB1S / 小米 BLE Mesh 模组串口观察器。

用途：把「米家 App 点一下」和「模组吐出的字节」一一对上。
默认只监听（模组 TX -> USB-TTL RX，单向），加 --ack 才会回 ok（需要接模组 RX）。

用法:
    python tools/mesh/uart_watch.py COM6                 # 纯监听，打屏
    python tools/mesh/uart_watch.py COM6 logs/light.log  # 同时落盘
    python tools/mesh/uart_watch.py COM6 --ack           # 自动回 ok，能一直看到 down 帧
    python tools/mesh/uart_watch.py COM6 --ack logs/a.log

操作：运行后按回车打一个标记（"我点了一下 App"），Ctrl+C 结束并打印
      每个标记前后 3 秒的帧，直接就是一张对应表。
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺 pyserial：pip install pyserial")

try:
    import msvcrt  # Windows：非阻塞读键盘
except ImportError:
    msvcrt = None

PORT = next((a for a in sys.argv[1:] if not a.startswith("--") and not a.endswith(".log")), "COM6")
ACK = "--ack" in sys.argv
LOG = next((a for a in sys.argv[1:] if a.endswith(".log")), None)
SECS = next((float(a.split("=")[1]) for a in sys.argv[1:] if a.startswith("--secs=")), 0) or None
KEYS = "--no-keys" not in sys.argv      # 后台/管道运行时关掉键盘轮询

t0 = time.time()
logf = open(LOG, "w", encoding="utf-8") if LOG else None
marks = []          # [(t, n)]
buf = bytearray()
frame_log = []      # [(t, dir, text)]


def stamp(t):
    return "[+%7.3f]" % t


def emit(t, direction, text, extra=""):
    line = "%s %s  %s%s" % (stamp(t), direction, text, extra)
    print(line, flush=True)
    frame_log.append((t, direction, text))
    if logf:
        logf.write("t=%.3f dir=%s data=%r\n" % (t, direction, text))


def show(raw):
    """可打印就原样，含二进制就附 hex，两种都不丢。"""
    txt = raw.decode("utf-8", "replace").strip("\r\n")
    if any(not (32 <= b < 127 or b in (9, 10, 13)) for b in raw):
        txt += "   hex=" + raw.hex(" ")
    return txt


def on_frame(raw, t):
    text = show(raw)
    emit(t, "RX", text)
    if ACK and ("get_down" in text or text.startswith("down")):
        # 模组在等 MCU 应答，不回 ok 它重试几次就放弃
        try:
            ser.write(b"ok\r\n")
            emit(time.time() - t0, "TX", "ok")
        except Exception as e:
            emit(time.time() - t0, "TX", "<写失败 %s>" % e)


def summary():
    print("\n" + "=" * 62)
    if not marks:
        if not frame_log:
            print("没有标记也没有帧：链路不通（接线/波特率/模组没上电）。")
            return
        print("没有标记，直接列出全部帧：")
        for t, d, txt in frame_log:
            print("  %7.3fs %s  %s" % (t, d, txt))
        return
    for t, n in marks:
        print("\n--- 标记 #%d  @ %.3fs 的前后窗口 ---" % (n, t))
        hit = [f for f in frame_log if t - 1.0 <= f[0] <= t + 3.0]
        if not hit:
            print("  (窗口内没有帧：要么单向接线只听到了 get_down，要么这条命令走网关没到串口)")
        for ft, d, txt in hit:
            print("  %+7.3fs %s  %s" % (ft - t, d, txt))


ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.bytesize = 8
ser.parity = "N"
ser.stopbits = 1
ser.timeout = 0.05
ser.dtr = False
ser.rts = False
try:
    ser.open()
except Exception as e:
    sys.exit("打不开 %s: %s（端口被占？先关掉串口助手）" % (PORT, e))

print("已连 %s @115200 8N1  模式=%s" % (PORT, "自动回ok" if ACK else "纯监听"))
print("点 App 前按回车打标记，Ctrl+C 结束。\n")

try:
    while True:
        if SECS and time.time() - t0 >= SECS:
            break
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                if raw.strip():
                    on_frame(raw + b"\n", time.time() - t0)
            if len(buf) > 512:      # 没有换行的二进制流，兜底刷出来
                on_frame(bytes(buf), time.time() - t0)
                buf.clear()
        if KEYS and msvcrt and msvcrt.kbhit():
            ch = msvcrt.getwch()
            if ch in ("\r", "\n"):
                n = len(marks) + 1
                t = time.time() - t0
                marks.append((t, n))
                print("%s == MARK #%d ==" % (stamp(t), n), flush=True)
                if logf:
                    logf.write("t=%.3f MARK #%d\n" % (t, n))
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    summary()
    if logf:
        logf.close()
        print("\n已存 %s" % LOG)
