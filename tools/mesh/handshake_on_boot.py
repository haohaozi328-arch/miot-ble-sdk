#!/usr/bin/env python3
"""等模组上电（看到 "Reset by Power"）后立刻按 MCU 协议握手。

背景：模组固件在启动 ~12s 时会打印 `close uart` —— 如果这 12s 内没有 MCU
发 `model` / `mcu_version`，它就把 MCU 串口关掉（之后只剩日志，不再应答）。
本脚本盯着日志，每次上电（boot 循环也算）都在 `System Init Succ` 后立刻握手。

用法:
    python tools/mesh/handshake_on_boot.py COM5 --secs=180 --log=x.log
"""
import sys
import time

import serial

PORT = next((a for a in sys.argv[1:] if not a.startswith("--")), "COM5")
SECS = float(next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--secs=")), 0) or 180)
MODEL = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--model=")), "yyzn.light.wy0a04")
_logarg = next((a for a in sys.argv[1:] if a.startswith("--log=")), None)
LOG = _logarg.split("=", 1)[1] if _logarg else None

sio = serial.Serial(PORT, 115200, timeout=0.05)
sio.dtr = False
sio.rts = False
out = open(LOG, "w", encoding="utf-8", buffering=1) if LOG else None


def emit(s):
    print(s, flush=True)
    if out:
        out.write(s + "\n")


def tx(cmd):
    sio.write((cmd + "\r").encode())
    emit(">>> " + cmd)


buf = b""
t0 = time.time()
boot_pending = False       # 看到过 Reset by Power，等 System Init Succ
boots = 0
polling = False
last_poll = 0.0
closed = False

while time.time() - t0 < SECS:
    chunk = sio.read(2048)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").strip()
        if not text:
            continue
        emit(text)
        if "Reset by Power" in text:
            boot_pending = True
            polling = False
        elif "System Init Succ" in text and boot_pending:
            boot_pending = False
            boots += 1
            emit("*** boot #%d：%s0.1s，立刻握手 ***" % (boots, ""))
            sio.reset_input_buffer()
            tx("model " + MODEL)
            time.sleep(0.1)
            tx("mcu_version 0001")
            polling = True
            closed = False
            last_poll = time.time()
        elif "ok" == text or text.startswith("down") or text.startswith("result"):
            emit("★★★★ 模组应答：%s" % text)
        elif "close uart" in text:
            emit("!!! 模组关闭 MCU 串口（本窗口握手未成功）")
            polling = False
            closed = True

    if polling and time.time() - last_poll > 0.2:
        tx("get_down")
        last_poll = time.time()

sio.close()
if out:
    out.close()
