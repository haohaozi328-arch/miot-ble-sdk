"""同一窗口内并行观察：串口日志 + 空中广播，定位两者不一致的原因。"""
import asyncio, time, threading, serial
from bleak import BleakScanner

import sys
TARGET = (sys.argv[1] if len(sys.argv)>1 else '10:00:3B:CE:72:05').upper()
UUID = '0000fe95-0000-1000-8000-00805f9b34fb'
stop = False

def serial_task():
    s = serial.Serial('COM6', 115200, timeout=0.2)
    time.sleep(0.2); s.reset_input_buffer()
    if len(sys.argv) > 2: s.write(('mpair 1\r\n').encode()); s.flush()
    t0 = time.time()
    buf = b''
    while not stop:
        buf += s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            txt = line.decode('utf-8', 'replace').rstrip()
            if txt and ('adv' in txt.lower() or 'meshpair' in txt or 'ROM' in txt
                        or 'abort' in txt.lower() or 'Guru' in txt or 'reset' in txt.lower()):
                print(f'[S {time.time()-t0:5.1f}] {txt}', flush=True)
    s.close()

async def main(dur=10.0):
    th = threading.Thread(target=serial_task, daemon=True)
    th.start()
    await asyncio.sleep(0.6)
    seen = {}
    t0 = time.time()
    def cb(dev, adv):
        if dev.address.upper() != TARGET:
            return
        try:
            sd = bytes(adv.service_data.get(UUID, b'')).hex()
        except Exception as e:
            sd = f'ERR:{e}'
        seen.setdefault(sd, []).append(round(time.time() - t0, 1))
    async with BleakScanner(cb):
        await asyncio.sleep(dur)
    global stop
    stop = True
    print('--- 广播 ---')
    for d, ts in seen.items():
        print(f'  n={len(ts):<3} len={len(d)//2:2d} bytes={d}')

asyncio.run(main())
