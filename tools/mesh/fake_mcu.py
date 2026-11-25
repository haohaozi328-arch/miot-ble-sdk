#!/usr/bin/env python3
"""假 MCU：按小米《模组串口指令手册》跟 BLE Mesh 模组对话。

流程（手册原文顺序）：
  1. MCU 上电第一时间 `model <model>`     -> ok
  2. 紧接着 `mcu_version <4位>`            -> ok
  3. 以 200ms 周期轮询 `get_down`          -> `down none` 或 `down <方法> <参数...>`
  4. 收到 down 后 1s 内必须回 result / error
  5. 模组自己的调试日志（wd feed / Heap left）会混在协议行里，自动忽略

用法:
    python tools/mesh/fake_mcu.py COM5
    python tools/mesh/fake_mcu.py COM5 --model yyzn.light.wy0a04 --secs=180
    python tools/mesh/fake_mcu.py COM5 --reply 'result 1 1 0'   # 自定义应答

接线（双向，3.3V）：
    模组 TX -> TTL RX     模组 RX -> TTL TX     GND 共地
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺 pyserial：pip install pyserial")

PORT = next((a for a in sys.argv[1:] if not a.startswith("--") and not a.endswith(".log")), "COM5")
MODEL = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--model=")), "yyzn.light.wy0a04")
MCU_VER = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--ver=")), "0001")
REPLY = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--reply=")), "result")
SECS = float(next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--secs=")), 0) or 180)
LOG = next((a for a in sys.argv[1:] if a.endswith(".log")), None)
POLL = 0.2

t0 = time.time()
logf = open(LOG, "w", encoding="utf-8") if LOG else None
sio = serial.Serial()
sio.port = PORT
sio.baudrate = 115200
sio.timeout = 0.02
sio.dtr = False
sio.rts = False
try:
    sio.open()
except Exception as e:
    sys.exit("打不开 %s: %s" % (PORT, e))


def log(direction, text):
    t = time.time() - t0
    line = "[+%7.3f] %s %s" % (t, direction, text)
    print(line, flush=True)
    if logf:
        logf.write(line + "\n")
        logf.flush()


def send(cmd):
    try:
        sio.write(cmd.encode() + b"\r")
    except serial.SerialException as e:
        # 廉价 CH340 跑久了会卡死写队列，重开一次端口继续
        log("ERR", "写失败(%s)，重开端口" % e)
        try:
            sio.close()
        except Exception:
            pass
        time.sleep(0.5)
        try:
            sio.open()
        except Exception as e2:
            log("ERR", "重开失败 %s" % e2)
            return
    log("TX", cmd)


# 协议行判定：只认这几种前缀，其余当模组日志扔掉
KNOWN = ("ok", "error", "down", "result", "up", "version")
buf = bytearray()
booted = False          # 检测到模组上电/重启，要在开机窗口内立刻重新握手


def read_lines():
    """按 \\r 或 \\n 切完整行；不做 readline，避免超时把一行截成两半。"""
    global buf, booted
    d = sio.read(512)
    if d:
        buf.extend(d)
    out = []
    while True:
        cut = [i for i in (buf.find(b"\r"), buf.find(b"\n")) if i >= 0]
        if not cut:
            break
        i = min(cut)
        raw = bytes(buf[:i])
        del buf[:i + 1]
        text = raw.decode("utf-8", "replace").strip()
        if not text:
            continue
        if text.split(" ")[0] in KNOWN:
            log("RX", text)
            out.append(text)
        else:
            log("LOG", text[:120])   # 模组固件日志，留着看它有没有重启
            # 模组上电约 11s 会 close uart，握手必须抢进这个窗口
            if ("Firmware for app" in text or "Reset by" in text
                    or text.endswith("start")):
                booted = True
    return out


print("== 假 MCU 启动 model=%s mcu_version=%s poll=%.0fms ==" % (MODEL, MCU_VER, POLL * 1000))
send("model " + MODEL)
send("mcu_version " + MCU_VER)

down_count = 0
next_poll = time.time()
next_hs = time.time() + 1.0
hs_ok = False
while time.time() - t0 < SECS:
    got = read_lines()
    if booted:
        booted = False
        hs_ok = False
        print("      ### 检测到模组上电，立刻重新握手 ###", flush=True)
        send("model " + MODEL)
        send("mcu_version " + MCU_VER)
        next_hs = time.time() + 1.0
    if not hs_ok and time.time() >= next_hs:
        # 手册要求：没收到 ok 就重发，否则模组一直当没有 MCU
        send("model " + MODEL)
        send("mcu_version " + MCU_VER)
        next_hs = time.time() + 1.0
    for line in got:
        if line == "ok" and not hs_ok:
            hs_ok = True
            print("      ### 握手成功，模组已认下 MCU ###", flush=True)
        if line.startswith("down"):
            body = line[4:].strip()
            if body in ("", "none"):
                continue
            # 真·下行命令：立刻应答，1s 内不回模组就算失败
            down_count += 1
            print("      ^^^ 第 %d 条下行命令: %s" % (down_count, body), flush=True)
            send(REPLY)
            next_poll = time.time() + POLL
    if time.time() >= next_poll:
        send("get_down")
        next_poll = time.time() + POLL

print("\n== 结束：共收到 %d 条下行命令 ==" % down_count)
sio.close()
if logf:
    logf.close()
    print("日志: %s" % LOG)
