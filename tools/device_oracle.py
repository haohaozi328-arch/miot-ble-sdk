#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
device_oracle.py —— 把真机当成「签名 oracle」，逼它吐出真实的 pkg4。

思路
----
已确认：真机只有在**整段会话自洽**时才发 pkg4（重放别的会话的 pkg7/3/8 会被 42 中止）。
那就由我们扮演**服务器**：

  1. 以 App 身份走完 001C / A4 / mng / LOGIN 40
  2. 收下真机 pkg1（451B 证书）+ pkg3（84B：dev_info + manu_cert_id + dev_mesh_pub）
  3. 用**我们自己签发**的 "Mijia Cloud" 证书当 pkg7、自己的临时公钥当 pkg3、
     自己的签名当 pkg8 发给真机
  4. 真机若只验签名不看证书链 → 它会认为服务器可信 → 回 **pkg4（真实签名）**

拿到 pkg4 后即可离线穷举「它到底签了什么」（我们手里有它的 dev_mesh_pub，验签可做）。

用法
----
    python tools/device_oracle.py --addr 14:D8:81:E6:95:3C
    python tools/device_oracle.py --addr XX --cand 1

pkg8 明文候选（--cand）：
    0  epub（pkg3 = 我们的临时公钥 64B）
    1  epub ‖ dev_mesh_pub
    2  dev_mesh_pub ‖ epub
    3  S = ECDH(临时私钥, dev_mesh_pub) 32B
    4  sha256(S)
    5  全 0（测真机是否根本不验 pkg8）
