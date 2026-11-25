#!/usr/bin/env python3
"""扩大搜索：pkg8(64B) 被 Mijia Cloud 证书私钥签的明文到底是什么。

除了 84B 里那把 dev_mesh_pub，还试：
  * ASCII/hex/base64 形式的 pub、cert、dev_info
  * SHA-1/256/384/512 预哈希
  * 真机 pub ↔ 我们的 pub 都试
  * pkg8 的两种切片（原样 / 去 2B 头）
"""
import base64, hashlib, itertools, json
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
eph = rd("_cert/eph_pub.bin")

cert = x509.load_der_x509_certificate(pkg7)
pub = cert.public_key()

dev_pub = blob[20:84]
dev_info = blob[:12]
manu = blob[12:20]
# 真机证书里的那把公钥
i = real_cert.find(b"\x03\x42\x00\x04")
cert_pub = real_cert[i + 4:i + 68]

print("server cert:", cert.subject.rfc4514_string(), "| sig alg:", cert.signature_algorithm_oid._name)


def forms(tag, b):
    d = {}
    d[tag] = b
    d[f"hex({tag})"] = b.hex().encode()
    d[f"HEX({tag})"] = b.hex().upper().encode()
    d[f"b64({tag})"] = base64.b64encode(b)
    d[f"sha1({tag})"] = hashlib.sha1(b).digest()
    d[f"sha256({tag})"] = hashlib.sha256(b).digest()
    d[f"sha384({tag})"] = hashlib.sha384(b).digest()
    d[f"sha512({tag})"] = hashlib.sha512(b).digest()
    return d


base = {}
for tag, b in (("devpub", dev_pub), ("ephpub", eph), ("certpub", cert_pub),
               ("serverpub", pkg3), ("blob84", blob), ("devcert", real_cert),
               ("devinfo", dev_info), ("manu", manu)):
    base.update(forms(tag, b))

# 两两拼接（含哈希）
combos = {}
keys = ["devpub", "ephpub", "serverpub", "devinfo", "manu", "certpub"]
for a, b in itertools.permutations(keys, 2):
    m = base[a] + base[b]
    combos[f"{a}+{b}"] = m
    combos[f"sha256({a}+{b})"] = hashlib.sha256(m).digest()

msgs = {**base, **combos}
# JSON 风格
msgs["json(devpub_b64)"] = json.dumps(base64.b64encode(dev_pub).decode()).encode()
msgs["str(devpub_b64)"] = base64.b64encode(dev_pub)

print(f"明文候选 {len(msgs)} 个")

sigs = [("pkg8[:64]", pkg8[:64])]
if len(pkg8) == 66:
    sigs.append(("pkg8[2:]", pkg8[2:]))

hits = []
for sk, s in sigs:
    der = utils.encode_dss_signature(int.from_bytes(s[:32], "big"), int.from_bytes(s[32:], "big"))
    for mk, m in msgs.items():
        for hname, h in (("sha256", hashes.SHA256()), ("sha1", hashes.SHA1()),
                         ("sha384", hashes.SHA384()), ("sha512", hashes.SHA512())):
            try:
                pub.verify(der, m, ec.ECDSA(h))
                hits.append((sk, mk, hname, len(m)))
            except InvalidSignature:
                pass
            # 预哈希模式（m 本身是摘要）
            if h.name == "sha256" and len(m) == 32:
                try:
                    pub.verify(der, m, ec.ECDSA(Prehashed(hashes.SHA256())))
                    hits.append((sk, mk, "prehashed-sha256", len(m)))
                except InvalidSignature:
                    pass

print("hits:", hits if hits else "无")
