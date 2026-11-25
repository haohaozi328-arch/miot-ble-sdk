#!/usr/bin/env python3
"""pkg8 明文暴力搜索（全排列版）

设备要能验证 pkg8，说明它的明文必须完全由设备手里有的材料算出来。
把已知材料当积木，穷举 1~4 块的拼接（含各顺序）以及 SHA-256 预哈希，
用 Mijia Cloud 证书公钥验签。
"""
import hashlib, itertools
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed
from cryptography import x509
from cryptography.exceptions import InvalidSignature

R = "D:/workspace/temp/miot-ble-sdk/"


def rd(p):
    return open(R + p, "rb").read()


pkg7 = rd("_cert/rx/r19_pkg7.bin")
pkg3 = rd("_cert/rx/r19_pkg3.bin")
pkg8 = rd("_cert/rx/r19_pkg8.bin")
blob = rd("_cert/blob84.bin")
real_cert = rd("logs/real_cert.der")

pub = x509.load_der_x509_certificate(pkg7).public_key()

i = real_cert.find(b"\x03\x42\x00\x04")
eph = rd("_cert/eph_pub.bin")
blocks = {
    "SP": pkg3,                 # server pub 64
    "DP": blob[20:84],          # dev mesh pub 64（真机）
    "EP": eph,                  # 我们自己的公钥 64
    "CP": real_cert[i + 4:i + 68],  # 证书里的设备公钥 64
    "BL": blob,                 # 84B
    "DI": blob[:12],
    "MC": blob[12:20],
    "DC": real_cert,            # 设备证书 451
    "SC": pkg7,                 # 服务器证书 379
}
sigs = {"pkg8[:64]": utils.encode_dss_signature(int.from_bytes(pkg8[:32], "big"), int.from_bytes(pkg8[32:], "big"))}
if len(pkg8) == 66:
    s6 = pkg8[2:]
    sigs["pkg8[2:66]"] = utils.encode_dss_signature(int.from_bytes(s6[:32], "big"), int.from_bytes(s6[32:], "big"))

names = list(blocks)
hit = None
cnt = 0
for r in range(1, 5):
    for combo in itertools.permutations(names, r):
        m = b"".join(blocks[c] for c in combo)
        cands = [("raw", m), ("sha256", hashlib.sha256(m).digest())]
        for tag, mm in cands:
            for sk, sig in sigs.items():
                cnt += 1
                try:
                    if tag == "sha256":
                        pub.verify(sig, mm, ec.ECDSA(Prehashed(hashes.SHA256())))
                    else:
                        pub.verify(sig, mm, ec.ECDSA(hashes.SHA256()))
                    hit = ("+".join(combo), tag, sk, len(m))
                    print("★★ HIT", hit)
                    raise SystemExit
                except InvalidSignature:
                    pass
print(f"试了 {cnt} 次，未命中")


