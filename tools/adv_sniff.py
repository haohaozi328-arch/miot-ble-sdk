"""MHCB12G 广播嗅探：解码 MiBeacon 帧头，判定 registered / solicited / mesh 状态。
用法：python adv_sniff.py [秒数，默认 20]
"""
import sys, asyncio
from bleak import BleakScanner

TARGET_SUBSTR = ('XRY', 'SW4B01', 'SWITCH')

def decode_mibeacon(sd: bytes):
    """sd = 0xFE95 service data（去掉 UUID 后的 payload）"""
    if len(sd) < 6:
        return None
    fc_lo, fc_hi = sd[0], sd[1]
    pid = sd[2] | (sd[3] << 8)
    ver = (fc_hi >> 4) & 0xF
    auth = (fc_hi >> 2) & 0x3
    solicited = (fc_hi >> 1) & 1
    registered = fc_hi & 1
    mesh_bit = (fc_lo >> 7) & 1
    fcnt = fc_lo & 0x0F
    return dict(ver=ver, auth=auth, solicited=solicited, registered=registered,
                mesh=mesh_bit, fcnt=fcnt, pid=pid, rest=sd[4:].hex())

async def main(sec):
    print(f'scanning {sec}s ...')
    seen = {}
    def cb(dev, adv):
        name = dev.name or (adv.local_name or '')
        key = dev.address
        hit = any(s.upper() in name.upper() for s in TARGET_SUBSTR)
        for mfr in (adv.manufacturer_data or {}):
            pass
        sd_fe95 = (adv.service_data or {}).get('0000fe95-0000-1000-8000-00805f9b34fb')
        if hit or sd_fe95:
            rec = seen.setdefault(key, {'name': name, 'n': 0})
            rec['n'] += 1
            rec['name'] = name or rec['name']
            rec['adv'] = bytes(adv.raw_descriptor).hex() if hasattr(adv, 'raw_descriptor') else None
            if sd_fe95:
                rec['sd'] = sd_fe95.hex()
                d = decode_mibeacon(sd_fe95)
                if d:
                    rec['dec'] = d
    async with BleakScanner(cb, timeout=sec) as sc:
        await asyncio.sleep(sec)
    for addr, r in seen.items():
        print(f"\n{addr}  name={r['name']!r}  frames={r['n']}")
        if 'sd' in r:
            print(f"  MiBeacon sd: {r['sd']}")
        if 'dec' in r:
            d = r['dec']
            print(f"  PID=0x{d['pid']:04X} ver={d['ver']} authMode={d['auth']} "
                  f"registered={d['registered']} solicited={d['solicited']} "
                  f"meshbit={d['mesh']} fcnt={d['fcnt']}")
            print(f"  payload: {d['rest']}")
        else:
            print(f"  adv(raw): {r.get('adv')}")
    if not seen:
        print('（没扫到目标）')

if __name__ == '__main__':
    sec = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    asyncio.run(main(sec))
