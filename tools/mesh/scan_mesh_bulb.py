#!/usr/bin/env python3
"""扫周围广播：找目标 Mesh 灯（PID 0x4707 / yyzn.light.wy0a04）。

判读三件事：
  1. 有没有 0xFE95 的 MiBeacon（米家生态广告），以及它的 fc 位：registered / mesh
  2. 有没有 SIG Mesh 未配网 beacon（Service Data 0x1827，UUID 里带 8 字节 Device UUID）
  3. 有没有名字/厂商字段能直接认出这颗灯

用法: python tools/mesh/scan_mesh_bulb.py [秒数=20]
"""
import asyncio
import sys

from bleak import BleakScanner

TARGET_PID = 0x4707          # 18183 = yyzn.light.wy0a04
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 20

FE95 = "0000fe95-0000-1000-8000-00805f9b34fb"
FE9F = "0000fe9f-0000-1000-8000-00805f9b34fb"
P1827 = "00001827-0000-1000-8000-00805f9b34fb"
P1828 = "00001828-0000-1000-8000-00805f9b34fb"


def decode_fc(sd: bytes):
    if len(sd) < 6:
        return None
    fc_lo, fc_hi = sd[0], sd[1]
    return dict(
        ver=(fc_hi >> 4) & 0xF,
        auth=(fc_hi >> 2) & 0x3,
        solicited=(fc_hi >> 1) & 1,
        registered=fc_hi & 0x1,
        mesh=(fc_lo >> 7) & 1,
        fcnt=fc_lo & 0xF,
        pid=sd[2] | (sd[3] << 8),
        mac=":".join(f"{b:02X}" for b in sd[4:10]) if len(sd) >= 10 else "",
        rest=sd[4:].hex(),
    )


async def main():
    found = {}
    print(f"扫描 {SECS}s ...", flush=True)

    def cb(dev, adv):
        sd_map = adv.service_data or {}
        interesting = False
        lines = []

        sd = sd_map.get(FE95)
        if sd:
            interesting = True
            d = decode_fc(sd)
            tag = " ★目标PID" if d and d["pid"] == TARGET_PID else ""
            lines.append(f"    FE95 MiBeacon: pid=0x{d['pid']:04X} ver={d['ver']} "
                         f"auth={d['auth']} reg={d['registered']} solicited={d['solicited']} "
                         f"mesh={d['mesh']}{tag}\n    payload={d['rest']}")

        sd = sd_map.get(P1827)
        if sd:
            interesting = True
            uuid = sd[:16].hex()
            lines.append(f"    0x1827 Mesh Prov beacon: uuid={uuid} oob={sd[16] if len(sd) > 16 else '?'}")

        sd = sd_map.get(P1828)
        if sd:
            interesting = True
            lines.append(f"    0x1828 Mesh Proxy: {sd.hex()}")

        if not interesting:
            return

        key = dev.address.upper()
        if key in found:
            return
        found[key] = True
        print(f"\n[{dev.address}] rssi={adv.rssi} name={adv.local_name!r} "
              f"mfr={list((adv.manufacturer_data or {}).keys())}", flush=True)
        for ln in lines:
            print(ln, flush=True)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(SECS)
    await scanner.stop()
    print(f"\n共 {len(found)} 个带米家/ mesh 广告的设备", flush=True)


asyncio.run(main())
