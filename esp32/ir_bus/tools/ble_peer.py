"""ir_bus 对端模拟器（PC 用，替代第二块 ESP32）

用 bleak 当 BLE 中央，模拟总线另一头：
  scan    → 扫附近 BLE 设备
  listen  → 连 Hub，订阅 CMD 通知，把收到的红外指令打印出来（带码表翻译）
  drive   → 连 Node，写 CMD 指令，并订阅 STATE 看继电器状态
  watch   → 连 Node，只订阅不写，纯接收 ESP 主动发的状态（含 3s 心跳）
  sniff   → 只扫广播不连接，读 ESP 广播里的状态字节

用法：
  python ble_peer.py scan
  python ble_peer.py sniff  --name IR-NODE --duration 20
  python ble_peer.py watch  --name IR-NODE
  python ble_peer.py listen --name IR-HUB
  python ble_peer.py drive  --name IR-NODE --cmd 0x15
  python ble_peer.py drive  --name IR-NODE --cmd 0x2F
  python ble_peer.py drive  --name IR-NODE --cmd 0x14 --times 2

依赖：pip install bleak
"""
import argparse
import asyncio
import json
import os
import sys
import time

from bleak import BleakClient, BleakScanner

SVC_UUID = "1a2b0001-9e3d-4c5f-8b7a-0d1e2f3a4b5c"
CMD_UUID = "1a2b0002-9e3d-4c5f-8b7a-0d1e2f3a4b5c"
STATE_UUID = "1a2b0003-9e3d-4c5f-8b7a-0d1e2f3a4b5c"

_HERE = os.path.dirname(os.path.abspath(__file__))
BOOK_CANDIDATES = [
    os.path.join(_HERE, "..", "ir_lib", "ir_codes.json"),
    os.path.join(_HERE, "ir_codes.json"),
]


def load_book():
    for path in BOOK_CANDIDATES:
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            return {int(k["cmd"], 16): f"{k['name']}/{k['zh']}" for k in data["keys"]}
        except FileNotFoundError:
            continue
        except Exception:
            return {}
    return {}


BOOK = load_book()


def label(cmd):
    return BOOK.get(cmd, "未收录")


async def find(name, timeout=15.0):
    print(f"[peer] 扫描 {timeout:g}s 找「{name}」…", flush=True)

    def match(d, adv):
        n = adv.local_name or d.name or ""
        return n.strip() == name

    dev = await BleakScanner.find_device_by_filter(match, timeout=timeout)
    if dev is None:
        print(f"[peer] 没找到 {name}。确认板子已上电、串口打印 BLE 就绪，"
              f"并用 `python ble_peer.py scan` 看实际广播名。")
        sys.exit(1)
    print(f"[peer] 找到 {dev.name} @ {dev.address}", flush=True)
    return dev


async def cmd_scan(_args):
    print("[peer] 扫描 8s…", flush=True)
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    items = []
    for addr, (d, adv) in found.items():
        items.append((adv.rssi, d.name or adv.local_name or "?", addr))
    for rssi, name, addr in sorted(items, reverse=True):
        print(f"  {rssi:4d} dBm  {name:16s} {addr}")
    return


async def cmd_listen(args):
    dev = await find(args.name)
    async with BleakClient(dev) as cli:
        print(f"[peer] 已连接: {cli.is_connected}", flush=True)

        def on_cmd(_handle, data: bytearray):
            cmd = data[0] if len(data) > 0 else 0
            arg = data[1] if len(data) > 1 else 0
            seq = data[2] if len(data) > 2 else 0
            print(f"<<< RX cmd=0x{cmd:02X} arg={arg} seq={seq}  = {label(cmd)}", flush=True)

        await cli.start_notify(CMD_UUID, on_cmd)
        print("[peer] 已订阅 CMD 通知，去让小爱音箱发红外（Ctrl+C 结束）", flush=True)
        try:
            while cli.is_connected:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
    print("[peer] 连接已断开")


async def cmd_sniff(args):
    """只扫广播、不连接：读 Node 广播里带来的状态字节。"""
    print(f"[peer] 只扫广播 {args.duration:g}s，不建立连接…", flush=True)
    t0 = time.time()
    last = {}

    def on_adv(dev, adv):
        name = (adv.local_name or dev.name or "").strip()
        if args.name and name != args.name:
            return
        st = None
        for cid, payload in (adv.manufacturer_data or {}).items():
            if cid == 0xFFFF and len(payload) >= 2 and payload[0] == 0x01:
                st = payload[1]
                break
        prev = last.get(dev.address, "unseen")
        if st == prev:
            return
        last[dev.address] = st
        ts = time.time() - t0
        state = "?" if st is None else ("通" if st else "断")
        print(f"[{ts:6.1f}s] AD {name:10s} {dev.address}  rssi={adv.rssi:4d}  "
              f"状态={state}" + (f"({st})" if st is not None else "（广播里没带状态）"),
              flush=True)

    async with BleakScanner(detection_callback=on_adv):
        await asyncio.sleep(args.duration)

    if not last:
        print("[peer] 没扫到匹配的广播（确认板子已上电且没被别人连着）")


