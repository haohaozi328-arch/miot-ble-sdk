#!/usr/bin/env python3
"""从真机配网 btsnoop 中重建 Mesh 安全芯片认证的全部多帧包。

目标：拿到真机的 pkg4（设备签名，发给 /v2/blemesh/bind 的 sign 字段），
以及整段会话的 pkg1/3/7/8/9，用于离线枚举「pkg4 到底签了哪些字节」。

用法:
  python pkg4_extract.py <btsnoop_hci.log> [--outdir _cert/rx]

协议（双向对称，见 OooO0o.java / replay2.log 实测）:
  START  = 00 00 00 <pkg> <n> 00   （发送方先发，接收方回 00 00 01 01）
  数据帧 = [seq][00][<=18B] × n    （收方回 00 00 01 00 表示收全）
方向：RX(设备->手机) 的包 = 设备发(pkg1 证书 / pkg3 84B / pkg4 签名)
      TX(手机->设备) 的包 = App 发(pkg7 证书链 / pkg3 公钥 / pkg8 挑战 / pkg9 配网数据)
"""
import struct, sys, argparse, os, datetime
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from btsnoop_parse import parse as btsnoop_parse

ATT_NOTIFY = (0x1A, 0x1B)
ATT_WRITE  = (0x12, 0x14, 0x52)

def ts_str(ts):
    unix_us = ts - 62135596800 * 1_000_000
    try:
        return datetime.datetime.fromtimestamp(unix_us / 1e6).strftime('%H:%M:%S.%f')[:-3]
    except Exception:
        return f'ts={ts}'

def extract(path, outdir):
    recs = btsnoop_parse(path)
    # per (conn, handle) 一个重组状态机
    st = {}      # key -> dict(pkg=.., need=.., got=.., buf=bytearray, frames=[])
    pkgs = []    # (ts, conn, handle, dir, pkgid, bytes)
    log = []
    for ts, direction, ch, cid, l2 in recs:
        if cid != 4 or len(l2) < 3:
            continue
        op = l2[0]
        if op in ATT_NOTIFY:
            h = struct.unpack('<H', l2[1:3])[0]
            d = l2[3:]
            dkey = 'DEV'
        elif op in ATT_WRITE:
            h = struct.unpack('<H', l2[1:3])[0]
            d = l2[3:]
            dkey = 'APP'
        else:
            continue
        if not d:
            continue
        key = (ch, h, dkey)
        s = st.setdefault(key, dict(pkg=None, need=0, got=0, buf=bytearray()))
        # START?
        if len(d) == 6 and d[0] == 0 and d[1] == 0 and d[2] == 0 and d[5] == 0 and d[4] > 0:
            s.update(pkg=d[3], need=d[4], got=0, buf=bytearray())
            log.append(f"{ts_str(ts)} {dkey} conn={ch} h={h:04X} START pkg={d[3]} n={d[4]}")
            continue
        # ACK?
        if len(d) == 4 and d[0] == 0 and d[1] == 0 and d[2] == 1:
            continue
        # 数据帧?
        if s['need'] and len(d) >= 2 and d[1] == 0:
            s['buf'] += d[2:]
            s['got'] += 1
            if s['got'] >= s['need']:
                log.append(f"{ts_str(ts)} {dkey} conn={ch} h={h:04X} DONE  pkg={s['pkg']} "
                           f"bytes={len(s['buf'])} frames={s['got']}")
                pkgs.append((ts, ch, h, dkey, s['pkg'], bytes(s['buf'])))
                s.update(pkg=None, need=0, got=0, buf=bytearray())
            continue
        log.append(f"{ts_str(ts)} {dkey} conn={ch} h={h:04X} ??? d={d.hex()}")
    print(f"== {path}: {len(pkgs)} 个完整包 ==")
    for line in log:
        print(line)
    os.makedirs(outdir, exist_ok=True)
    saved = []
    for ts, ch, h, dkey, pkg, data in pkgs:
        name = f"{'real' if dkey=='DEV' else 'app'}_pkg{pkg}_c{ch}.bin"
        p = os.path.join(outdir, name)
        open(p, 'wb').write(data)
        saved.append(p)
        print(f"[saved] {name}  {len(data)}B  head={data[:16].hex()}")
    return saved

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--outdir', default='_cert/rx')
    a = ap.parse_args()
    extract(a.path, a.outdir)
