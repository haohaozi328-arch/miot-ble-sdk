"""设置 mpair 后立刻扫原始广告字节，验证空中帧。"""
import asyncio, sys, time, serial
from bleak import BleakScanner

TARGET = '10:00:3B:CE:72:05'

def set_mpair(port='COM6', val='1'):
    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.3); s.reset_input_buffer()
    s.write(f'mpair {val}\r\n'.encode()); s.flush()
    time.sleep(1.0)
    out = s.read(65535).decode('utf-8', 'replace')
    s.close()
    for line in out.splitlines():
        if 'meshpair' in line or 'adv' in line.lower():
            print('  |', line.strip())

async def main(dur):
    set_mpair()
    seen = {}
    def cb(dev, adv):
        if dev.address.upper() != TARGET:
            return
        try:
            sd = adv.service_data.get('0000fe95-0000-1000-8000-00805f9b34fb', b'')
            data = bytes(sd).hex()
            if not data:
                pd = getattr(adv, 'platform_data', ())
                data = 'P:' + repr(pd[0])[:80]
        except Exception as e:
            data = f'ERR:{e}'
        seen[data] = seen.get(data, 0) + 1
    async with BleakScanner(cb):
        await asyncio.sleep(dur)
    print(f'--- {dur}s 内收到的不同广播 {len(seen)} 种 ---')
    for d, c in sorted(seen.items(), key=lambda x: -x[1]):
        print(f'  x{c:<4} len={len(d)//2:2d}  {d}')

asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 10))
