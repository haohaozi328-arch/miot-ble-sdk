"""连接 MHCB12G 并 dump GATT 服务/特征表（只读，不改任何东西）。
用法：python gatt_dump.py [ADDR]
"""
import sys, asyncio
from bleak import BleakClient

ADDR = sys.argv[1] if len(sys.argv) > 1 else '14:D8:81:E6:95:3C'

SVC_NAMES = {
    '00001800-0000-1000-8000-00805f9b34fb': 'GAP',
    '00001801-0000-1000-8000-00805f9b34fb': 'GATT',
    '00001827-0000-1000-8000-00805f9b34fb': 'Mesh Provisioning',
    '00001828-0000-1000-8000-00805f9b34fb': 'Mesh Proxy',
    '0000fe95-0000-1000-8000-00805f9b34fb': 'MIOT (FE95)',
}

async def main():
    async with BleakClient(ADDR, timeout=20) as c:
        print('connected, mtu =', c.mtu_size)
        for s in c.services:
            tag = SVC_NAMES.get(s.uuid.lower(), '?')
            print(f'\n[SVC] {s.uuid}  ({tag})  handle={s.handle}')
            for ch in s.characteristics:
                props = ','.join(ch.properties)
                print(f'   [CH ] {ch.uuid}  h={ch.handle}  props={props}')
                # 读一下可读的短特征，看看当前值
                if 'read' in ch.properties:
                    try:
                        v = await c.read_gatt_char(ch)
                        if len(v) <= 24:
                            print(f'         value({len(v)}B) = {v.hex()}')
                        else:
                            print(f'         value({len(v)}B) = {v[:24].hex()}...')
                    except Exception as e:
                        print(f'         read fail: {e}')

asyncio.run(main())
