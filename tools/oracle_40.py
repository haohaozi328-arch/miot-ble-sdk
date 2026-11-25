"""对真机做「oracle」探测：把 App 发给 ESP32 的那两条命令原样打给真机，
看真机怎么回。用来确定 0x40 到底该回什么。

用法：python tools/oracle_40.py 14:D8:81:E6:95:3C
"""
import asyncio, sys, time
from bleak import BleakClient, BleakScanner

ADDR = sys.argv[1] if len(sys.argv) > 1 else '14:D8:81:E6:95:3C'
FE95 = '-0000-1000-8000-00805f9b34fb'
C = lambda s: '0000' + s + FE95

H10, H19, H1C = C('0010'), C('0019'), C('001c')
got = []

def on_notify(uuid):
    def cb(_, data: bytearray):
        hx = ' '.join(f'{b:02X}' for b in data)
        print(f'   <<< [{uuid[:8]}] {len(data)}B  {hx}', flush=True)
        got.append((uuid, bytes(data)))
    return cb

async def main():
    print(f'扫描 {ADDR} ...')
    dev = await BleakScanner.find_device_by_address(ADDR, timeout=15.0)
    if not dev:
        print('没扫到，设备可能不在广播'); return
    print('找到:', dev.name, dev.address)

    async with BleakClient(dev, timeout=25.0) as cli:
        print('已连接 mtu=', cli.mtu_size)
        for u, n in ((H19, '0019'), (H1C, '001c')):
            try:
                await cli.start_notify(u, on_notify(n))
                print(f'订阅 {n} ok')
            except Exception as e:
                print(f'订阅 {n} 失败: {e}')

        async def w(uuid, tag, data):
            hx = ' '.join(f'{b:02X}' for b in data)
            print(f'>>> 写 [{tag}] {len(data)}B  {hx}', flush=True)
            try:
                await cli.write_gatt_char(uuid, data, response=True)
            except Exception as e:
                print('   写失败:', e)

        await w(H10, '0010', bytes([0xA4]))
        await asyncio.sleep(1.5)
        await w(H10, '0010', bytes([0x40, 0x00, 0x00, 0x00]))
        print('等 6 秒看回包 ...')
        await asyncio.sleep(6.0)

        print(f'\n共收到 {len(got)} 个 notify')
        for u, d in got:
            print(f'  {u[:8]}  {d.hex()}')
        try:
            await cli.disconnect()
        except Exception:
            pass

asyncio.run(main())
