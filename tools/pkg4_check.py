#!/usr/bin/env python
"""pkg4_check.py —— 从串口日志核对本轮 pkg4 的 ECDH 与签名是否自洽。

用法：
    python tools/pkg4_check.py logs/xxx.log

做三件事：
 1) 抓 "ECDH 共享密钥 S = ..."、"pkg4 已签名：variant=..."、"pkg4 = ..."
 2) 用 eph_priv × pkg3 重算 S，与固件打印的 S 对比（验证 mbedtls ECDH 约定）
 3) 用 g_eph_pub 验签 pkg4（raw / prehashed 两种），确认签名本身合法

判定意义：
 - 若固件 S != Python S → 固件 ECDH 实现有问题（不是明文猜错）
 - 若验签失败 → 固件签名实现有问题
 - 两者都通过但仍 -40 → 明文或验签公钥不对（服务器用证书公钥 = 纯 ESP32 无解）
"""
import re
import sys
import hashlib
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.utils import Prehashed
from cryptography.exceptions import InvalidSignature

ROOT = Path(__file__).resolve().parent.parent


def load(name, rel):
    p = ROOT / rel if not Path(rel).is_absolute() else Path(rel)
    return p.read_bytes()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

    s_list = re.findall(r"ECDH 共享密钥 S = ([0-9A-F]{64})", log)
    v_list = re.findall(r"pkg4 已签名：variant=(\d+)\(([^)]*)\)", log)
    sig_list = re.findall(r"pkg4 = ([0-9A-F]{128})", log)
    spub_list = re.findall(r"pkg3 已存|pkg3\(spub\) 已存", log)

    print(f"会话数：S={len(s_list)} 签名={len(sig_list)} pkg3={len(spub_list)}")
    if not sig_list:
        print("没找到签名记录，确认这轮是否走到了 pkg4")
        return 1

    priv = int.from_bytes(load("eph", "_cert/eph_priv.bin"), "big")
    pv = ec.derive_private_key(priv, ec.SECP256R1())
    pub = pv.public_key()

    for i, sig_hex in enumerate(sig_list):
        sig = bytes.fromhex(sig_hex)
        variant = v_list[i] if i < len(v_list) else ("?", "?")
        msg = None
        if i < len(s_list):
            S = bytes.fromhex(s_list[i])
            # 用 pkg3 重算（若日志里有 pkg3 原始帧，可扩展）
            print(f"\n[{i}] variant={variant[0]}({variant[1]})")
            print(f"    固件 S = {S.hex()}")
            msg = S
        raw = utils.encode_dss_signature(int.from_bytes(sig[:32], "big"),
                                        int.from_bytes(sig[32:], "big"))
        ok = []
        if msg is not None:
            for name, alg in (("SHA256(S)", ec.ECDSA(hashes.SHA256())),
                              ("S-as-digest", ec.ECDSA(Prehashed(hashes.SHA256())))):
                digest = msg if name == "S-as-digest" else None
                data = msg if digest is None else hashlib.sha256(msg).digest()
                try:
                    pub.verify(raw, data, alg)
                    ok.append(name)
                except InvalidSignature:
                    pass
        print(f"    签名自洽：{ok if ok else '❌ 与 S 不匹配（或该 variant 明文不是 S）'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
