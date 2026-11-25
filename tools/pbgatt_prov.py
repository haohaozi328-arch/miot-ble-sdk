#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PB-GATT 配网探测 / 配置器（PC 端，bleak）

背景：MHCB12G（RTL8762EGF）连上后 7~8 秒就被固件主动断开 —— PB-GATT 节点要求
客户端在连接建立后立刻开始发 Provisioning PDU，否则释放连接。nRF Connect 手工
点读按钮追不上这个超时，所以只能脚本化：连上即发包。

三个阶段：
  1) probe —— 连上后按 3 种 GATT 层封装各发一次 Invite，看设备回不回
              Capabilities PDU。回来 = 标准 PB-GATT 且封装确认；全都不回 =
              这个 0x2B29/0x2B2A 是厂商私有通道，不是标准 provisioning。
  2) prov  —— 收到 Capabilities 后跑完整流程（ECDH P-256 → Confirm → Random → Data）
  3) 报告  —— 全过程 hex 打印，便于逐字节对照

用法：
  python tools/pbgatt_prov.py --addr 14:D8:81:E6:95:3C
  python tools/pbgatt_prov.py --addr 14:D8:81:E6:95:3C --full \
      --net-key 00112233445566778899aabbccddeeff --unicast 0x0001

依赖：bleak, cryptography
"""
import argparse
import asyncio
import os
import sys
import time

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("需要 bleak：python -m pip install bleak")

# ---------------------------------------------------------------- UUID 候选
# 标准 Mesh 1.0.1 = 2ADB/2ADC；Mesh 1.1 与部分 Realtek 固件 = 2B29/2B2A
IN_UUIDS = ["00002b29-0000-1000-8000-00805f9b34fb",
            "00002adb-0000-1000-8000-00805f9b34fb"]
OUT_UUIDS = ["00002b2a-0000-1000-8000-00805f9b34fb",
             "00002adc-0000-1000-8000-00805f9b34fb"]

# ---------------------------------------------------------------- Provisioning PDU 类型
T_INVITE = 0x00
T_CAPS = 0x01
T_START = 0x02
T_PUBKEY = 0x03
T_INPUT_OOB = 0x04
T_OUTPUT_OOB = 0x05
T_CONFIRM = 0x05       # 注意：Provisioning Confirmation 也是 0x05，按上下文区分
T_RANDOM = 0x06
T_DATA = 0x07
T_COMPLETE = 0x08
T_FAILED = 0x09

PDU_NAME = {
    0x00: "Invite", 0x01: "Capabilities", 0x02: "Start", 0x03: "PublicKey",
    0x04: "InputOO B", 0x05: "Confirm/OutputOOB", 0x06: "Random",
    0x07: "Data", 0x08: "Complete", 0x09: "Failed",
}


# ---------------------------------------------------------------- AES-CMAC / k1 / s1
def _ecb(key: bytes, blk: bytes) -> bytes:
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return e.update(blk) + e.finalize()


def _dbl(blk: bytes) -> bytes:
    i = int.from_bytes(blk, "big") << 1
    if i >> 128:
        i = (i & ((1 << 128) - 1)) ^ 0x87
    return i.to_bytes(16, "big")


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    k1 = _dbl(_ecb(key, bytes(16)))
    k2 = _dbl(k1)
    if not msg:
        blocks = [_xor(bytes(16), k2)]
    else:
        n = (len(msg) + 15) // 16
        blocks = [msg[i * 16:(i + 1) * 16] for i in range(n)]
        if len(blocks[-1]) == 16:
            blocks[-1] = _xor(blocks[-1], k1)
        else:
            tail = blocks[-1] + b"\x80" + bytes(15 - len(blocks[-1]))
            blocks[-1] = _xor(tail, k2)
    x = bytes(16)
    for b in blocks:
        x = _ecb(key, _xor(x, b))
    return x


def s1(msg: bytes) -> bytes:
    return aes_cmac(bytes(16), msg)


def k1(n: bytes, salt: bytes, p: bytes) -> bytes:
    return aes_cmac(aes_cmac(salt, n), p)


# ---------------------------------------------------------------- 封装（GATT bearer）
def wrap(pdu: bytes, framing: str) -> bytes:
    """GATT bearer 的封装之争：三种候选，probe 阶段自动判定。"""
    if framing == "len":
        return bytes([len(pdu)]) + pdu
    if framing == "len+1":
        return bytes([len(pdu) + 1]) + pdu
    return pdu            # raw


def unwrap(data: bytes, framing: str) -> bytes:
    if framing == "raw":
        return data
    if len(data) >= 1 and data[0] in (len(data) - 1, len(data) - 2):
        return data[1:]
    return data[1:] if len(data) > 1 else data


def hexs(b: bytes) -> str:
    return b.hex(" ", 1) if b else "(空)"


# ---------------------------------------------------------------- 主流程
class ProvSession:
    def __init__(self, client: BleakClient, in_uuid: str, out_uuid: str, args):
        self.c = client
        self.in_uuid = in_uuid
        self.out_uuid = out_uuid
        self.args = args
        self.q: asyncio.Queue = asyncio.Queue()
        self.framing = None
        self.t0 = time.time()
        self.caps_raw = None
        self.dev_pub = None
        self.pdus = {}          # 供 ConfirmationInputs 使用（不带封装）
        self.priv = ec.generate_private_key(ec.SECP256R1())

    # --- 底层收
    def _on_notify(self, _h, data: bytearray):
        d = bytes(data)
        el = time.time() - self.t0
        print("   << [%5.2fs] %s" % (el, hexs(d)))
        self.q.put_nowait(d)

    async def setup(self):
        for u in OUT_UUIDS:
            try:
                await self.c.start_notify(u, self._on_notify)
                self.out_uuid = u
                print("   订阅 Out 特征值: %s" % u[4:8])
                return
            except Exception as e:
                last = e
        raise RuntimeError("订阅 Out 特征值失败: %s" % last)

    async def send(self, pdu: bytes, framing=None, note=""):
        fr = framing or self.framing or "len"
        data = wrap(pdu, fr)
        print("   >> [%5.2fs] %-12s %s" % (time.time() - self.t0, note or pdu[0:1].hex(), hexs(data)))
        await self.c.write_gatt_char(self.in_uuid, data, response=True)

    async def recv(self, timeout=3.0):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    # --- 阶段 1：判断封装
    async def probe(self):
        print("\n== 阶段 1：判定 GATT 层封装 ==")
        invite = bytes([T_INVITE, 0x00])
        for fr in ("len", "len+1", "raw"):
            while not self.q.empty():
                self.q.get_nowait()
            print("\n  [候选 %s] Invite = %s" % (fr, hexs(wrap(invite, fr))))
            try:
                await self.send(invite, fr, "Invite")
            except Exception as e:
                print("    发送失败: %s" % e)
                return None
            r = await self.recv(2.0)
            if r is None:
                print("    → 无应答")
                continue
            pdu = unwrap(r, fr)
            if pdu and pdu[0] == T_CAPS:
                self.framing = fr
                self.caps_raw = pdu
                self.pdus["invite"] = invite
                self.pdus["caps"] = pdu
                print("\n  ★ 封装确认 = %s，收到 Capabilities" % fr)
                self._show_caps(pdu)
                return True
            print("    → 收到非 Capabilities PDU 0x%02X" % pdu[0])
        print("\n  ✗ 三种封装均无应答 —— 该通道不是标准 PB-GATT 配网口")
        return False

    def _show_caps(self, pdu: bytes):
        p = pdu[1:]
        if len(p) < 11:
            print("    Capabilities 长度异常: %s" % hexs(pdu))
            return
        el, alg, pkt, soob, oos, ooa, ios, ioa = (
            p[0], int.from_bytes(p[1:3], "big"), p[3], p[4], p[5],
            int.from_bytes(p[6:8], "big"), p[8], int.from_bytes(p[9:11], "big"))
        print("    Elements=%d  Algorithms=0x%04X(P-256=%s)  PubKeyType=%d"
              % (el, alg, "支持" if alg & 1 else "不支持", pkt))
        print("    StaticOOB=0x%02X  OutputOOB(size=%d actions=0x%04X)  InputOOB(size=%d actions=0x%04X)"
              % (soob, oos, ooa, ios, ioa))

    # --- 阶段 2：完整配网
    async def run_prov(self):
        a = self.args
        print("\n== 阶段 2：标准配网 ==")
        # Start：FIPS P-256 / 无 OOB 公钥 / No OOB 认证
        start = bytes([T_START, 0x00, 0x00, 0x00, 0x00, 0x00])
        await self.send(start, note="Start")
        self.pdus["start"] = start

        r = await self.recv(3.0)
        if r is None:
            print("  ✗ 等不到设备公钥")
            return False
        pdu = unwrap(r, self.framing)
        if pdu[0] == T_FAILED:
            print("  ✗ 设备返回 Provisioning Failed, reason=0x%02X" % pdu[1])
            return False
        if pdu[0] != T_PUBKEY or len(pdu) < 65:
            print("  ✗ 预期 PublicKey，收到 0x%02X len=%d" % (pdu[0], len(pdu)))
            return False
        self.pdus["dev_pub"] = pdu
        self.dev_pub = pdu[1:65]

        # 我方公钥
        nums = self.priv.public_key().public_numbers()
        my_pub = nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")
        prov_pub_pdu = bytes([T_PUBKEY]) + my_pub
        await self.send(prov_pub_pdu, note="ProvPublicKey")
        self.pdus["prov_pub"] = prov_pub_pdu

        # ECDH
        peer = ec.EllipticCurvePublicNumbers(
            int.from_bytes(self.dev_pub[:32], "big"),
            int.from_bytes(self.dev_pub[32:], "big"),
            ec.SECP256R1()).public_key()
        secret = self.priv.exchange(ec.ECDH(), peer)

        conf_inputs = (self.pdus["invite"] + self.pdus["caps"] + self.pdus["start"]
                       + self.pdus["prov_pub"] + self.pdus["dev_pub"])
        salt = s1(conf_inputs)
        conf_key = k1(secret, salt, b"prck")
        sess_key = k1(secret, salt, b"prsk")
        sess_nonce = k1(secret, salt, b"prsn")

        prov_rand = os.urandom(16)
        auth_value = bytes(16)                      # No OOB
        prov_confirm = aes_cmac(conf_key, prov_rand + auth_value)
        await self.send(bytes([T_CONFIRM]) + prov_confirm, note="Confirm")

        r = await self.recv(3.0)
        if r is None:
            print("  ✗ 等不到设备 Confirmation")
            return False
        pdu = unwrap(r, self.framing)
        if pdu[0] == T_FAILED:
            print("  ✗ Failed reason=0x%02X" % pdu[1])
            return False
        dev_confirm = pdu[1:17]
        print("    设备 Confirmation = %s" % dev_confirm.hex())

        await self.send(bytes([T_RANDOM]) + prov_rand, note="Random")

        r = await self.recv(3.0)
        if r is None:
            print("  ✗ 等不到设备 Random（Confirmation 校验失败的典型表现）")
            return False
        pdu = unwrap(r, self.framing)
        if pdu[0] == T_FAILED:
            print("  ✗ Failed reason=0x%02X（0x04=Confirmation 校验失败）" % pdu[1])
            return False
        dev_rand = pdu[1:17]
        if aes_cmac(conf_key, dev_rand + auth_value) != dev_confirm:
            print("  ✗ 设备 Confirmation 校验不通过 —— 我方公钥/随机数不同步")
            return False
        print("    ✓ 设备 Confirmation 校验通过")

        # Provisioning Data：netkey(16) + keyindex(2) + flags(1) + ivindex(4) + unicast(2)
        plain = (a.net_key + a.key_index.to_bytes(2, "big") + bytes([0x00])
                 + a.iv_index.to_bytes(4, "big") + a.unicast.to_bytes(2, "big"))
        aad = b"\x07" if a.aad == "07" else b""
        enc = AESCCM(sess_key, tag_length=8).encrypt(sess_nonce, plain, aad)
        await self.send(bytes([T_DATA]) + enc, note="Data")

        r = await self.recv(4.0)
        if r is None:
            print("  ✗ 等不到 Provisioning Complete")
            return False
        pdu = unwrap(r, self.framing)
        if pdu[0] == T_COMPLETE:
            print("\n  ★★★ 配网成功：设备已入网 netkey_idx=%d unicast=0x%04X"
                  % (a.key_index, a.unicast))
            return True
        print("  ✗ 收到 0x%02X：%s" % (pdu[0], hexs(pdu)))
        return False


async def find(addr_hint, name_hint, window=20.0):
    """持续监听 + 多轮重试。这块模组广播间隔很长，单轮 discover() 经常漏掉。"""
    for rnd in range(1, 4):
        print("扫描中（第 %d 轮，%ds 持续监听）…" % (rnd, int(window)))
        hits = {}
        miss = {}

        def cb(d, adv):
            a = d.address.upper().replace("-", ":")
            nm = (d.name or adv.local_name or "")
            if (addr_hint and addr_hint.upper() in a) or (name_hint and name_hint.lower() in nm.lower()):
                hits[a] = (adv.rssi, nm, adv, d)
            else:
                miss[a] = 1

        async with BleakScanner(detection_callback=cb) as sc:
            await asyncio.sleep(window)
        if hits:
            break
        print("  第 %d 轮没扫到（可见 %d 台）" % (rnd, len(miss)))
    else:
        print("  三轮都没扫到。给模组断电重启（拔 VCC 3 秒）再试。")
        return None
    for addr, (rssi, nm, adv, _d) in hits.items():
        print("  HIT %s  rssi=%d  name=%s" % (addr, rssi, nm))
        print("      uuids   : %s" % [str(u)[4:8] for u in adv.service_uuids])
        for k, v in adv.service_data.items():
            print("      svcdata : %s = %s" % (str(k)[4:8], v.hex()))
        for k, v in adv.manufacturer_data.items():
            print("      mfgdata : 0x%04X = %s" % (k, v.hex()))
        if not adv.service_data and not adv.manufacturer_data:
            print("      (无 svc/mfg data —— 只有 flags+name，说明它已不在 MiBeacon 配网态)")
    dev = [d for a, (r, n, adv, d) in hits.items()][0]
    return dev


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="14:D8:81:E6:95:3C")
    ap.add_argument("--name", default="xry")
    ap.add_argument("--full", action="store_true", help="探测成功后继续完整配网")
    ap.add_argument("--net-key", default="00112233445566778899aabbccddeeff")
    ap.add_argument("--key-index", type=int, default=0)
    ap.add_argument("--iv-index", type=int, default=0)
    ap.add_argument("--unicast", type=lambda s: int(s, 0), default=0x0001)
    ap.add_argument("--aad", choices=["07", "none"], default="07")
    a = ap.parse_args()
    a.net_key = bytes.fromhex(a.net_key.replace(" ", ""))
    assert len(a.net_key) == 16, "net-key 必须 16 字节"

    dev = await find(a.addr, a.name)
    if not dev:
        return 2

    disc = asyncio.Event()
    t_conn = [time.time()]

    def on_dc(_c):
        print("  !! 连接断开（%.2fs）" % (time.time() - t_conn[0]))
        disc.set()

    # Windows 上必须用扫描得到的 BLEDevice 对象连接（用地址字符串会 DeviceNotFound），
    # 而且这块模组广播很稀，连不上就重扫再来。
    c = None
    for attempt in range(1, 4):
        try:
            c = BleakClient(dev, timeout=20, disconnected_callback=on_dc)
            t_conn[0] = time.time()
            await c.connect()
            print("已连接（第 %d 次尝试），MTU=%s" % (attempt, getattr(c, "mtu_size", "?")))
            break
        except Exception as e:
            print("  连接失败(%d/%d): %s %s" % (attempt, 3, type(e).__name__, e))
            c = None
            if attempt < 3:
                dev = await find(a.addr, a.name, window=12.0)
                if not dev:
                    return 2
    if c is None:
        print("三次都没连上。给模组断电重启一次再跑。")
        return 2

    try:
        chrs = {ch.uuid.lower(): ch for s in c.services for ch in s.characteristics}
        in_uuid = next((u for u in IN_UUIDS if u in chrs), None)
        out_uuid = next((u for u in OUT_UUIDS if u in chrs), None)
        if not in_uuid:
            print("✗ 这个固件没有 provisioning 数据通道（2B29/2ADB 都没有）")
            return 3
        print("  In=%s props=%s" % (in_uuid[4:8], chrs[in_uuid].properties))
        if out_uuid:
            print("  Out=%s props=%s" % (out_uuid[4:8], chrs[out_uuid].properties))
        else:
            print("✗ 没有 Data Out 特征值，无法收设备应答")
            return 4

        s = ProvSession(c, in_uuid, out_uuid, a)
        try:
            await s.setup()
        except Exception as e:
            print("订阅 Out 失败: %s" % e)
            print("\n结论：Data Out 不可 notify。标准 PB-GATT 必须靠 notify 回包，")
            print("      所以这对特征值不是标准配网口。")
            return 4

        ok = await s.probe()
        if not ok:
            print("\n结论：连上了、通道在、但标准 Invite 无应答。"
                  "\n      说明这对 2B29/2B2A 不是标准 SIG PB-GATT 配网口（很可能是米家私有通道）。")
            return 5

        if a.full:
            good = await s.run_prov()
            return 0 if good else 6
        print("\n（只探测。加 --full 继续完整配网）")
        return 0
    finally:
        try:
            await c.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
