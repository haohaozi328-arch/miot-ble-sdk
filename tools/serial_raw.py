"""CH340 驱动异常时的原始串口读取器：绕过 SetCommState，直接 ReadFile。
前提：设备默认波特率已是 115200（用 GetCommState 已确认）。
用法：python serial_raw.py PORT SECONDS [OUTFILE]
"""
import sys, time, ctypes, ctypes.wintypes as wt

port = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
out = sys.argv[3] if len(sys.argv) > 3 else 'logs/mesh_uart.log'

k = ctypes.WinDLL('kernel32', use_last_error=True)
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
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
    print('CreateFile failed err=', ctypes.get_last_error(), flush=True)
    sys.exit(1)

stat = COMSTAT()
err = wt.DWORD()
total = 0
t0 = time.time()
buf = b''
print(f'raw read {port} for {dur}s -> {out}', flush=True)
with open(out, 'w', encoding='utf-8', errors='replace') as f:
    while time.time() - t0 < dur:
        if not k.ClearCommError(h, ctypes.byref(err), ctypes.byref(stat)):
            time.sleep(0.05)
            continue
        n = stat.cbInQue
        if n:
            data = ctypes.create_string_buffer(n)
            read = wt.DWORD()
            if k.ReadFile(h, data, n, ctypes.byref(read), None):
                chunk = data.raw[:read.value]
                total += len(chunk)
                buf += chunk
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    ts = time.strftime('%H:%M:%S')
                    text = line.decode('utf-8', 'replace').rstrip('\r')
                    f.write(f'[{ts}] {text}\n')
                    f.flush()
                    print(f'[{ts}] {text}', flush=True)
        else:
            time.sleep(0.03)
if buf:
    f.write(f'[{time.strftime("%H:%M:%S")}] {buf.decode("utf-8", "replace")}\n')
k.CloseHandle(h)
print(f'=== done bytes={total}', flush=True)
