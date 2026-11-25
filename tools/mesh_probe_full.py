#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""全序列探针：PC 扮演米家 App，把配网握手在真机上完整走一遍。

时机：真机重置后 30 分钟配网窗口内（P2_5：3 短按 + 长按 5 秒）。

已知序列（来自真机成功配网的 logcat 写入长度 + 我们 ESP32 上的实测）：
    001C <- 00                 (len=1)
    001C <- 08 01 00           (len=3)   ← R2 未知 ★
    0010 <- A4                 (len=1)   → 0016 回 00 00 04 00 06 <dmtu>
    0016 <- 00 00 05 00 06 <dmtu>        ← R4 未知 ★
    之后 App 发 109B 大包（疑似 2B 通道头 + 认证数据）

本脚本把前 4 步打给真机，**原样录下每一条 notify**，R2/R4 就是我们要的字典。
R4 拿到后，如果它像「请发公钥」，脚本会自动补发 64B 随机公钥试探下一层。

用法: python tools/mesh_probe_full.py [MAC]
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

TARGET = (sys.argv[1] if len(sys.argv) > 1 else "14:D8:81:E6:95:3C").upper()
FE95 = "-0000-1000-8000-00805f9b34fb"
SUBS = ["0010", "0016", "001A", "001B", "001C"]

got = []          # (t, label, bytes)
all_log = []      # 全程记录，不清理


def u16(n):
    return f"0000{n}{FE95}"


def cb(label):
    def _f(_ch, data):
        b = bytes(data)
        got.append((time.time(), label, b))
        all_log.append((time.time(), label, b))
        print(f"    <- NOTIFY [{label}] {len(b)}B  {b.hex(' ')}")
    return _f


async def write_and_wait(c, chr_name, data: bytes, desc: str, wait=2.5):
    print(f"\n>>> {desc}")
    got.clear()
    t0 = time.time()
    try:
        await c.write_gatt_char(u16(chr_name), data, response=False)
    except Exception as e:
        print(f"    write fail: {e}")
        return []
    await asyncio.sleep(wait)
    if not got:
        print("    （无应答）")
        return []
    return [g for g in got]


async def main():
    dev = await BleakScanner.find_device_by_address(TARGET, timeout=20.0)
    if dev is None:
        print("!! 扫不到真机 —— 请确认已做重置手势、处于配网窗口")
        return
    print(f"[*] 连接 {dev.address}")
    async with BleakClient(dev, timeout=30.0) as c:
        present = set()
        for s in c.services:
            if "fe95" in s.uuid:
                for ch in s.characteristics:
                    present.add(ch.uuid[4:8])
        print("[*] 订阅:", ",".join(n for n in SUBS if n in present))
        for n in SUBS:
            if n in present:
                try:
                    await c.start_notify(u16(n), cb(n))
                except Exception as e:
                    print(f"    sub {n} fail: {e}")
        await asyncio.sleep(0.8)

        # 1) 001C <- 00
        r1 = await write_and_wait(c, "001C", b"\x00", "001C <- 00")

        # 2) 001C <- 08 01 00   ★R2
        r2 = await write_and_wait(c, "001C", bytes.fromhex("080100"),
                                  "001C <- 08 01 00", wait=3.0)

        # 3) 0010 <- A4
        r3 = await write_and_wait(c, "0010", b"\xA4", "0010 <- A4")

        # 从 mng 应答里取真机自己的 dmtu
        dmtu = 0x6B
        for _, _, b in r3:
            if len(b) == 6 and b[0] == 0 and b[1] == 0 and b[2] == 4:
                dmtu = b[5]
                print(f"[*] 真机 mng: num={b[4]} dmtu=0x{dmtu:02X}")
                break

        # 4) 0016 <- 00 00 05 00 06 <dmtu>   ★R4
        r4 = await write_and_wait(
            c, "0016", bytes([0, 0, 5, 0, 6, dmtu]),
            f"0016 <- 00 00 05 00 06 {dmtu:02X}", wait=3.5)

        # 5) 若 R4 像「请求更多」，补发一包 64B 假公钥探下一层
        if r4:
            print("\n[*] R4 已拿到，补发 64B 假公钥探下一层")
            fake = bytes([0x15, 0, 0, 0]) + bytes(range(64))
            await write_and_wait(c, "0010", fake, "0010 <- 15 + 64B 假公钥",
                                 wait=3.0)

    print("\n=== 全程 notify 汇总 ===")
    for t, label, b in all_log:
        print(f"  [{label}] {len(b):3d}B  {b.hex(' ')}")


asyncio.run(main())
