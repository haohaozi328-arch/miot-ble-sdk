#!/usr/bin/env python3
"""盯一盏灯：把目标 MAC 的 MiBeacon 位与 payload 变化逐条打印（配网过程记录用）。

用法: python tools/mesh/watch_bulb.py 8C:D0:B2:69:96:35 240 [out.log]
"""
import asyncio
import sys
import time

from bleak import BleakScanner

TARGET = sys.argv[1].upper() if len(sys.argv) > 1 else "8C:D0:B2:69:96:35"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 240
LOG = sys.argv[3] if len(sys.argv) > 3 else None

FE95 = "0000fe95-0000-1000-8000-00805f9b34fb"
P1827 = "00001827-0000-1000-8000-00805f9b34fb"
P1828 = "00001828-0000-1000-8000-00805f9b34fb"

out = open(LOG, "w", encoding="utf-8", buffering=1) if LOG else None
t0 = time.time()


def emit(s):
    line = f"[{time.time() - t0:6.1f}] {s}"
    print(line, flush=True)
    if out:
        out.write(line + "\n")


def decode(sd):
    fc_lo, fc_hi = sd[0], sd[1]
    return dict(
        fc=f"{fc_lo:02X}{fc_hi:02X}",
        ver=(fc_hi >> 4) & 0xF, auth=(fc_hi >> 2) & 3,
        solicited=(fc_hi >> 1) & 1, registered=fc_hi & 1,
        mesh=(fc_lo >> 7) & 1, fcnt=fc_lo & 0xF,
        pid=sd[2] | (sd[3] << 8), rest=sd[4:].hex(),
    )


async def main():
    last = None
    frames = 0

    def cb(dev, adv):
        nonlocal last, frames
        if dev.address.upper() != TARGET:
            return
        sd_map = adv.service_data or {}
        parts = []
        sd = sd_map.get(FE95)
        if sd:
            d = decode(sd)
            key = (d["fc"], d["rest"], d["pid"])
            frames += 1
            if key != last:
                last = key
                parts.append(f"FE95 pid=0x{d['pid']:04X} fc={d['fc']} ver={d['ver']} auth={d['auth']} "
                             f"reg={d['registered']} solicited={d['solicited']} mesh={d['mesh']} "
                             f"rest={d['rest']} rssi={adv.rssi}")
        for name, uuid in (("0x1827", P1827), ("0x1828", P1828)):
            sd = sd_map.get(uuid)
            if sd:
                parts.append(f"{name} {sd.hex()} rssi={adv.rssi}")
        for p in parts:
            emit(p)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    emit(f"watching {TARGET} for {SECS}s")
    await asyncio.sleep(SECS)
    await scanner.stop()
    emit(f"done, {frames} 帧")


asyncio.run(main())