"""
import argparse
import asyncio
import hashlib
import sys
import time
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("需要 bleak：python -m pip install bleak")

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

U = "0000%s-0000-1000-8000-00805f9b34fb"
C10, C16, C1A, C1B, C1C = (U % x for x in ("0010", "0016", "001a", "001b", "001c"))
ROOT = Path(__file__).resolve().parent.parent


def hx(b):
    return " ".join("%02X" % x for x in b)


class Oracle:
    def __init__(self, args):
        self.a = args
        self.t0 = 0.0
        self.cli = None
        self.c16 = None
        self.c10 = None
        self.c1c = None

        if args.p7 == "real":
            self.cert = (ROOT / "_cert/rx3/0_pkg7.bin").read_bytes()
            self.real_cert = True
        else:
            self.cert = (ROOT / "_srv/cloud.der").read_bytes()
            self.real_cert = False
        key = serialization.load_pem_private_key((ROOT / "_srv/cloud.key").read_bytes(), None)
        assert isinstance(key, ec.EllipticCurvePrivateKey)
        self.skey = key
        d = int.from_bytes((ROOT / "_cert/eph_priv.bin").read_bytes(), "big")
        self.ekey = ec.derive_private_key(d, ec.SECP256R1())
        self.epub = (ROOT / "_cert/eph_pub.bin").read_bytes()

        self.dev_cert = b""
        self.dev_pub = b""
        self.dev_info = b""
        self.pkg4 = b""
        self.cur_pkg = None
        self.cur_frames = {}
        self.cur_need = 0
        self.got_pkg3 = asyncio.Event()
        self.got_pkg4 = asyncio.Event()

    # ── 帧 ──
    @staticmethod
    def split_frames(data, size=18):
        return [(i // size + 1, data[i:i + size]) for i in range(0, len(data), size)]

    async def send_pkg(self, pkg, data, tag, gap=0.022):
        frames = self.split_frames(data)
        print("  -> 0016 %7.3fs START pkg=%d 帧数=%d" % (time.monotonic() - self.t0, pkg, len(frames)))
        await self.cli.write_gatt_char(self.c16, bytes([0, 0, 0, pkg, len(frames), 0]), response=False)
        await asyncio.sleep(0.25)
        for seq, payload in frames:
            await self.cli.write_gatt_char(self.c16, bytes([seq, 0x00]) + payload, response=False)
            await asyncio.sleep(gap)
        await asyncio.sleep(0.15)
        print("  -> 0016 %7.3fs transfer-done (pkg=%d %s)" % (time.monotonic() - self.t0, pkg, tag))
        await self.cli.write_gatt_char(self.c16, bytes([0, 0, 1, 0]), response=False)

    # ── pkg8 ──
    @staticmethod
    def _raw(der):
        r, s = utils.decode_dss_signature(der)
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")

    def build_pkg8(self, cand):
        if self.a.p8 == "zero":
            return b"\x00" * 64
        if self.a.p8 == "rand":
            import os
            return os.urandom(64)
        if cand == 5:
            return b"\x00" * 64
        if cand in (0, 1, 2):
            msg = self.epub
            if cand == 1:
                msg = self.epub + self.dev_pub
            elif cand == 2:
                msg = self.dev_pub + self.epub
            return self._raw(self.skey.sign(msg, ec.ECDSA(hashes.SHA256())))
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + self.dev_pub)
        S = self.ekey.exchange(ec.ECDH(), peer)
        print("     [S] " + S.hex())
        msg = S if cand == 3 else hashlib.sha256(S).digest()
        return self._raw(self.skey.sign(msg, ec.ECDSA(hashes.SHA256())))

    # ── 通知 ──
    def _auto_ack(self, payload):
        async def go():
            try:
                await self.cli.write_gatt_char(self.c16, payload, response=False)
            except Exception as e:
                print("     [!] ack 失败 %r" % e)
        asyncio.get_event_loop().create_task(go())

    def on_ntf(self, name, data):
        b = bytes(data)
        u = name
        print("     <- %-4s %7.3fs len=%-3d %s" % (u, time.monotonic() - self.t0, len(b), hx(b)))
        if u != "0016" or len(b) < 3:
            return
        if len(b) == 6 and b[0] == 0 and b[1] == 0 and b[2] == 0:
            self.cur_pkg, self.cur_need, self.cur_frames = b[3], b[4], {}
            print("     ★ 设备 START pkg=%d 帧数=%d -> 回 ag_ack" % (self.cur_pkg, self.cur_need))
            self._auto_ack(bytes([0, 0, 1, 1]))
            return
        if len(b) == 4 and b[0] == 0 and b[1] == 0 and b[2] == 1:
            print("     ★ 设备 ACK%s" % ("（可发帧）" if b[3] == 1 else "（传输完成）"))
            return
        if len(b) >= 3 and b[1] == 0 and self.cur_pkg is not None:
            self.cur_frames[b[0]] = b[2:]
            if len(self.cur_frames) >= self.cur_need:
                joined = b"".join(self.cur_frames[k] for k in sorted(self.cur_frames))
                pkg, self.cur_pkg = self.cur_pkg, None
                self.on_pkg(pkg, joined)

    def on_pkg(self, pkg, data):
        print("     ✔ pkg=%d 收全 %d 字节 -> 回 transfer-done" % (pkg, len(data)))
        self._auto_ack(bytes([0, 0, 1, 0]))
        if pkg == 1:
            self.dev_cert = data
            print("       dev_cert len=%d" % len(data))
        elif pkg == 3 and len(data) >= 84:
            self.dev_info = data[:12]
            self.dev_pub = data[20:84]
            print("       dev_info     = %s" % self.dev_info.hex())
            print("       manu_id      = %s" % data[12:20].hex())
            print("       dev_mesh_pub = %s" % self.dev_pub.hex())
            self.got_pkg3.set()
        elif pkg == 4:
            self.pkg4 = data
            print("       ★★★ 真机 pkg4 = %s" % data.hex())
            self.got_pkg4.set()
        elif pkg == 8:
            print("       (设备回 pkg8? %s)" % data.hex())

    # ── 主流程 ──
    async def run(self):
        """连不上就等：真机随时被重置都能接住。"""
        end = time.monotonic() + self.a.connect_wait
        n = 0
        while True:
            n += 1
            try:
                await self.session()
                return
            except Exception as e:
                if time.monotonic() > end:
                    print("[!] 放弃：%r" % e)
                    return
                print("[retry %d] 连接/订阅失败(%s)，2s 后重试…" % (n, type(e).__name__))
                await asyncio.sleep(2.0)

    async def session(self):
        async with BleakClient(self.a.addr, timeout=20.0) as cli:
            self.cli = cli
            self.t0 = time.monotonic()
            print("[conn] mtu=%s" % getattr(cli, "mtu_size", "?"))
            chars = {c.uuid.lower(): c for s in cli.services for c in s.characteristics}
            names = {C10: "0010", C16: "0016", C1A: "001A", C1B: "001B", C1C: "001C"}
            for u in (C10, C16, C1A, C1B, C1C):
                if u in chars:
                    await cli.start_notify(chars[u], lambda ch, data, u=u: self.on_ntf(names[u], data))
            self.c16, self.c10, self.c1c = chars[C16], chars[C10], chars[C1C]

            async def wr(ch, data, note, gap=0.35):
                tag = ch.uuid[4:8].upper()
                print("  -> %-4s %7.3fs %-22s %s" % (tag, time.monotonic() - self.t0, note, hx(data)))
                await cli.write_gatt_char(ch, data, response=False)
                await asyncio.sleep(gap)

            await wr(self.c1c, b"\x00", "001C probe")
            await wr(self.c1c, b"\x08\x01\x00", "001C alive?")
            await wr(self.c1c, b"\x03", "001C model?")
            await wr(self.c10, b"\xA4", "0010 A4 login", 0.5)
            await wr(self.c16, bytes([0, 0, 5, 0, 6, 0x12]), "0016 mng_ack", 0.5)
            await wr(self.c16, bytes([0, 0, 5, 1]) + b"\x12" * 16, "0016 test-ack", 0.6)
            print("  -> 0010 %7.3fs LOGIN 0x40" % (time.monotonic() - self.t0))
            await cli.write_gatt_char(self.c10, b"\x40\x00\x00\x00", response=False)

            try:
                await asyncio.wait_for(self.got_pkg3.wait(), timeout=15.0)
            except asyncio.TimeoutError:
                print("\n[!] 15s 内没收到 84B 第二段（dev_mesh_pub）——真机可能没进配网窗口")
                return
            if not self.dev_pub:
                print("[!] dev_mesh_pub 为空，中止")
                return

            await asyncio.sleep(0.8)
            cand = self.a.cand
            print("\n[server] pkg7：%d B 自签证书" % len(self.cert))
            await self.send_pkg(7, self.cert, "our-cloud-cert")
            await asyncio.sleep(0.35)
            print("[server] pkg3：我们的临时公钥")
            await self.send_pkg(3, self.epub, "epub")
            await asyncio.sleep(0.35)
            p8 = self.build_pkg8(cand)
            print("[server] pkg8：cand=%d" % cand)
            await self.send_pkg(8, p8, "cand%d" % cand)

            print("\n[wait] 等真机 pkg4 …")
            try:
                await asyncio.wait_for(self.got_pkg4.wait(), timeout=self.a.wait)
            except asyncio.TimeoutError:
                print("[wait] %.0fs 内没等到 pkg4" % self.a.wait)

            out = ROOT / "_cert/oracle_rx"
            out.mkdir(parents=True, exist_ok=True)
            (out / "dev_cert.bin").write_bytes(self.dev_cert)
            (out / "dev_pub.bin").write_bytes(self.dev_pub)
            (out / ("pkg4_cand%d.bin" % cand)).write_bytes(self.pkg4)
            (out / ("pkg8_cand%d.bin" % cand)).write_bytes(p8)
            print("\n[saved] %s" % out)
            try:
                await cli.disconnect()
            except Exception:
                pass


async def list_devices():
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for dev, adv in found.values():
        print("  %-38s %-22s rssi=%s" % (dev.address, (dev.name or "-")[:22], getattr(adv, "rssi", "?")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr")
    ap.add_argument("--cand", type=int, default=0)
    ap.add_argument("--wait", type=float, default=18.0)
    ap.add_argument("--connect-wait", type=float, default=120.0)
    ap.add_argument("--p7", choices=["fake", "real"], default="fake")
    ap.add_argument("--p8", choices=["sign", "zero", "rand"], default="sign")
    a = ap.parse_args()
    if not a.addr:
        asyncio.run(list_devices())
        print("\n用 --addr 指定目标")
        return 0
    asyncio.run(Oracle(a).run())
    return 0


if __name__ == "__main__":
    sys.exit(main())
