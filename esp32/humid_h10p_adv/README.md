# HHGPK智能加湿器H10P 固件 —— ESP32-C3

**humidifier（加湿器）全新品类**：`zh001.humidifier.h10p` / PID **0x805F**（pdid 32863）。
从 `thermo_m76_adv` 派生。

- 官方产品库：`bt_bind_style=2`（BLE 直连）、`resolve_type=3`
- spec：`urn:miot-spec-v2:device:humidifier:0000A00E:zh001-h10p:1`

## 对象表（siid=2，piid<1000 → 0x4000|(siid<<10)|piid）

| 对象号 | spec | 类型 | 实测 |
|---|---|---|---|
| `0x4801` | 2.1 workmode 工作模式 | u8 0关/1-4档/5开 | ✅ 设备页 L1/L2/L3 |
| `0x4802` | 2.2 level 喷雾大小 | u8 1-100 | ✅ 设备页 1-100 |
| `0x4803` | 2.3 color 效果灯颜色 | uint32 RGB | ✅ 设备页效果灯 |
| `0x4804` | 2.4 light 灯模式 | u8 0关/1彩/2黄/3自定义 | 轮播 |
| `0x4805` | 2.5 auto-power-off | u16 0-720 | 轮播 |

spec 另有 string/int64 属性（program/unixtime 等）由 SDK 兜底 GET 处理，不进数值表。

## 实测（2026-11-25）

1. 米家「+ → 添加设备」发现「HHGPK智能加湿器H10P」→ 3/3 绑定
2. 设备页完整渲染：**已连接 / 开启关闭 / 模式(L1/L2/L3) / 喷雾大小(1-100) / 效果灯 / 剩余时间**

## 串口命令

`work 0-5` / `level 1-100` / `color RRGGBB` / `light 0-3` / `autoff 0-720` / `info` / `factory`。
BOOT 键：短按开关机；长按 5s 恢复出厂。

## 编译 / 烧录

```bash
cd esp32/humid_h10p_adv
idf.py build
idf.py -p COM36 flash
```

## 工程来源

从 `thermo_m76_adv` 派生：GATT/ECDH/MiBeacon2 骨架沿用，属性表改为 humidifier 的 5 个数值属性
（含首个 uint32 color），换 PID/MAC/NVS/广播对象。
