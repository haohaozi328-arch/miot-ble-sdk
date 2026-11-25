"""常驻串口日志采集：边收边写文件（带时间戳），便于手势操作期间后台记录。
用法：python serial_log.py PORT SECONDS [BAUD] [OUTFILE]
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200
out = sys.argv[4] if len(sys.argv) > 4 else "logs/mesh_uart.log"

s = serial.Serial(port, baud, timeout=0.2)
s.reset_input_buffer()
t0 = time.time()
total = 0
print(f"open {port} @ {baud} -> {out} ({dur}s)", flush=True)
with open(out, "w", encoding="utf-8", errors="replace") as f:
    buf = b""
    while time.time() - t0 < dur:
        chunk = s.read(4096)
        if not chunk:
            continue
        total += len(chunk)
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            ts = time.strftime("%H:%M:%S")
            text = line.decode("utf-8", "replace").rstrip("\r")
            f.write(f"[{ts}] {text}\n")
            f.flush()
            print(f"[{ts}] {text}", flush=True)
s.close()
print(f"=== done bytes={total}", flush=True)
