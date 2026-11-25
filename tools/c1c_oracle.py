#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
c1c_oracle.py —— 把真机当 oracle，问出 0x001C 通道的完整应答（PC 端，bleak）

为什么要它：
  我们用 ESP32 冒充 xry.switch.sw4b01，App 连上后往 001C 依次写：
      00            -> 我们回 00 02 03 08（这一包已照真机复刻，App 继续）
      08 01 00      -> 真机回什么？未知。我们沉默 2.5s 后 App 就放弃 Mesh 路径，
                       回落到普通 BLE 认证（A4 -> 40 -> E1），最后报 -17 断开。
  缺的就是 08 01 00 的应答。手机 App 的 dec/enc 我们看不到，但真机可以说。

用法（真机需处于配网窗口：任意键 3 短按 + 长按 5s，松开后 30 分钟内可连）：
    python tools/c1c_oracle.py                      # 扫描，列出候选
    python tools/c1c_oracle.py --addr 14:D8:81:E6:95:3C
    python tools/c1c_oracle.py --addr XX --pdu 00 --pdu "08 01 00" --wait 6
    python tools/c1c_oracle.py --addr XX --sweep     # 内置矩阵：00 / 08 01 00 / 01 / 02 ...

输出：每个 PDU 的写入时刻、每条 notify 的时刻+字节，直接可抄进固件。
"""
import argparse
import asyncio
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("需要 bleak：python -m pip install bleak")

UUID_C1C = "0000001c-0000-1000-8000-00805f9b34fb"
UUID_C1C_CCCD = "00002902-0000-1000-8000-00805f9b34fb"


def hx(b):
    return " ".join("%02X" % x for x in b)


async def scan(timeout=8.0):
    print("[scan] %.0fs ..." % timeout)
    try:
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        items = [(d, adv) for d, adv in found.values()]
    except TypeError:                      # 老版 bleak 没有 return_adv
        items = [(d, None) for d in await BleakScanner.discover(timeout=timeout)]
    for d, adv in sorted(items, key=lambda x: -(getattr(x[1], "rssi", 0) or 0)):
        rssi = getattr(adv, "rssi", None)
        mf = getattr(adv, "manufacturer_data", None) or {}
        extra = ""
        if mf:
            for cid, payload in mf.items():
                extra += " mf=%04X:%s" % (cid, payload.hex())
        print("  %-38s %-24s rssi=%-5s%s" % (d.address, (d.name or "-")[:24], rssi, extra))
    return items


async def run(addr, pdus, wait, gap, dump_all):
    t0 = None
    notes = []

    def on_ntf(_char, data):
        import time
        notes.append((time.monotonic() - t0, bytes(data)))
        print("  <- ntf %7.3fs  len=%-3d  %s" % (time.monotonic() - t0, len(data), hx(data)))

    async with BleakClient(addr, timeout=20.0) as cli:
        import time
        t0 = time.monotonic()
        print("[conn] %s mtu=%s" % (addr, getattr(cli, "mtu_size", "?")))
        svcs = cli.services
        has1c = None
        for s in svcs:
            for c in s.characteristics:
                u = c.uuid.lower()
                if dump_all:
                    print("  chr %s  props=%s" % (u, ",".join(c.properties)))
                if u == UUID_C1C:
                    has1c = c
        if has1c is None:
            print("!! 这台设备没有 001C")
            return 1
        await cli.start_notify(has1c, on_ntf)
        print("[sub] 001C notify on\n")

        for i, p in enumerate(pdus, 1):
            print("[%02d] -> %s   (t=%.3fs)" % (i, hx(p), time.monotonic() - t0))
            await cli.write_gatt_char(has1c, p, response=False)
            await asyncio.sleep(wait if i == len(pdus) else gap)

        print("\n[done] notify 共 %d 条" % len(notes))
        for dt, d in notes:
            print("  %7.3fs  %s" % (dt, hx(d)))
    return 0


def parse_hex(s):
    return bytes(int(x, 16) for x in s.replace(",", " ").split())


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr")
    ap.add_argument("--pdu", action="append", help='可重复，如 --pdu 00 --pdu "08 01 00"')
    ap.add_argument("--wait", type=float, default=6.0, help="最后一个包之后的等待秒数")
    ap.add_argument("--gap", type=float, default=1.0, help="包之间间隔")
    ap.add_argument("--sweep", action="store_true", help="跑内置探测矩阵")
    ap.add_argument("--dump", action="store_true", help="打印全部特征/属性")
    a = ap.parse_args()

    if not a.addr:
        await scan()
        print("\n挑一个 MAC，用 --addr 传进来")
        return 0

    if a.pdu:
        pdus = [parse_hex(x) for x in a.pdu]
    elif a.sweep:
        pdus = [b"\x00", b"\x08\x01\x00", b"\x01", b"\x02", b"\x03",
                b"\x08\x00\x00", b"\x08\x01\x01"]
    else:
        pdus = [b"\x00", b"\x08\x01\x00"]
    return await run(a.addr, pdus, a.wait, a.gap, a.dump)


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
