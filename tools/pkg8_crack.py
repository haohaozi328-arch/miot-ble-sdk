#!/usr/bin/env python3
"""离线破解：服务器 pkg8（64B）到底签的是什么明文？

材料（真机/假设备会话里 App 发来的三个包）：
  pkg7 = server_cert (379B X.509)
  pkg3 = server_pub  (64B 未压缩 P-256 点)
  pkg8 = sign        (64B  r||s，ECDSA-SHA256)

思路：pkg8 是**服务器**用 server_cert 私钥签的。只要枚举出「服务器签的明文」，
就能知道设备侧 pkg4 该签什么（通常两侧签同一段 transcript）。

用法: python tools/pkg8_crack.py
"""
import itertools, hashlib, sys
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes, serialization
from cryptography import x509
from cryptography.exceptions import InvalidSignature

R = "D:/workspace/temp/miot-ble-sdk/"


def rd(p):
    return open(R + p, "rb").read()


def load_cert_pub(der):
    c = x509.load_der_x509_certificate(der)
    print(f"  subject : {c.subject.rfc4514_string()}")
    print(f"  issuer  : {c.issuer.rfc4514_string()}")
    print(f"  serial  : {hex(c.serial_number)}  notAfter={c.not_valid_after_utc}")
    return c.public_key()


def raw_to_der(sig64):
    r = int.from_bytes(sig64[:32], "big")
    s = int.from_bytes(sig64[32:], "big")
    return utils.encode_dss_signature(r, s)


def try_verify(pub, sig64, msg):
    for name, sig in (("raw64", raw_to_der(sig64)),):
        try:
            pub.verify(sig, msg, ec.ECDSA(hashes.SHA256()))
            return name
        except InvalidSignature:
            pass
    return None


def main():
    pkg7 = rd("_cert/rx/r19_pkg7.bin")
    pkg3 = rd("_cert/rx/r19_pkg3.bin")
    pkg8 = rd("_cert/rx/r19_pkg8.bin")
    print(f"pkg7={len(pkg7)}B pkg3={len(pkg3)}B pkg8={len(pkg8)}B")

    print("[server cert]")
    try:
        pub = load_cert_pub(pkg7)
    except Exception as e:
        print("  parse fail:", e)
        return

    # 可能的「设备侧」材料
    blob = rd("_cert/blob84.bin")          # 真机 84B
    real_cert = rd("logs/real_cert.der")   # 真机设备证书
    eph = rd("_cert/eph_pub.bin")          # 我们自己的公钥
    dev_pub_real = blob[20:84]
    dev_info = blob[:12]
    manu = blob[12:20]

    # pkg8 去前缀（有些抓包里带 2B 头）
    sig_candidates = {"pkg8[0:64]": pkg8[:64], "pkg8[2:66]": pkg8[2:] if len(pkg8) > 64 else b""}

    # 明文候选（都用 device 视角的 84B pub，先试真机）
    msgs = {}
    for tag, dpub in (("realpub", dev_pub_real), ("ephpub", eph)):
        msgs[f"{tag}"] = dpub
        msgs[f"sha256({tag})"] = hashlib.sha256(dpub).digest()
        msgs[f"{tag}+serverpub"] = dpub + pkg3
        msgs[f"serverpub+{tag}"] = pkg3 + dpub
        msgs[f"serverpub"] = pkg3
        msgs[f"{tag}+serverpub+{tag}"] = dpub + pkg3 + dpub
        msgs[f"sha256({tag}+serverpub)"] = hashlib.sha256(dpub + pkg3).digest()
        msgs[f"sha256(serverpub+{tag})"] = hashlib.sha256(pkg3 + dpub).digest()
        msgs[f"dev_info+manu+{tag}"] = dev_info + manu + dpub
        msgs[f"{tag}+dev_info+manu"] = dpub + dev_info + manu
    msgs["blob84"] = blob
    msgs["sha256(blob84)"] = hashlib.sha256(blob).digest()
    msgs["dev_cert"] = real_cert
    msgs["sha256(dev_cert)"] = hashlib.sha256(real_cert).digest()
    msgs["dev_cert+blob84"] = real_cert + blob
    msgs["blob84+dev_cert"] = blob + real_cert
    msgs["cert_pub"] = real_cert[real_cert.find(b"\x03\x42\x00\x04") + 4:][:64]

    hit = None
    for sk, sig in sig_candidates.items():
        if len(sig) != 64:
            continue
        for mk, m in msgs.items():
            if try_verify(pub, sig, m):
                print(f"\n★★ HIT: {sk}  验签明文 = {mk}  ({len(m)}B)")
                print("   hex:", m.hex())
                hit = (sk, mk, m)
                break
        if hit:
            break
    if not hit:
        print("\n未命中：pkg8 的明文包含我们没抓到的随机量（或不是纯 ECDSA-SHA256）")
        print("已试签名切片:", list(sig_candidates), " 明文候选:", len(msgs))


if __name__ == "__main__":
    main()
