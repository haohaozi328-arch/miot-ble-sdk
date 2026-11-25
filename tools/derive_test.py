#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证「设备私钥是否由 MAC / PIN / dev_info 等公开信息派生」。

思路：如果私钥是确定性派生的，那么用公开信息 + 已知派生算法就能算出来，
      算出来的公钥必然等于证书里那把。所以穷举所有合理组合，比对公钥即可。
      全部不中 → 私钥是产线随机生成，只能从芯片里 dump，无法推导。

用法：python tools/derive_test.py [--pin 123456] [--sn XXX]
"""
import argparse
import hashlib
import hmac
import itertools

from cryptography.hazmat.primitives.asymmetric import ec

CURVE = ec.SECP256R1()
ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

# 真机证书里的公钥（OooO0o 那台 14:D8:81:E6:95:3C）
TX = 0xd2e9be91dc3a3ff5cc98bef8f8db3a38b89396c7f0f550b88ed7f1f13ed32ed1
TY = 0xb4078b2a75ebe869b8eb05e684e4f6b2ae4a67bbd98968ac3904d982a06c882f

MAC_BE = bytes.fromhex("14D881E6953C")
MAC_LE = MAC_BE[::-1]
# pkg3 前 12 字节 dev_info（实测全 0，仅末两位 01 02）
DEV_INFO = bytes.fromhex("000000000000000000000102")
MANU_CERT_ID = bytes.fromhex("1200000000000000")
# 84B blob 里的 64B 段
BLOB_TAIL = bytes.fromhex(
    "3DC03B82627C0B31788943BFBF80369A11013DE8673C504FCC39A6EB26E3E547"
    "F020627919343B9EC79A7B391F96A78E5D0AA4CA060C734F03170F0BC36763E6"
)
PUB_XY = TX.to_bytes(32, "big") + TY.to_bytes(32, "big")

CONSTS = [b"", b"mijia", b"miio", b"xiaomi", b"miot", b"mible", b"mesh",
          b"Mijia", b"MIJIA", b"mhc", b"mhcb", b"sw4b01", b"xry"]


def pub_of(k):
    k %= ORDER
    if k == 0:
        return None
    try:
        n = ec.derive_private_key(k, CURVE).public_key().public_numbers()
    except Exception:
        return None
    return (n.x, n.y)


def matches(k):
    p = pub_of(k)
    return p == (TX, TY)


def kid(k):
    return int.from_bytes(k, "big")


def candidates(seeds):
    """yield (描述, 32字节候选私钥)"""
    for name, s in seeds:
        # 直接用种子当标量
        if len(s) >= 32:
            yield f"{name}[:32] 直取", s[:32]
            yield f"{name}[-32:] 直取", s[-32:]
        if 0 < kid(s) < ORDER:
            yield f"{name} 整体当标量", s.rjust(32, b"\x00")
        # 哈希派生
        yield f"sha256({name})", hashlib.sha256(s).digest()
        yield f"sha256({name}|{name})", hashlib.sha256(s + s).digest()
        yield f"sha512({name})[:32]", hashlib.sha512(s).digest()[:32]
        yield f"sha256(00|{name})", hashlib.sha256(b"\x00" + s).digest()
        yield f"sha256(01|{name})", hashlib.sha256(b"\x01" + s).digest()
        yield f"sha256({name}|00)", hashlib.sha256(s + b"\x00").digest()
        yield f"double-sha256({name})", hashlib.sha256(hashlib.sha256(s).digest()).digest()
        for c in CONSTS:
            if not c:
                continue
            yield f"sha256({name}|{c.decode()})", hashlib.sha256(s + c).digest()
            yield f"sha256({c.decode()}|{name})", hashlib.sha256(c + s).digest()
            try:
                yield f"hmac({c.decode()},{name})", hmac.new(c, s, hashlib.sha256).digest()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin", default=None, help="设备上的 PIN / 配网码（如果外壳上印了）")
    ap.add_argument("--sn", default=None, help="设备 SN（如果外壳贴纸上有）")
    a = ap.parse_args()

    seeds = [
        ("MAC_BE", MAC_BE),
        ("MAC_LE", MAC_LE),
        ("MAC_BE_ascii", MAC_BE.hex().upper().encode()),
        ("DEV_INFO", DEV_INFO),
        ("MANU_CERT_ID", MANU_CERT_ID),
        ("BLOB_TAIL", BLOB_TAIL),
        ("PUB_XY", PUB_XY),
    ]
    if a.pin:
        seeds.append(("PIN_ascii", a.pin.encode()))
        seeds.append(("PIN_hex", bytes.fromhex(a.pin)))
        for nm, s in list(seeds):
            if nm.startswith("MAC"):
                seeds.append((f"{nm}+PIN", s + a.pin.encode()))
                seeds.append((f"PIN+{nm}", a.pin.encode() + s))
    if a.sn:
        seeds.append(("SN_ascii", a.sn.encode()))

    # 两两组合（按字节拼接，两种顺序）
    base = list(seeds)
    for (n1, s1), (n2, s2) in itertools.combinations(base, 2):
        if len(s1) + len(s2) > 96:
            continue
        seeds.append((f"{n1}+{n2}", s1 + s2))

    tried = 0
    hits = []
    seen = set()
    for name, s in seeds:
        if len(s) == 0:
            continue
        for desc, k in candidates([(name, s)]):
            if k in seen:
                continue
            seen.add(k)
            tried += 1
            if matches(kid(k)):
                hits.append((desc, k))
                print(f"\n*** 命中 *** {desc}\n  私钥 {k.hex()}")

    print(f"\n种子 {len(seeds)} 个，候选私钥 {tried} 个，命中 {len(hits)} 个")
    if not hits:
        print("结论：私钥不是由 MAC/PIN/dev_info 派生 —— 产线随机生成，只能 dump")
        print("\n提示：如果设备外壳上印了 PIN 或 SN，用 --pin / --sn 再跑一遍")
    return 0 if hits else 1


if __name__ == "__main__":
    raise SystemExit(main())
