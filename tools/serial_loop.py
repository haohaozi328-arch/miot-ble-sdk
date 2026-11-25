"""串口自环测试：写入 PING 字节并读回，验证 USB-TTL + 驱动读写链路。
用法：python serial_loop.py [PORT] [BAUD]
"""
import sys, time, ctypes, ctypes.wintypes as wt

port = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

k = ctypes.WinDLL('kernel32', use_last_error=True)
GENERIC_READ, GENERIC_WRITE, OPEN_EXISTING = 0x80000000, 0x40000000, 3
INVALID = ctypes.c_void_p(-1).value

class COMSTAT(ctypes.Structure):
    _fields_ = [('fCtsHold', wt.DWORD, 1), ('fDsrHold', wt.DWORD, 1),
                ('fRlsdHold', wt.DWORD, 1), ('fXoffHold', wt.DWORD, 1),
                ('fXoffSent', wt.DWORD, 1), ('fEof', wt.DWORD, 1),
                ('fTxim', wt.DWORD, 1), ('fReserved', wt.DWORD, 25),
                ('cbInQue', wt.DWORD), ('cbOutQue', wt.DWORD)]

h = k.CreateFileW(f'\\\\.\\{port}', GENERIC_READ | GENERIC_WRITE, 0, None,
                  OPEN_EXISTING, 0, None)
if not h or h == INVALID:
    print('CreateFile failed', ctypes.get_last_error()); sys.exit(1)

msg = b'PING-12345\r\n'
w = wt.DWORD()
ok = k.WriteFile(h, msg, len(msg), ctypes.byref(w), None)
print('WriteFile', bool(ok), 'written', w.value, 'err', ctypes.get_last_error())

stat, err = COMSTAT(), wt.DWORD()
got = b''
t0 = time.time()
while time.time() - t0 < 3:
    if k.ClearCommError(h, ctypes.byref(err), ctypes.byref(stat)) and stat.cbInQue:
        data = ctypes.create_string_buffer(stat.cbInQue)
        r = wt.DWORD()
        if k.ReadFile(h, data, stat.cbInQue, ctypes.byref(r), None):
            got += data.raw[:r.value]
    time.sleep(0.03)
k.CloseHandle(h)
print('got', len(got), 'bytes:', got)
print('LOOPBACK', 'OK' if msg.rstrip() in got else 'FAIL')
