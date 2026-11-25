# 慕向暖阳加热杯垫 M76 固件 —— ESP32-C3

**thermostat（温控器）全新品类**：`muxnw.airrtc.m76` / PID **0x5A60**（pdid 23136）。
从 `kettle_adv` 派生，只保留 spec 需要的 4 个属性。

- 官方产品库：`bt_bind_style=2`（BLE 直连）、`resolve_type=3`
- spec：`urn:miot-spec-v2:device:thermostat:0000A031:muxnw-m76:1`

## 对象表（siid=2，piid<1000 按 0x4000|(siid<<10)|piid 推导）

| 对象号 | spec | 类型 |
|---|---|---|
| `0x4801` | 2.1 switch 开关 | u8 0关/1开 |
| `0x4802` | 2.2 mode 模式 | u8 枚举 1温水/2牛奶/3茶/4咖啡/5面包 |
| `0x4803` | 2.3 alarm-timer 预约 | u8 0-16 小时 |
| `0x4804` | 2.4 remaining-time 剩余 | u16 |

## 实测（2026-11-25）

1. 米家「+ → 添加设备」发现「慕向暖阳加热杯垫M76」→ 3/3 绑定
2. 云端记录：did `blt.3.1qvpavrkc4402` / MAC `10:00:3B:CE:E7:03`
3. 设备页完整渲染：**已连接 / 开始 / 模式设置（温水/牛奶/茶包/咖啡/面包）/ 预约设置（16H）**
4. BLE spec 会话建立后 4 属性初值全部推送（initial props pushed 4）

## 串口命令

`switch 0|1` / `mode 1-5` / `alarm 0-16` / `remain N` / `info` / `factory`。
BOOT 键：短按切开关；长按 5s 恢复出厂。

## 编译 / 烧录

```bash
cd esp32/thermo_m76_adv
idf.py build
idf.py -p COM36 flash        # 不要 erase-flash（丢绑定身份）
```

## 工程来源

从 `kettle_adv` 派生：GATT + ECDH + MiBeacon2 骨架沿用，状态表精简为 thermostat 的 4 个属性，
换 PID / MAC / NVS 命名空间 / 广播对象（0x4801-0x4804）/ 串口命令。
