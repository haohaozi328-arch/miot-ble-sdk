#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
after_cert_oracle.py —— 抓真机「证书传完之后」的第二段传输。

已确认的时序（2026-11-19 23:08，真机 14:D8:81:E6:95:3C）：
  0010 <- 40 00 00 00          (App 发 LOGIN)
  0016 -> 00 00 00 01 1A 00    (START：26 帧 / 451B 证书)
  0016 <- 00 00 01 01          (App ACK)
  0016 -> 01 00 … 1A 00 AC     (26 帧)
  0016 <- 00 00 01 00          (App 传输完成 ACK)
  0016 -> 00 00 00 03 05 00    (★第二段 START —— ESP32 缺的就是这一包之后的对话)

本脚本跑完前半段后，对第二段 START 依次尝试几种 ACK 形态，哪种能让真机开口就记哪种。

用法：python tools/after_cert_oracle.py --addr 14:D8:81:E6:95:3C
"""
import argparse
import asyncio
import sys
import time

try:
    from bleak import BleakClient
except ImportError:
    sys.exit("需要 bleak")

U = "0000%s-0000-1000-8000-00805f9b34fb"
C10, C16, C1A, C1B, C1C = (U % x for x in ("0010", "0016", "001a", "001b", "001c"))


def hx(b):
    return " ".join("%02X" % x for x in b)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True)
    a = ap.parse_args()

    t0 = 0.0
    got = []

    def on_ntf(ch, data):
        b = bytes(data)
        got.append(b)
        print("     <- %-4s %7.3fs len=%-3d %s" % (ch.uuid[4:8].upper(), time.monotonic() - t0, len(b), hx(b)))

    async with BleakClient(a.addr, timeout=20.0) as cli:
        t0 = time.monotonic()
        print("[conn] mtu=%s" % getattr(cli, "mtu_size", "?"))
        chars = {c.uuid.lower(): c for s in cli.services for c in s.characteristics}
        for u in (C10, C16, C1A, C1B, C1C):
            if u in chars:
                await cli.start_notify(chars[u], on_ntf)

        async def wr(u, data, note, gap=0.4):
            print("  -> %-4s %7.3fs %-22s %s" % (u[-6:-4].upper(), time.monotonic() - t0, note, hx(data)))
            await cli.write_gatt_char(chars[u], data, response=False)
            await asyncio.sleep(gap)

        # ── 前半段：和 App 完全一致 ──
        await wr(C1C, b"\x00", "001C probe")
        await wr(C1C, b"\x08\x01\x00", "001C alive?")
        await wr(C1C, b"\x03", "001C model?")
        await wr(C10, b"\xA4", "0010 A4")
        await wr(C16, bytes([0, 0, 5, 0, 6, 0x12]), "0016 mng_ack")
        await wr(C16, bytes([0, 0, 5, 1]) + b"\x12" * 16, "0016 test-ack", 0.6)
        await wr(C10, b"\x40\x00\x00\x00", "0010 LOGIN", 1.2)

        print("  -> 0016 ag_ack            00 00 01 01")
        await cli.write_gatt_char(chars[C16], bytes([0, 0, 1, 1]), response=False)
        await asyncio.sleep(2.5)
        print("  -> 0016 transfer-done     00 00 01 00")
        await cli.write_gatt_char(chars[C16], bytes([0, 0, 1, 0]), response=False)
        await asyncio.sleep(1.5)

        # ── 第二段：试几种 ACK 形态 ──
        for ack in ([0, 0, 1, 1], [0, 0, 1, 3], [0, 0, 2, 3], [0, 0, 0, 2]):
            got.clear()
            print("  -> 0016 2nd-start ack     %s" % hx(bytes(ack)))
            await cli.write_gatt_char(chars[C16], bytes(ack), response=False)
            await asyncio.sleep(2.5)
            n = len(got)
            print("     (2.5s 内收到 %d 包)" % n)
            if n:
                print("     ★ 这个 ACK 有效：" + hx(bytes(ack)))
                await asyncio.sleep(4.0)
                break

        try:
            await cli.disconnect()
        except Exception:
            pass
    print("\n[done]")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
