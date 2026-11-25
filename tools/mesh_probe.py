#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把真机（MHCB12G / xry.switch.sw4b01）当协议字典用。

真机处于配网窗口时（P2_5 手势 3 短 + 5 秒长按后 30 分钟内），
PC 直连上去，扮演米家 App 走一遍握手，把设备的每一条 notify 原样录下来。

目的是搞清两件事：
  1) 配网窗口内真机的 GATT 表和「已入网」时的表是否不同
  2) 往 001C 写 0x00 后，设备回什么（我们缺的就是这个应答）

用法: python tools/mesh_probe.py [MAC]
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

TARGET = (sys.argv[1] if len(sys.argv) > 1 else "14:D8:81:E6:95:3C").upper()
FE95 = "-0000-1000-8000-00805f9b34fb"

# 米家配网时实际订阅的 5 条
SUB_UUIDS = ["0000001b", "0000001a", "0000001c", "00000016", "00000010"]

notifies = []  # (uuid, bytes)


def u16(name: str) -> str:
    return f"0000{name}{FE95}"


def on_notify(label: str):
    def cb(_ch, data: bytearray):
        b = bytes(data)
        notifies.append((label, b))
        print(f"    <- NOTIFY [{label}] {len(b)}B  {b.hex(' ')}")
    return cb


async def main():
    print(f"[1] 扫描 {TARGET} ...")
    dev = await BleakScanner.find_device_by_address(TARGET, timeout=20.0)
    if dev is None:
        print("!! 扫不到设备。请先对真机做重置手势（P2_5：3 短按 + 长按 5 秒），"
              "确认进入配网窗口后重试。")
        return

    print(f"[2] 连接 {dev.address} ...")
    async with BleakClient(dev, timeout=30.0) as c:
        # ---- 完整服务表 ----
        print("\n[3] GATT 表（配网窗口内）")
        present = set()
        for s in c.services:
            if "fe95" not in s.uuid:
                continue
            print(f"  [service] {s.uuid}")
            for ch in s.characteristics:
                short = ch.uuid[4:8]
                present.add(short)
                print(f"      {short}  h={ch.handle}  ({','.join(ch.properties)})")

        # ---- 版本串 ----
        print("\n[4] 读 0004（版本串）")
        try:
            v = await c.read_gatt_char(u16("0004"))
            print(f"    {len(v)}B: {bytes(v).hex()}  ascii={bytes(v)!r}")
        except Exception as e:
            print(f"    read fail: {e}")

        # ---- 订阅 ----
        print("\n[5] 订阅 notify 通道")
        for name in SUB_UUIDS:
            if name[4:] not in present:
                print(f"    {name[4:]} 不存在，跳过")
                continue
            try:
                await c.start_notify(u16(name[4:]), on_notify(name[4:]))
                print(f"    {name[4:]} subscribed")
            except Exception as e:
                print(f"    {name[4:]} subscribe fail: {e}")
        await asyncio.sleep(0.8)

        # ---- 探针 A：001C 写 0x00（App 的第一步，我们缺的应答） ----
        print("\n[6] 探针 A: 001C <- 00")
        notifies.clear()
        try:
            await c.write_gatt_char(u16("001C"), b"\x00", response=False)
        except Exception as e:
            print(f"    write fail: {e}")
        await asyncio.sleep(2.0)
        if not notifies:
            print("    无任何 notify 应答")

        # ---- 探针 B：0010 写 A4（普通 BLE 的 MNG 查询） ----
        print("\n[7] 探针 B: 0010 <- A4")
        notifies.clear()
        try:
            await c.write_gatt_char(u16("0010"), b"\xA4", response=False)
        except Exception as e:
            print(f"    write fail: {e}")
        await asyncio.sleep(2.0)
        if not notifies:
            print("    无任何 notify 应答")

        # ---- 探针 C：0010 写 40 00 00 00（App 卡住的那一步） ----
        print("\n[8] 探针 C: 0010 <- 40 00 00 00")
        notifies.clear()
        try:
            await c.write_gatt_char(u16("0010"), b"\x40\x00\x00\x00", response=False)
        except Exception as e:
            print(f"    write fail: {e}")
        await asyncio.sleep(3.0)
        if not notifies:
            print("    无任何 notify 应答")

        # ---- 探针 D：001C 写 0x01（备用起始值） ----
        print("\n[9] 探针 D: 001C <- 01")
        notifies.clear()
        try:
            await c.write_gatt_char(u16("001C"), b"\x01", response=False)
        except Exception as e:
            print(f"    write fail: {e}")
        await asyncio.sleep(2.0)
        if not notifies:
            print("    无任何 notify 应答")

    print("\n=== 完成 ===")


asyncio.run(main())
