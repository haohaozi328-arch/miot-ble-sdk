#!/usr/bin/env python3
"""btsnoop_hci.log 解析器 —— 提取 ATT 层操作时间线（米家 Mesh 配网抓包用）。

用法:
  python btsnoop_parse.py <btsnoop_hci.log> [--filter MAC|handle] [--raw] [--out file]

btsnoop 格式: 8B "btsnoop\0" + 4B version + 4B datalink, 之后每条记录:
  original_len(4) inclusive_len(4) flags(4) drops(4) timestamp(8 BIG-ENDIAN)
  + HCI 包 (datalink 1002 = HCI UART: 1B type, 1005 =monitor)
ATT opcode 表: 0x10 error,0x11 MTUreq,0x12 MTUresp,0x08 readreq,0x09 readresp,
  0x12? ... 详见 ATT_SPEC。写/通知/指示是 Mesh 配网的主通道。
"""
import struct, sys, argparse, datetime

ATT_OPS = {
 0x01:"ERR_RSP",0x02:"MTU_REQ",0x03:"MTU_RSP",0x04:"FIND_INFO_REQ",0x05:"FIND_INFO_RSP",
 0x06:"READ_BY_TYPE_REQ",0x07:"READ_BY_TYPE_RSP",0x08:"READ_REQ",0x09:"READ_RSP",
 0x0A:"READ_BLOB_REQ",0x0B:"READ_BLOB_RSP",0x0C:"READ_MULTI_REQ",0x0D:"READ_MULTI_RSP",
 0x10:"READ_BY_GRP_REQ",0x11:"READ_BY_GRP_RSP",0x12:"WRITE_REQ",0x13:"WRITE_RSP",
 0x14:"WRITE_CMD",0x16:"PREP_WRITE_REQ",0x17:"PREP_WRITE_RSP",0x18:"EXEC_WRITE_REQ",
 0x19:"EXEC_WRITE_RSP",0x1A:"HANDLE_VALUE_NOTIFY",0x1B:"HANDLE_VALUE_IND",
 0x1D:"HANDLE_VALUE_CFM",0x23:"WRITE_SIGNED",0x52:"SIGNED_WRITE_CMD",
}
HCI_ACL_H4 = 0x02
HCI_EVT_H4 = 0x04

def parse(path, filt=None, show_raw=False):
    data = open(path, 'rb').read()
    if data[:8] != b'btsnoop\x00':
        sys.exit('not a btsnoop file')
    ver, dlink = struct.unpack('>II', data[8:16])
    off = 16
    recs = []
    while off + 24 <= len(data):
        olen, ilen, flags, drops = struct.unpack('>IIII', data[off:off+16])
        ts = struct.unpack('>Q', data[off+16:off+24])[0]
        off += 24
        pkt = data[off:off+ilen]
        off += ilen
        # flags bit0: 0=sent(host->ctrl) 1=received
        direction = 'RX' if flags & 1 else 'TX'
        recs.append((ts, direction, pkt))
    # datalink 1002: first byte is H4 type
    out = []
    for ts, direction, pkt in recs:
        if not pkt: continue
        h4 = pkt[0]
        body = pkt[1:]
        if h4 == HCI_ACL_H4 and len(body) >= 4:
            ch, hl = struct.unpack('<HH', body[:4])
            payload = body[4:4+hl]
            if len(payload) >= 4:
                l2len, l2cid = struct.unpack('<HH', payload[:4])
                l2 = payload[4:4+l2len]
                rec = (ts, direction, ch, l2cid, l2)
                if filt is None or filt == str(ch):
                    out.append(rec)
        elif h4 == HCI_EVT_H4 and len(body) >= 2:
            pass  # 事件暂时不展开（LE conn complete 等后续要）
    return out

def fmt(rec, show_raw):
    ts, direction, ch, cid, l2 = rec
    # btsnoop 时间戳 = 自公元 0 年起（proleptic Gregorian）的微秒数
    unix_us = ts - 62135596800 * 1_000_000
    try:
        t = datetime.datetime.fromtimestamp(unix_us / 1e6)
        stamp = t.strftime('%H:%M:%S.%f')[:-3]
    except (OSError, OverflowError, ValueError):
        stamp = f'ts={ts}'
    base = f"{stamp} {direction} conn={ch} cid={cid}"
    if cid == 0x0004 and len(l2) >= 1:          # ATT
        op = l2[0]
        name = ATT_OPS.get(op, f'ATT_{op:02X}')
        detail = l2[1:].hex()
        if op in (0x12,0x14,0x1A,0x1B,0x52) and len(l2) >= 3:
            h = struct.unpack('<H', l2[1:3])[0]
            base += f" ATT {name} h={h:04X} d={l2[3:].hex()}"
        elif op in (0x08,0x0A,0x0D) and len(l2) >= 3:
            h = struct.unpack('<H', l2[1:3])[0]
            base += f" ATT {name} h={h:04X}"
        elif op == 0x01 and len(l2) >= 5:
            h, err = struct.unpack('<HB', l2[1:4])
            base += f" ATT ERR h={h:04X} err=0x{err:02X}"
        else:
            base += f" ATT {name} d={detail}"
        if show_raw:
            base += f"  [{l2.hex()}]"
        return base
    if cid == 0x0006:                            # SMP 配对
        base += f" SMP op=0x{l2[0]:02X} d={l2[1:].hex()}"
        return base
    base += f" L2CAP d={l2.hex()}"
    return base

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--filter', default=None, help='connection handle 或包含的 MAC 片段')
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    recs = parse(a.path, a.filter)
    lines = [fmt(r, a.raw) for r in recs]
    txt = '\n'.join(lines)
    if a.out:
        open(a.out, 'w', encoding='utf-8').write(txt)
    print(txt[:12000])
    print(f'---- total {len(recs)} ATT/L2CAP records ----', file=sys.stderr)