async def cmd_watch(args):
    """连上后只订阅、不写：纯接收 ESP 主动发的状态（状态变化 + 3s 心跳）。"""
    dev = await find(args.name)
    t0 = time.time()
    last = [None]

    def on_state(_handle, data: bytearray):
        v = data[0] if data else 0xFF
        ts = time.time() - t0
        if last[0] is None:
            tag = "首次"
        elif v != last[0]:
            tag = "变化"
        else:
            tag = "心跳"
        last[0] = v
        print(f"[{ts:6.1f}s] <<< STATE relay={v}  ({tag})", flush=True)

    async with BleakClient(dev) as cli:
        print(f"[peer] 已连接: {cli.is_connected}", flush=True)
        await cli.start_notify(STATE_UUID, on_state)
        print("[peer] 只订阅、不写。ESP 会每 3s 主动推一次状态，Ctrl+C 结束", flush=True)

        async def inject():
            await asyncio.sleep(args.after)
            cmd = int(args.cmd, 16)
            await cli.write_gatt_char(CMD_UUID, bytes([cmd, 0, 1]), response=False)
            print(f">>> TX cmd=0x{cmd:02X}  = {label(cmd)}", flush=True)

        if args.cmd:
            asyncio.create_task(inject())

        try:
            while cli.is_connected:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
    print("[peer] 连接已断开")


async def cmd_drive(args):
    dev = await find(args.name)
    async with BleakClient(dev) as cli:
        print(f"[peer] 已连接: {cli.is_connected}", flush=True)

        def on_state(_handle, data: bytearray):
            v = data[0] if data else 0xFF
            print(f"<<< STATE relay={v}", flush=True)

        try:
            await cli.start_notify(STATE_UUID, on_state)
            print("[peer] 已订阅 STATE 通知", flush=True)
        except Exception as exc:
            print(f"[peer] 订阅 STATE 失败（可只读不订阅）: {exc}")

        # CCCD 写入完成后需要一点时间才在主机侧生效；立刻 write 会漏掉第一条 notify。
        await asyncio.sleep(0.3)

        cmd = int(args.cmd, 16)
        for i in range(args.times):
            payload = bytes([cmd, args.arg, i + 1])
            await cli.write_gatt_char(CMD_UUID, payload, response=args.with_response)
            print(f">>> TX cmd=0x{cmd:02X} arg={args.arg} seq={i + 1}  = {label(cmd)}", flush=True)
            await asyncio.sleep(0.4)

        await asyncio.sleep(0.3)
        try:
            st = await cli.read_gatt_char(STATE_UUID)
            print(f"[peer] 读回 STATE relay={st[0] if st else '?'}", flush=True)
        except Exception as exc:
            print(f"[peer] 读取 STATE 失败: {exc}")


def main():
    ap = argparse.ArgumentParser(description="ir_bus 对端模拟器")
    sub = ap.add_subparsers(dest="action", required=True)

    p = sub.add_parser("scan", help="扫描附近 BLE 设备")
    p.set_defaults(func=cmd_scan)

    p = sub.add_parser("listen", help="连 Hub，订阅红外指令")
    p.add_argument("--name", default="IR-HUB")
    p.set_defaults(func=cmd_listen)

    p = sub.add_parser("watch", help="连 Node，只订阅不写，纯接收 ESP 主动发的状态")
    p.add_argument("--name", default="IR-NODE")
    p.add_argument("--cmd", default=None, help="可选：N 秒后补发一条指令，看状态变化")
    p.add_argument("--after", type=float, default=5.0, help="--cmd 的延迟秒数")
    p.set_defaults(func=cmd_watch)

    p = sub.add_parser("sniff", help="只扫广播不连接，读广播里的状态字节")
    p.add_argument("--name", default="IR-NODE")
    p.add_argument("--duration", type=float, default=20.0)
    p.set_defaults(func=cmd_sniff)

    p = sub.add_parser("drive", help="连 Node，下发指令")
    p.add_argument("--name", default="IR-NODE")
    p.add_argument("--cmd", default="0x15", help="指令字，如 0x15=开 0x2F=关 0x14=翻转")
    p.add_argument("--arg", type=int, default=0)
    p.add_argument("--times", type=int, default=1)
    p.add_argument("--with-response", action="store_true", help="用带响应的写（默认 write-no-response）")
    p.set_defaults(func=cmd_drive)

    args = ap.parse_args()
    try:
        asyncio.run(args.func(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
