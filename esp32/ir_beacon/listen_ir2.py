"""红外接收监听（自动重连 + 码表标注）

用法: python listen_ir2.py COM6 [--reset]

功能：
  1. ESP32-C3 用原生 USB-Serial-JTAG 做控制台，芯片一复位端口就重新枚举，
     宿主句柄失效 → 这里做自动重连。
  2. 对固件解出的 NEC 码做二次判定（标准 / 16bit 扩展 / 校验失败）。
  3. 对 SIRC 码查 ir_codes.json，把 cmd 翻译成键位名（VOL+/POWER/...），
     未收录的码会标记「未收录」，方便发现码表缺口。

--reset: 打开端口后先通过 DTR/RTS 打一次硬件复位（自检板子是否活着）。
"""
import json
import os
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "COM6"
RESET = "--reset" in sys.argv

PAT_NEC = re.compile(r"code=0x([0-9A-Fa-f]{8})")
PAT_SIRC = re.compile(r"NEW id=\S+ n=\d+\s+SIRC(\d+) dev=0x([0-9A-Fa-f]{2}) cmd=0x([0-9A-Fa-f]{2})")

_HERE = os.path.dirname(os.path.abspath(__file__))
# 码表已迁到 ir_bus/ir_lib/ 作为唯一来源，这里做路径兼容
BOOK_CANDIDATES = [
    os.path.join(_HERE, "..", "ir_bus", "ir_lib", "ir_codes.json"),
    os.path.join(_HERE, "ir_codes.json"),
]


def load_book():
    """读码表：{cmd_int: {"name":..., "zh":..., "note":...}}"""
    for path in BOOK_CANDIDATES:
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            print(f"[listen] 码表: {os.path.normpath(path)}", flush=True)
            return {int(k["cmd"], 16): k for k in data["keys"]}
        except FileNotFoundError:
            continue
        except Exception as exc:
            print(f"[listen] 码表加载失败（{exc}）", flush=True)
            return {}
    print("[listen] 未找到 ir_codes.json，仅显示原始 cmd", flush=True)
    return {}


BOOK = load_book()


def explain_nec(m):
    """把 32bit NEC 码拆成 addr/cmd 并判定标准/扩展 NEC。"""
    code = int(m.group(1), 16)
    # NEC 是 LSB first：byte0=addrL, byte1=addrH, byte2=cmd, byte3=~cmd
    b0 = code & 0xFF
    b1 = (code >> 8) & 0xFF
    b2 = (code >> 16) & 0xFF
    b3 = (code >> 24) & 0xFF
    if b3 != ((~b2) & 0xFF):
        return "  -> 判定: 非法 NEC（cmd 取反位不匹配），可能是别的协议"
    if b1 == ((~b0) & 0xFF):
        return f"  -> 判定: NEC   addr=0x{b0:02X}      cmd=0x{b2:02X}"
    return f"  -> 判定: NEC-EXT addr=0x{(b1 << 8) | b0:04X}  cmd=0x{b2:02X}"


def explain_sirc(m):
    """把 SIRC 的 dev/cmd 翻译成键位名。"""
    bits = int(m.group(1))
    dev = int(m.group(2), 16)
    cmd = int(m.group(3), 16)
    k = BOOK.get(cmd)
    if k is None:
        return f"    ↳ SIRC{bits} dev=0x{dev:02X} cmd=0x{cmd:02X}  ★未收录（码表缺口）"
    tail = f"  ({k['note']})" if k.get("note") else ""
    return f"    ↳ SIRC{bits} dev=0x{dev:02X} cmd=0x{cmd:02X}  = {k['name']} / {k['zh']}{tail}"


def run():
    s = serial.Serial(PORT, 115200, timeout=0.5)
    print(f"[listen] {PORT} @115200 已连接（Ctrl+C 结束）", flush=True)
    if RESET:
        # 通过 USB-Serial-JTAG 的 DTR/RTS 打一次硬件复位：RTS=EN，DTR=IO9(启动模式)
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
        time.sleep(0.15)
        print("[listen] 已发送复位脉冲，等待启动横幅…", flush=True)

    while True:
        line = s.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace")
        sys.stdout.write(text)
        sys.stdout.flush()

        m = PAT_SIRC.search(text)
        if m:
            print(explain_sirc(m), flush=True)
            continue
        m = PAT_NEC.search(text)          # 只在非 SIRC 帧上做 NEC 判定
        if m:
            print(explain_nec(m), flush=True)


while True:
    try:
        run()
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as exc:                      # 端口掉线 / 芯片复位
        print(f"[listen] 串口断开（{exc.__class__.__name__}: {exc}），3s 后重连…", flush=True)
        time.sleep(3)
