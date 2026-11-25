#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""adv_dump2.py —— 只读：并排打印两台设备的原始广播（service_data / manufacturer_data）。
用法：python tools/adv_dump2.py [秒数]
"""
import sys, asyncio
from bleak import BleakScanner

WANT = {
    "14:D8:81:E6:95:3C": "REAL",
    "0A:1B:2C:3D:4E:02": "FAKE",
}


def dec(sd):
    if len(sd) < 4:
        return None
    fc = sd[0] | (sd[1] << 8)
    pid = sd[2] | (sd[3] << 8)
    return "FC=%04X pid=%04X rest=%s" % (fc, pid, sd[4:].hex())


async def main(sec):
    got = {}
    def cb(dev, adv):
        tag = WANT.get(dev.address.upper())
        if not tag:
            return
        sd = (adv.service_data or {})
        mf = (adv.manufacturer_data or {})
        line = []
        for u, v in sd.items():
            line.append("  sd[%s] = %s   %s" % (u[-8:-4] if len(u) > 8 else u, v.hex(), dec(v) or ""))
        for k, v in mf.items():
            line.append("  mf[%04X] = %s" % (k, v.hex()))
        got[tag] = (dev.name, adv.rssi, "\n".join(line) or "  (无 sd/mf)")

    sc = BleakScanner(detection_callback=cb)
    await sc.start()
    await asyncio.sleep(sec)
    await sc.stop()
    for tag in ("REAL", "FAKE"):
        if tag in got:
            name, rssi, body = got[tag]
            print("%s  %-22s rssi=%s" % (tag, name, rssi))
            print(body)
        else:
            print("%s  未出现" % tag)


if __name__ == "__main__":
    asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 12))
