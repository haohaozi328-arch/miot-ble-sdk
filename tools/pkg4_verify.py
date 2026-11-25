#!/usr/bin/env python3
"""pkg4 签名内容逆向 —— 抓到真机 pkg4 后离线枚举候选明文验签。

背景：App 把 pkg4 原样放进 /v2/blemesh/bind 的 sign 字段，服务器验签。
我们不知道签名覆盖哪些字节。真机证书（logs/real_cert.der）里有公钥，
pkg3(84B) 里还有设备自己的 ECDH 公钥 —— 两个候选验签钥都试。

用法:
  python pkg4_verify.py --cert logs/real_cert.der --rx _cert/rx
                       [--pkg4 real_pkg4_cX.bin] [--session cX]

候选明文 = 下列基本块的两两/三三组合（按常见顺序）+ 各自的 SHA-256：
  cert   = 设备证书(pkg1)
  blob   = pkg3 84B
  devinfo= blob[0:12]
  manu   = blob[12:20]
  dpub   = blob[20:84]  设备 ECDH 公钥
  apub   = App pkg3 64B
  ssign  = App pkg8 64B（服务器挑战/签名）
  scert  = App pkg7 379B（服务器证书链）
"""
import os, sys, glob, hashlib, argparse
from cryptography.hazmat.primitives.serialization import load_der_x509_certificate
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes
from cryptography.exceptions import InvalidSignature

def load_pubkeys(cert_path, rx):
    keys = []
    try:
        cert = load_der_x509_certificate(open(cert_path, 'rb').read())
        keys.append(('cert(pub)', cert.public_key()))
        print(f"[cert] {cert.subject.rfc4514_string()}  serial={cert.serial_number:#x}")
    except Exception as e:
        print(f"[warn] 证书加载失败: {e}")
    # 设备 ECDH 公钥：real_pkg3 84B 的最后 64B（无 04 前缀的 X||Y）
    for f in sorted(glob.glob(os.path.join(rx, 'real_pkg3*.bin'))):
        b = open(f, 'rb').read()
        if len(b) >= 84:
            pub = b[20:84]
            try:
                k = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b'\x04' + pub)
                keys.append((f'pkg3Eph({os.path.basename(f)})', k))
            except Exception:
                pass
    return keys

def try_verify(key, sig, msg):
    # 签名可能是 64B raw r||s，也可能 DER —— 两种都试；明文可能先过 SHA-256
    if len(sig) == 64:
        r = int.from_bytes(sig[:32], 'big')
        s = int.from_bytes(sig[32:], 'big')
        der = b'\x30' + bytes([0x44 + (1 if r > 0x7f else 0) + (1 if s > 0x7f else 0)])
        # 手工 DER 编码太啰嗦，用 ecdsa 库兜底
        import ecdsa as _e
        vk = _e.VerifyingKey.from_string(sig[:0] or key.public_numbers().x.to_bytes(32,'big')
                                         + key.public_numbers().y.to_bytes(32,'big'),
                                         curve=_e.SECP256r1)
        for m in (msg, hashlib.sha256(msg).digest()):
            for prehash in (False, True):
                try:
                    if prehash:
                        vk.verify_digest(sig, m)          # m 已是哈希
                    else:
                        vk.verify(sig, m, hashfunc=hashlib.sha256)  # 先哈希再验
                    return True
                except Exception:
                    pass
        return False
    else:
        for m in (msg, hashlib.sha256(msg).digest()):
            try:
                key.verify(sig, m, ec.ECDSA(hashes.SHA256()))
                return True
            except InvalidSignature:
                pass
            except Exception:
                pass
        return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cert', default='logs/real_cert.der')
    ap.add_argument('--rx', default='_cert/rx')
    ap.add_argument('--pkg4', default=None)
    ap.add_argument('--session', default=None, help='连接号 cXX，过滤同会话的包')
    a = ap.parse_args()
    rx = a.rx

    # 找 pkg4
    if a.pkg4:
        p4 = open(a.pkg4, 'rb').read()
    else:
        cands = sorted(glob.glob(os.path.join(rx, 'real_pkg4*.bin')))
        if not cands:
            sys.exit('没有找到 real_pkg4*.bin —— 先跑 pkg4_extract.py')
        p4 = open(cands[0], 'rb').read()
        print(f"[pkg4] 用 {cands[0]} ({len(p4)}B)")
    # 找同会话（同 conn）的其余包：优先 session 过滤，否则全用
    sess = a.session
    def pick(pat):
        fs = sorted(glob.glob(os.path.join(rx, pat)))
        if sess:
            fs = [f for f in fs if sess in os.path.basename(f)] or fs
        return fs[0] if fs else None
    f = lambda p: open(p, 'rb').read() if p else b''
    cert_dev = f(pick('real_pkg1*.bin'))
    blob     = f(pick('real_pkg3*.bin'))
    apub     = f(pick('app_pkg3*.bin'))
    ssign    = f(pick('app_pkg8*.bin'))
    scert    = f(pick('app_pkg7*.bin'))
    print(f"[块] cert={len(cert_dev)}B blob={len(blob)}B apub={len(apub)}B "
          f"ssign={len(ssign)}B scert={len(scert)}B")

    keys = load_pubkeys(a.cert, rx)
    if blob and len(blob) >= 84:
        devinfo, manu, dpub = blob[:12], blob[12:20], blob[20:84]
    else:
        devinfo = manu = dpub = b''

    blocks = {
        'cert': cert_dev, 'blob': blob, 'devinfo': devinfo, 'manu': manu,
        'dpub': dpub, 'apub': apub, 'ssign': ssign, 'scert': scert,
        'sha_cert': hashlib.sha256(cert_dev).digest(),
        'sha_dpub': hashlib.sha256(dpub).digest(),
        'sha_apub': hashlib.sha256(apub).digest(),
        'sha_ssign': hashlib.sha256(ssign).digest(),
        'sha_blob': hashlib.sha256(blob).digest(),
        'p4': p4,
    }
    names = list(blocks)
    cands = []
    for n in names:                                  # 单块
        cands.append((n, blocks[n]))
    for i in range(len(names)):                      # 两块有序组合
        for j in range(len(names)):
            if i == j: continue
            cands.append((f'{names[i]}|{names[j]}',
                          blocks[names[i]] + blocks[names[j]]))
    for i in range(len(names)):                      # 三块有序组合（限常用块，防爆量）
        for j in range(len(names)):
            if j == i: continue
            for k in range(len(names)):
                if k == j or k == i: continue
                trio = (names[i], names[j], names[k])
                if set(trio) & {'devinfo', 'manu', 'dpub', 'apub', 'ssign'} == set(trio):
                    cands.append(('|'.join(trio),
                                  blocks[names[i]] + blocks[names[j]] + blocks[names[k]]))
    print(f"[候选] 共 {len(cands)} 个明文 × {len(keys)} 个公钥 × raw/DER × 直哈希/预哈希")
    hit = False
    for kn, key in keys:
        for cn, msg in cands:
            if try_verify(key, p4, msg):
                print(f"\n★★★ 命中!  公钥={kn}  明文={cn}")
                print(f"    明文长度={len(msg)}B  hex={msg.hex()}")
                hit = True
    if not hit:
        print('\n未命中 —— 签名可能覆盖其他结构（TLV/带长度前缀/含随机数）。')
        print('把 pkg4 与当轮全部包的时间戳对齐后再扩充候选。')

if __name__ == '__main__':
    main()
