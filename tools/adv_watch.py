"""MHCB12G 实时广播监视：状态一变立即打印，便于配合 GPIO 手势操作。
用法：python adv_watch.py [秒数，默认 300]
"""
import sys, time, asyncio
from bleak import BleakScanner

TARGET_ADDR = '14:D8:81:E6:95:3C'

def decode(sd: bytes):
    fc_lo, fc_hi = sd[0], sd[1]
    pid = sd[2] | (sd[3] << 8)
    return dict(ver=(fc_hi >> 4) & 0xF, auth=(fc_hi >> 2) & 3,
                solicited=(fc_hi >> 1) & 1, registered=fc_hi & 1,
                mesh=(fc_lo >> 7) & 1, fc=f'{fc_lo:02X}{fc_hi:02X}',
                pid=pid, payload=sd[4:].hex())

async def main(sec):
    print(f'watching {TARGET_ADDR} for {sec}s ...', flush=True)
    last_fc = None
    n = 0
    def cb(dev, adv):
        nonlocal last_fc, n
        if dev.address.upper() != TARGET_ADDR:
            return
        sd = (adv.service_data or {}).get('0000fe95-0000-1000-8000-00805f9b34fb')
        if not sd:
            return
        n += 1
        d = decode(sd)
        if d['fc'] != last_fc or n % 20 == 1:
            t = time.strftime('%H:%M:%S')
            print(f"[{t}] frame#{n} FC={d['fc']} PID=0x{d['pid']:04X} "
                  f"reg={d['registered']} sol={d['solicited']} mesh={d['mesh']} "
                  f"payload={d['payload']}", flush=True)
            last_fc = d['fc']
    async with BleakScanner(cb, timeout=sec):
        await asyncio.sleep(sec)
    print(f'done, total {n} frames', flush=True)

if __name__ == '__main__':
    sec = float(sys.argv[1]) if len(sys.argv) > 1 else 300
    asyncio.run(main(sec))
