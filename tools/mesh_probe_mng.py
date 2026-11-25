#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""问真机：MNG(0x04) 之后 App 回 0x05，设备该答什么？

背景（2026-11-19 实测我们自己的 ESP32）：
    0010 <- A4                 ⇒ 0016 回 00 00 04 00 06 12   （mng，code=0x04）
    0016 <- 00 00 05 00 06 12  ⇒ ？（App 把 code +1 回写，我们没答 → App 放弃）

这里的 6 字节帧格式看着就是 [00 00][code][00][num][dmtu]。真机的 num/dmtu
是 06/6b，所以本脚本对真机发 00 00 05 00 06 6b，看它回什么。

用法: python tools/mesh_probe_mng.py [MAC]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

TARGET = (sys.argv[1] if len(sys.argv) > 1 else "14:D8:81:E6:95:3C").upper()
FE95 = "-0000-1000-8000-00805f9b34fb"
SUBS = ["0010", "0016", "001A", "001B", "001C"]

got = []


def u16(n):
    return f"0000{n}{FE95}"


def cb(label):
    def _f(_ch, data):
        b = bytes(data)
        got.append((label, b))
        print(f"    <- NOTIFY [{label}] {len(b)}B  {b.hex(' ')}")
    return _f


async def step(c, name, label, timeout=2.5):
    print(f"\n>>> {label}")
    got.clear()
    try:
        await c.write_gatt_char(u16(name), bytes.fromhex(label.split("<-")[-1].strip()),
                                response=False)
    except Exception as e:
        print(f"    write fail: {e}")
        return
    await asyncio.sleep(timeout)
    if not got:
        print("    （无应答）")


async def main():
    dev = await BleakScanner.find_device_by_address(TARGET, timeout=20.0)
    if dev is None:
        print("!! 扫不到真机")
        return
    async with BleakClient(dev, timeout=30.0) as c:
        present = set()
        for s in c.services:
            if "fe95" in s.uuid:
                for ch in s.characteristics:
                    present.add(ch.uuid[4:8])
        for n in SUBS:
            if n in present:
                try:
                    await c.start_notify(u16(n), cb(n))
                except Exception as e:
                    print(f"    sub {n} fail {e}")
        await asyncio.sleep(0.8)

        # 1) 建链：A4
        await step(c, "0010", "0010 <- A4")

        # 2) 关键一问：App 在 0016 上的 code=0x05 回写，设备答什么
        await step(c, "0016", "0016 <- 00 00 05 00 06 6B")

        # 3) 万一真机按自己的 dmtu 校验，再试我们这边的 0x12
        await step(c, "0016", "0016 <- 00 00 05 00 06 12")

        # 4) 试探递增猜测：code=0x06 会不会是 App 期待的下一步
        await step(c, "0016", "0016 <- 00 00 06 00 06 6B")

    print("\n=== 完成 ===")


asyncio.run(main())
