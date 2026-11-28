"""node_miot 串口控制台（PC 侧）

用途：PC 通过 USB 串口给执行节点发指令，并同时看设备侧事件日志。
      这一版节点没有自定义 BLE 服务，指令源就是串口。

用法：
  python serial_node.py info                 # 只读 3 秒，看启动/轮播日志
  python serial_node.py on                   # 发 on，再读 8 秒
  python serial_node.py off --listen 15      # 发 off，读 15 秒
  python serial_node.py toggle --listen 10

★坑：pyserial 打开端口默认会拉 DTR/RTS，ESP32-C3 的 USB-JTAG 会因此复位。
     这里显式置 False，避免一开串口板子就重启。
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", default=None,
                    help="on / off / toggle / info / factory / obj XXXX / bval N")
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", type=float, default=8.0, help="发完后监听秒数")
    args = ap.parse_args()

    try:
        ser = serial.Serial()
        ser.port = args.port
        ser.baudrate = args.baud
        ser.timeout = 0.3
        ser.dtr = False          # 关键：别让开串口把板子复位
        ser.rts = False
        ser.open()
    except Exception as exc:
        print(f"[node] 打开 {args.port} 失败: {exc}", flush=True)
        return 1

    print(f"[node] {args.port} @{args.baud} 已打开", flush=True)

    if args.cmd:
        payload = (args.cmd.strip() + "\n").encode()
        ser.write(payload)
        ser.flush()
        print(f">>> 已发送: {args.cmd}", flush=True)

    t0 = time.time()
    while time.time() - t0 < args.listen:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", "replace").rstrip("\r\n")
            if text:
                print(text, flush=True)
    ser.close()
    print("[node] 结束", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
