#!/usr/bin/env python3
"""在一片固件/内存 dump 里找 P-256 私钥。

原理：设备私钥 d 满足 d*G = 已知公钥 Q。对 dump 的每个偏移滑一个 32 字节窗口，
当候选值看成标量算 Q' ，与目标 Q 比对。

用法：
  python tools/find_privkey.py dump.bin
  python tools/find_privkey.py dump.bin --align 4 --start 0 --end 0x40000
  python tools/find_privkey.py --selftest

默认目标公钥 = 真机证书（14:D8:81:E6:95:3C）里那把：
  X d2e9be91dc3a3ff5cc98bef8f8db3a38b89396c7f0f550b88ed7f1f13ed32ed1
  Y b4078b2a75ebe869b8eb05e684e4f6b2ae4a67bbd98968ac3904d982a06c882f
"""
import argparse
import os
import sys
import time

from cryptography.hazmat.primitives.asymmetric import ec

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

TARGET_X = 0xD2E9BE91DC3A3FF5CC98BEF8F8DB3A38B89396C7F0F550B88ED7F1F13ED32ED1
TARGET_Y = 0xB4078B2A75EBE869B8EB05E684E4F6B2AE4A67BBD98968AC3904D982A06C882F


def on_curve(x, y):
    return (y * y - (x * x * x + A * x + B)) % P == 0


def scan(data, target=(TARGET_X, TARGET_Y), align=1, start=0, end=None,
         progress=True):
    """返回所有命中的偏移。"""
    tx, ty = target
    curve = ec.SECP256R1()
    end = len(data) if end is None else min(end, len(data))
    hits = []
    t0 = time.time()
    total = max(1, (end - start - 31 + align - 1) // align)
    done = 0
    for off in range(start, end - 31, align):
        k = int.from_bytes(data[off:off + 32], "big")
        done += 1
        if k == 0 or k >= N:
            continue
        try:
            pub = ec.derive_private_key(k, curve).public_key().public_numbers()
        except Exception:
            continue
        if pub.x == tx and pub.y == ty:
            hits.append(off)
            print("HIT offset=0x%X key=%064x" % (off, k))
        if progress and done % 4096 == 0:
            el = time.time() - t0
            rate = done / el if el else 0
            eta = (total - done) / rate if rate else 0
            print("  ...0x%X / 0x%X  (%.0f cand/s, ETA %.0fs)"
                  % (off, end, rate, eta), flush=True)
    return hits


def selftest():
    d = 0x1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF
    pub = ec.derive_private_key(d, ec.SECP256R1()).public_key().public_numbers()
    blob = os.urandom(64) + d.to_bytes(32, "big") + os.urandom(64)
    print("selftest: target embedded at 0x40, scanning 160 bytes...")
    hits = scan(blob, target=(pub.x, pub.y), progress=False)
    print("hits:", [hex(h) for h in hits])
    assert hits == [0x40], "selftest failed"
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", nargs="?", help="固件 / 内存 dump 文件")
    ap.add_argument("--align", type=int, default=1, help="扫描步长（字节）")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--end", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0
    if not args.dump:
        ap.print_help()
        return 1

    data = open(args.dump, "rb").read()
    print("dump %s  %d bytes" % (args.dump, len(data)))
    print("target X %064x" % TARGET_X)
    print("target Y %064x" % TARGET_Y)
    hits = scan(data, align=args.align, start=args.start, end=args.end)
    if hits:
        print("\n找到 %d 处：" % len(hits))
        for h in hits:
            print("  offset 0x%X  key %064x" % (h, int.from_bytes(data[h:h + 32], "big")))
        return 0
    print("\n没找到。可能原因：私钥不在 dump 里 / 存在安全区 / dump 不完整")
    return 2


if __name__ == "__main__":
    sys.exit(main())
