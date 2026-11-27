#!/usr/bin/env python3
"""全量抓取周围 MiBeacon(0xFE95)/Mesh 未配网 beacon(0x1827)，带时间戳落盘。

用于抓 MHCB12G Mesh 2.0 配网瞬间的广播变化（不预置 MAC/PID，通吃）。
用法: python tools/mesh/watch_mhcb_adv.py 150 out.log
"""
import asyncio
import sys
import time
from bleak import BleakScanner

SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 150
OUT = sys.argv[2] if len(sys.argv) > 2 else "_mhcb_adv.log"
FE95 = "0000fe95-0000-1000-8000-00805f9b34fb"
P1827 = "00001827-0000-1000-8000-00805f9b34fb"
P1828 = "00001828-0000-1000-8000-00805f9b34fb"
WANT = {FE95, P1827, P1828}

seen = {}
t0 = time.time()


def cb(dev, adv):
    for uuid, data in adv.service_data.items():
        if uuid.lower() not in WANT:
            continue
        key = (dev.address, uuid.lower(), bytes(data).hex())
        now = time.time() - t0
        if seen.get(key, -999) + 1.0 > now:   # 同一种帧 1s 内只记一次
            continue
        seen[key] = now
        line = f"[{now:7.2f}] {dev.address} {uuid.split('-')[0]} {bytes(data).hex()}\n"
        with open(OUT, "a", encoding="utf-8") as f:
            f.write(line)


async def main():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(f"# start {time.strftime('%Y-%m-%d %H:%M:%S')} secs={SECS}\n")
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(SECS)
    await scanner.stop()


asyncio.run(main())
