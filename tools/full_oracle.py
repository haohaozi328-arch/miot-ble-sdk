#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
full_oracle.py —— PC 扮演米家 App，把整条认证链原样重放给真机，逐包抓回应。

为什么需要：
  ESP32 现在能走到 0x40（LOGIN），回了 E1 00 00 00 之后 App 就断。要判断到底
  是 E1 错了、还是设备 identity 包内容不对，只能问真机：同样的输入，真机输出什么。

重放的序列（与 ESP32 固件当前行为逐字节一致，方便左右对照）：
  001C: 00 / 08 01 00 / 03
  0010: A4
  0016: 00 00 05 00 06 12          (mng_ack，回应设备发来的 mng)
  0016: 00 00 05 01 12*16          (App 对 20B 建链测试包的回执)
  0010: 40 00 00 00                (LOGIN)

用法（真机需在配网窗口：任意键 3 短按 + 长按 5s）：
    python tools/full_oracle.py --addr 14:D8:81:E6:95:3C
    python tools/full_oracle.py --addr XX --fast     # 不按时间轴等，全速发
"""
import argparse
import asyncio
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("需要 bleak：python -m pip install bleak")

U = "0000%s-0000-1000-8000-00805f9b34fb"
C10, C16, C1A, C1B, C1C = (U % x for x in ("0010", "0016", "001a", "001b", "001c"))


def hx(b):
    return " ".join("%02X" % x for x in b)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr")
    ap.add_argument("--fast", action="store_true")
    a = ap.parse_args()

    if not a.addr:
        found = await BleakScanner.discover(timeout=8.0, return_adv=True)
        for dev, adv in found.values():
            print("  %-38s %-22s rssi=%s" % (dev.address, (dev.name or "-")[:22], getattr(adv, "rssi", "?")))
        print("\n用 --addr 指定目标")
        return 0

    t0 = None
    def on_ntf(ch, data):
        print("     <- %s %7.3fs len=%-3d %s" % (ch.uuid[4:8].upper(), time.monotonic() - t0,
                                                  len(data), hx(bytes(data))))

    async with BleakClient(a.addr, timeout=20.0) as cli:
        t0 = time.monotonic()
        print("[conn] mtu=%s" % getattr(cli, "mtu_size", "?"))
        chars = {c.uuid.lower(): c for s in cli.services for c in s.characteristics}
        for u in (C10, C16, C1A, C1B, C1C):
            if u in chars:
                await cli.start_notify(chars[u], on_ntf)
        print("[sub] 0010/0016/001A/001B/001C 全部订阅\n")

        wait = (lambda s: asyncio.sleep(0.1)) if a.fast else (lambda s: asyncio.sleep(s))

        async def wr(u, data, note, gap=0.35):
            print("  -> %s %7.3fs %-22s %s" % (u[-6:-4].upper(), time.monotonic() - t0, note, hx(data)))
            await cli.write_gatt_char(chars[u], data, response=False)
            await wait(gap)

        await wr(C1C, b"\x00", "001C probe")
        await wr(C1C, b"\x08\x01\x00", "001C alive?")
        await wr(C1C, b"\x03", "001C model?")
        await wr(C10, b"\xA4", "0010 A4 login", 0.5)
        await wr(C16, bytes([0, 0, 5, 0, 6, 0x12]), "0016 mng_ack", 0.5)
        await wr(C16, bytes([0, 0, 5, 1]) + b"\x12" * 16, "0016 test-ack", 0.6)
        await wr(C10, b"\x40\x00\x00\x00", "0010 LOGIN 0x40", 1.2)
        # 真机对 40 的回应是 START 帧 00 00 00 01 1A 00（要发 26B），
        # 必须回 ag_ack 它才把数据吐出来。
        print("  -> 0016 ag_ack            00 00 01 01")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x01]), response=False)
        # 等 26 帧收完，再补 App 的「传输完成」ACK（真 App 在收完后 40ms 就发它），
        # 然后静听 12s：看真机在这之后会不会主动发下一包（证书之后的握手）
        await asyncio.sleep(2.0)
        print("  -> 0016 transfer-done     00 00 01 00")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x00]), response=False)
        await asyncio.sleep(12.0)

        try:
            await cli.disconnect()
        except Exception:
            pass
    print("\n[done]")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
