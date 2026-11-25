#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
full_replay.py —— PC 扮演 App，把整条链走到「设备发完 84B」，
                 再把**真 App 的后续 34 包原样重放**给真机，看真机回什么。

目的：ESP32 现在能走到 40 → 证书 → 84B(ECDH 公钥)，App 随后发来
      证书(pkg7) + 公钥(pkg3) + 64B(pkg8)，然后就干等 21 秒断开。
      我们要知道：**设备在收到这三个包之后，应该发什么**。
      拿真机当字典：同样的输入，真机输出什么，我们照抄。

用法（真机需在配网窗口：任意键 3 短按 + 长按 5s）：
    python tools/full_replay.py --addr 14:D8:81:E6:95:3C
"""
import argparse
import asyncio
import json
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
    ap.add_argument("--replay", default="_cert/app_replay19.json")
    ap.add_argument("--listen", type=float, default=15.0)
    a = ap.parse_args()

    if not a.addr:
        found = await BleakScanner.discover(timeout=8.0, return_adv=True)
        for dev, adv in found.values():
            print("  %-38s %-22s rssi=%s" % (dev.address, (dev.name or "-")[:22], getattr(adv, "rssi", "?")))
        print("\n用 --addr 指定目标")
        return 0

    t0 = None
    ntf = []

    def on_ntf(ch, data):
        d = bytes(data)
        ntf.append((time.monotonic() - t0, ch.uuid[4:8].upper(), d))
        print("     <- %s %7.3fs len=%-3d %s" % (ch.uuid[4:8].upper(), time.monotonic() - t0, len(d), hx(d)))

    async with BleakClient(a.addr, timeout=20.0) as cli:
        t0 = time.monotonic()
        print("[conn] mtu=%s" % getattr(cli, "mtu_size", "?"))
        chars = {c.uuid.lower(): c for s in cli.services for c in s.characteristics}
        for u in (C10, C16, C1A, C1B, C1C):
            if u in chars:
                await cli.start_notify(chars[u], on_ntf)
        print("[sub] 全部订阅\n")

        async def wr(u, data, note, gap=0.35):
            print("  -> %s %7.3fs %-24s %s" % (u[-6:-4].upper(), time.monotonic() - t0, note, hx(data)))
            await cli.write_gatt_char(chars[u], data, response=False)
            await asyncio.sleep(gap)

        await wr(C1C, b"\x00", "001C probe")
        await wr(C1C, b"\x08\x01\x00", "001C alive?")
        await wr(C1C, b"\x03", "001C model?")
        await wr(C10, b"\xA4", "0010 A4 login", 0.5)
        await wr(C16, bytes([0, 0, 5, 0, 6, 0x12]), "0016 mng_ack", 0.5)
        await wr(C16, bytes([0, 0, 5, 1]) + b"\x12" * 16, "0016 test-ack", 0.6)
        await wr(C10, b"\x40\x00\x00\x00", "0010 LOGIN 0x40", 1.2)

        print("  -> 0016 ag_ack            00 00 01 01")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x01]), response=False)
        await asyncio.sleep(2.0)
        print("  -> 0016 transfer-done     00 00 01 00")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x00]), response=False)

        # 等真机发起第二段（START 00 00 00 03 05 00），ACK 之后收 84B
        await asyncio.sleep(2.5)
        print("  -> 0016 2nd-start ack     00 00 01 01")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x01]), response=False)
        await asyncio.sleep(1.2)
        print("  -> 0016 transfer-done     00 00 01 00")
        await cli.write_gatt_char(chars[C16], bytes([0x00, 0x00, 0x01, 0x00]), response=False)
        await asyncio.sleep(1.5)

        # ---- 重放真 App 的后续 34 包 ----
        seq = json.load(open(a.replay, encoding="utf-8"))
        print("\n[replay] 重放 App 后续 %d 包（真机接下来该说什么？）" % len(seq))
        for i, s in enumerate(seq):
            d = bytes.fromhex(s.replace(" ", ""))
            print("  -> 0016 %7.3fs #%-2d %s" % (time.monotonic() - t0, i, hx(d)))
            await cli.write_gatt_char(chars[C16], d, response=False)
            await asyncio.sleep(0.05)

        print("\n[listen] 静听 %.0fs，抓真机主动发出的下一包 ..." % a.listen)
        await asyncio.sleep(a.listen)

        try:
            await cli.disconnect()
        except Exception:
            pass
    print("\n[done] 共收到 %d 条 notify" % len(ntf))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
