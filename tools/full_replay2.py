#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
full_replay2.py —— PC 扮演 App，带完整流控地走完真机的证书互认证，
                   然后把真 App 的三个后续包（pkg07 证书链 / pkg03 公钥 /
                   pkg08 56B）按协议重放，看真机接下来主动发什么。

与 full_replay.py 的差别（这是上一轮失败的原因）：
  * 发 START 之后**必须等设备回 `00 00 01 01`** 再吐帧；
  * 吐完帧等设备回 `00 00 01 00`（传输完成）；
  * 收到 `00 00 01 05 <帧号列表>` 时按列表**只重传缺的帧**。

真机协议（本次实测）：
  START   dev->app : 00 00 00 <pkg> <帧数> 00
  ACK     app->dev : 00 00 01 01            （请求发下一段/收到 START）
  DONE    app->dev : 00 00 01 00            （这一段收全了）
  NAK     dev->app : 00 00 01 05 + [u16 帧号]*
  数据帧           : [seq][00][≤18B 载荷]

用法（真机需在配网窗口：任意键 3 短按 + 长按 5s）：
    python tools/full_replay2.py --addr 14:D8:81:E6:95:3C
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


class Link:
    """0016 上的一次 BLE 连接 + 收包队列。"""

    def __init__(self, cli, chars):
        self.cli = cli
        self.chars = chars
        self.t0 = time.monotonic()
        self.rx = asyncio.Queue()
        self.all_notify = []

    def _cb(self, ch, data):
        d = bytes(data)
        self.all_notify.append((time.monotonic() - self.t0, ch.uuid[4:8].upper(), d))
        print("     <- %s %7.3fs len=%-3d %s"
              % (ch.uuid[4:8].upper(), time.monotonic() - self.t0, len(d), hx(d)))
        self.rx.put_nowait(d)

    async def sub(self):
        for u in (C10, C16, C1A, C1B, C1C):
            if u in self.chars:
                await self.cli.start_notify(self.chars[u], self._cb)

    async def wr(self, u, data, note="", gap=0.2):
        print("  -> %s %7.3fs %-22s %s"
              % (u[-6:-4].upper(), time.monotonic() - self.t0, note, hx(data)))
        await self.cli.write_gatt_char(self.chars[u], data, response=False)
        if gap:
            await asyncio.sleep(gap)

    async def wait_for(self, pred, timeout, note=""):
        """等一条满足 pred 的 notify，返回它；超时返回 None。"""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                if note:
                    print("     .. 等 %s 超时" % note)
                return None
            try:
                d = await asyncio.wait_for(self.rx.get(), timeout=left)
            except asyncio.TimeoutError:
                if note:
                    print("     .. 等 %s 超时" % note)
                return None
            if pred(d):
                return d

    async def send_packet(self, pkg, blob, gap=0.022, chunk=18):
        """START → 等 ACK → 吐帧 → 等 DONE；遇 NAK 只补缺的帧。"""
        n = (len(blob) + chunk - 1) // chunk
        frames = {}
        for i in range(n):
            frames[i + 1] = blob[i * chunk:(i + 1) * chunk]

        start = bytes([0x00, 0x00, 0x00, pkg, n, 0x00])
        await self.wr(C16, start, "START pkg=%d n=%d" % (pkg, n), 0)

        ack = await self.wait_for(lambda d: d[:4] == b"\x00\x00\x01\x01", 3.0,
                                  "START ACK")
        if ack is None:
            print("     !! 设备没回 START ACK，跳过这一段")
            return False

        for seq in range(1, n + 1):
            await self.wr(C16, bytes([seq, 0x00]) + frames[seq], "seq %d" % seq, gap)

        # 等 DONE / NAK
        for _ in range(6):
            d = await self.wait_for(
                lambda d: d[:4] in (b"\x00\x00\x01\x00", b"\x00\x00\x01\x05"), 3.0,
                "DONE/NAK")
            if d is None:
                return False
            if d[:4] == b"\x00\x00\x01\x00":
                print("     ✔ pkg=%d 传输完成" % pkg)
                return True
            # NAK：d[4:] 是 u16 小端帧号列表
            miss = [d[4 + 2 * i] | (d[5 + 2 * i] << 8) for i in range((len(d) - 4) // 2)]
            print("     ✗ NAK，补发帧 %s" % miss)
            for seq in miss:
                if seq in frames:
                    await self.wr(C16, bytes([seq, 0x00]) + frames[seq], "retx %d" % seq, gap)
        return False


def split_packages(seq_hex):
    """把重放序列（按顺序的 0016 写入）切成 [(pkg, blob)]。
    START 帧形如 00 00 00 <pkg> <n> 00；随后的 [seq][00] 帧按 seq 收进 blob。"""
    pkgs, cur, frames = [], None, {}
    for s in seq_hex:
        b = bytes.fromhex(s.replace(" ", ""))
        if len(b) == 6 and b[:3] == b"\x00\x00\x00":
            if cur is not None and frames:
                pkgs.append((cur, b"".join(frames[k] for k in sorted(frames))))
            cur, frames = b[3], {}
        elif len(b) >= 3 and b[1] == 0x00 and cur is not None:
            frames[b[0]] = b[2:]
    if cur is not None and frames:
        pkgs.append((cur, b"".join(frames[k] for k in sorted(frames))))
    return pkgs


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr")
    ap.add_argument("--replay", default="_cert/app_replay19.json")
    ap.add_argument("--listen", type=float, default=20.0)
    ap.add_argument("--gap", type=float, default=0.022)
    a = ap.parse_args()

    if not a.addr:
        found = await BleakScanner.discover(timeout=8.0, return_adv=True)
        for dev, adv in found.values():
            print("  %-38s %-22s rssi=%s"
                  % (dev.address, (dev.name or "-")[:22], getattr(adv, "rssi", "?")))
        print("\n用 --addr 指定目标")
        return 0

    async with BleakClient(a.addr, timeout=20.0) as cli:
        chars = {c.uuid.lower(): c for s in cli.services for c in s.characteristics}
        Lk = Link(cli, chars)
        await Lk.sub()
        print("[conn] 已连接并订阅\n")

        # ── 1. 走到「真机发完 84B」 ──
        await Lk.wr(C1C, b"\x00", "001C probe", 0.3)
        await Lk.wr(C1C, b"\x08\x01\x00", "001C alive?", 0.3)
        await Lk.wr(C1C, b"\x03", "001C model?", 0.3)
        await Lk.wr(C10, b"\xA4", "0010 A4 login", 0.5)
        await Lk.wr(C16, bytes([0, 0, 5, 0, 6, 0x6B]), "0016 mng_ack")
        await Lk.wr(C16, bytes([0, 0, 5, 1]) + b"\x12" * 16, "0016 test-ack", 0.6)
        await Lk.wr(C10, b"\x40\x00\x00\x00", "0010 LOGIN 0x40", 1.0)

        # pkg01 证书：START(26) → ACK → 26 帧 → DONE
        if await Lk.wait_for(lambda d: d[:4] == b"\x00\x00\x00\x01", 3.0, "cert START") is None:
            print("!! 没拿到证书 START，终止")
            return 1
        await Lk.wr(C16, b"\x00\x00\x01\x01", "ack cert START", 2.5)
        await Lk.wr(C16, b"\x00\x00\x01\x00", "cert DONE", 1.5)

        # pkg03：84B（20B 头 + 64B 公钥）
        if await Lk.wait_for(lambda d: d[:4] == b"\x00\x00\x00\x03", 5.0, "blob START") is None:
            print("!! 没拿到 84B 的 START，终止")
            return 1
        await Lk.wr(C16, b"\x00\x00\x01\x01", "ack blob START", 2.0)
        await Lk.wr(C16, b"\x00\x00\x01\x00", "blob DONE", 0.6)

        # ── 2. 按协议重放 App 的三个包 ──
        seq = json.load(open(a.replay, encoding="utf-8"))
        pkgs = split_packages(seq)
        print("\n[replay] 解析出 %d 个包：%s"
              % (len(pkgs), ["pkg%d/%dB" % (p, len(b)) for p, b in pkgs]))
        for pkg, blob in pkgs:
            print("\n  === 重放 pkg=%d  %dB ===" % (pkg, len(blob)))
            await Lk.send_packet(pkg, blob, gap=a.gap)

        # ── 3. 静听：设备接下来主动发什么 ──
        print("\n[listen] 静听 %.0fs ..." % a.listen)
        end = time.monotonic() + a.listen
        while time.monotonic() < end:
            try:
                d = await asyncio.wait_for(Lk.rx.get(), timeout=max(0.1, end - time.monotonic()))
                if d[:4] == b"\x00\x00\x01\x05":
                    miss = [d[4 + 2 * i] | (d[5 + 2 * i] << 8) for i in range((len(d) - 4) // 2)]
                    print("     ⇒ NAK 要求补帧 %s" % miss)
            except asyncio.TimeoutError:
                break

        print("\n[done] 共收到 %d 条 notify" % len(Lk.all_notify))
        json.dump([[round(t, 3), ch, d.hex()] for t, ch, d in Lk.all_notify],
                  open("logs/replay2_notify.json", "w"), indent=0)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
