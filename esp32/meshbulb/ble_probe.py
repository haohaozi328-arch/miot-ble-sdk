import asyncio, sys
from bleak import BleakScanner, BleakClient

TARGET_NAME = "careco654"
FE95 = "0000fe95-0000-1000-8000-00805f9b34fb"

async def main():
    print("scanning 8s ...")
    devs = await BleakScanner.discover(timeout=8.0, return_adv=True)
    hit = None
    for addr, (d, adv) in devs.items():
        name = adv.local_name or d.name
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if (name and TARGET_NAME in name) or FE95 in uuids:
            print("FOUND %s  name=%s rssi=%d uuids=%s" % (addr, name, adv.rssi, uuids))
            if hit is None:
                hit = d
        else:
            if name:
                print("  .. %s %s rssi=%d" % (addr, name, adv.rssi))
    if not hit:
        print("NOT FOUND")
        return
    print("connecting", hit.address)
    async with BleakClient(hit.address, timeout=20.0) as cli:
        print("connected, mtu=", getattr(cli, "mtu_size", "?"))
        for svc in cli.services:
            print("SVC %s" % svc.uuid)
            for ch in svc.characteristics:
                print("   CH %s  handle=%d props=%s" % (ch.uuid, ch.handle, ",".join(ch.properties)))
        for u in ["00000004-0000-1000-8000-00805f9b34fb",
                  "00000013-0000-1000-8000-00805f9b34fb",
                  "00000010-0000-1000-8000-00805f9b34fb"]:
            try:
                v = await cli.read_gatt_char(u)
                print("READ %s -> %d bytes: %s | %r" % (u[4:8], len(v), v.hex(" "), bytes(v)))
            except Exception as e:
                print("READ %s FAILED: %r" % (u[4:8], e))

asyncio.run(main())
