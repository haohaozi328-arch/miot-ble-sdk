#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 ESP32 中继日志里重组 0016 通道的多帧包，并对 pkg9 做熵/结构分析。

用法：
    python tools/pkg9_entropy.py logs/relay25.log
    python tools/pkg9_entropy.py logs/relay25.log --pkg 9 --dir TX
    python tools/pkg9_entropy.py logs/relay25.log --all

日志约定（relay.c 的 log_hex）：
    HX <TX|RX> <uuid4> <已打>/<总长> <hex...>
其中 TX = 手机→真灯，RX = 真灯→手机。

多帧协议（0016）：
    START  00 00 00 <pkg> <n> 00
    数据   <seq> 00 <payload...>
    ACK    00 00 01 01 / 00 00 01 00
"""
import argparse
import math
import re
import sys

LINE_RE = re.compile(r"HX\s+(TX|RX)\s+([0-9A-Fa-f]{4})\s+(\d+)/(\d+)\s+(.*)$")


def parse_frames(path, want_dir=None, want_uuid="0016"):
    """把分块日志拼回整帧，返回 [(dir, uuid, bytes), ...]"""
    acc = {}          # key=(dir,uuid) -> (每帧dict)
    frames = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE_RE.search(raw)
            if not m:
                continue
            d, u, done, total, hexs = m.group(1), m.group(2).upper(), int(m.group(3)), int(m.group(4)), m.group(5)
            if want_dir and d != want_dir:
                continue
            if want_uuid and u != want_uuid.upper():
                continue
            key = (d, u)
            if done == total:                      # 单块即整帧
                acc.setdefault(key, bytearray())
                buf = bytes(int(x, 16) for x in hexs.split())
                if len(buf) == total:
                    frames.append((d, u, buf))
                    acc[key] = bytearray()
                continue
            # 多块：按 done 递增拼接（同一帧内 done 单调）
            st = acc.setdefault(key, bytearray())
            chunk = bytes(int(x, 16) for x in hexs.split())
            if done - len(chunk) != len(st):       # 新帧开始
                st = bytearray()
                acc[key] = st
            st.extend(chunk)
            if len(st) == total:
                frames.append((d, u, bytes(st)))
                acc[key] = bytearray()
    return frames


def split_pkgs(frames):
    """按 START 帧切分，返回 [(pkg_no, declared_n, payload, dir)]"""
    out = []
    cur = None
    for d, u, b in frames:
        if len(b) >= 5 and b[0] == 0 and b[1] == 0 and b[2] == 0:
            if cur and cur["buf"]:
                out.append(cur)
            pkg = b[3]
            n = b[4] if len(b) > 4 else 0
            cur = {"pkg": pkg, "n": n, "buf": bytearray(), "dir": d,
                   "seq": 1, "frames": 0}
            continue
        if cur is None:
            continue
        if len(b) >= 4 and b[0] == 0 and b[1] == 1:      # ACK，收尾
            out.append(cur)
            cur = None
            continue
        if len(b) >= 2 and b[1] == 0 and b[0] == cur["seq"]:
            cur["buf"].extend(b[2:])
            cur["seq"] += 1
            cur["frames"] += 1
    if cur and cur["buf"]:
        out.append(cur)
    return out


def entropy(data):
    if not data:
        return 0.0
    freq = [0] * 256
    for c in data:
        freq[c] += 1
    n = len(data)
    h = 0.0
    for c in freq:
        if c:
            p = c / n
            h -= p * math.log2(p)
    return h


def analyze(tag, data, declared=None, nframes=None):
    n = len(data)
    h = entropy(data)
    uniq = len(set(data))
    # 最长连续 0x00
    longest0 = run = 0
    for c in data:
        run = run + 1 if c == 0 else 0
        longest0 = max(longest0, run)
    low = sum(1 for c in data if c < 0x20) / n
    print(f"\n=== {tag} ===")
    if nframes is not None:
        print(f"帧数 {nframes}  声明 {declared}  重组 {n} 字节")
    else:
        print(f"共 {n} 字节")
    # hexdump
    for off in range(0, n, 16):
        chunk = data[off:off + 16]
        hx = " ".join(f"{c:02X}" for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"  {off:04X}  {hx:<47}  {asc}")
    print(f"\n  熵值        {h:.3f} / 8.000")
    print(f"  独特字节    {uniq} / 256")
    print(f"  最长零段    {longest0}")
    print(f"  低值(<0x20) {low * 100:.1f}%")
    # 判读
    print("  判读        ", end="")
    if h > 7.90 and longest0 <= 1:
        print("几乎均匀随机 → 强加密（AES-CCM/AEAD 一类），离线解不开")
    elif h > 7.60:
        print("高熵但略低于理论值 → 可能加密，也可能明文里夹了密钥块")
    elif h > 6.90:
        print("偏高熵 → 很可能「明文结构 + 16B 随机密钥」，值得逐字段拆")
    else:
        print("明显有结构 → 大概率未加密或只做了简单变换")
    if longest0 >= 3:
        print(f"               注意：存在 {longest0} 字节连续 0x00，加密数据几乎不可能出现")
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--pkg", type=lambda s: int(s, 0), help="只看某个 pkg 号（如 9 / 0x09）")
    ap.add_argument("--dir", choices=["TX", "RX"], help="只看某个方向")
    ap.add_argument("--all", action="store_true", help="列出所有重组包")
    a = ap.parse_args()

    frames = parse_frames(a.log, want_dir=a.dir)
    if not frames:
        print("日志里没有 HX 行。确认固件是带完整 hexdump 的版本。")
        return 1
    print(f"解析到 {len(frames)} 帧（0016 通道）")

    pkgs = split_pkgs(frames)
    if not pkgs:
        print("没有重组出完整包（可能日志里只有零散帧）")
        return 1

    print(f"重组出 {len(pkgs)} 个包：", end="")
    print(", ".join(f"pkg{p['pkg']:02X}({len(p['buf'])}B,{p['dir']})" for p in pkgs))

    targets = pkgs
    if a.pkg is not None:
        targets = [p for p in pkgs if p["pkg"] == a.pkg]
    if not targets:
        print(f"没有 pkg {a.pkg} 的包")
        return 1

    for p in targets:
        analyze(f"pkg 0x{p['pkg']:02X}  {p['dir']}", bytes(p["buf"]),
                p["n"], p["frames"])

    if not a.all and a.pkg is None:
        big = max(targets, key=lambda p: len(p["buf"]))
        print(f"\n提示：最大的包是 pkg 0x{big['pkg']:02X}（{len(big['buf'])}B）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
