# s800_adv —— 米家八电极体脂秤 S800（小白版，广播型）

| 项 | 值 |
|---|---|
| 产品 | 米家八电极体脂秤 S800 / MJTZC04YM |
| spec | `xiaomi.scales.ms116`（[home.miot-spec.com](https://home.miot-spec.com/spec/xiaomi.scales.ms116)） |
| PID | **0x51E2**（pdid 20962） |
| 广播名 | `S800` |
| MAC | `10:00:3B:CE:75:03`（派生后缀 0x75） |
| NVS 命名空间 | `s800` |
| 代码量 / 体积 | 约 700 行 / 582,784 B（零告警） |

## 为什么走广播，不走 GATT spec 会话

S800 的称重数据是 **MiBeacon 事件 `3.e.1022`**，不是 GATT 会话里推的。
依据：XiaomiGateway3 为这台设备写了专属解码器 `BLEScaleS800`
（`converters/devices.py` 20962 条目 → `core/converters/mibeacon.py`）：

```python
MapConv("battery_low", "binary_sensor", mi="4.e.1001.p.1", ...)
BLEScaleS800("p2", mi="3.e.1022.p.2")   # 阶段帧
BLEScaleS800("p3", mi="3.e.1022.p.3")   # 数据帧
```

对象号同样用 MiBeacon2 公式（与 pir2s / occ 一致）：

```
eid = 0x4000 | (0x200 事件) | (siid << 10) | (iid - 1000)
3.e.1022  →  0x4000|0x200|0xC00|22  =  0x4E16
```

## 事件载荷与解码规则

载荷 8 字节 = 两个 uint32 参数（按 spec 顺序）：

| 参数 | 含义 |
|---|---|
| `p.2` | `(kind<<24) | (stage<<16) | data` —— **阶段帧** |
| `p.3` | 该阶段的数据 —— **数据帧** |

GW3 实测的语义（`p.3` 怎么解由「上一个 `p.2` 阶段」决定）：

| 上一阶段 | `p.3` 解码 |
|---|---|
| `0x05` | `weight = ((p3>>16) & 0xFFFF) / 100` kg |
| `0x15` | `heart_rate = ((p3>>24) & 0xFF) + 50` |

所以一次称重必须**严格成对按序发**：阶段帧 `0x05` → 数据帧（体重）→ 阶段帧 `0x15` → 数据帧（心率）。

## 对象表

| 对象 | 含义 | 说明 |
|---|---|---|
| `0x4E16` | 称重事件 `3.e.1022` | 8B：p2 阶段 + p3 数据 |
| `0x100A` | 电量 | 标准对象（公式值网关不映射，沿用 pir2s 结论） |

## 按键操作

| 操作 | 行为 |
|---|---|
| 短按 BOOT | 模拟一次上秤：`0x05` 阶段 → 体重 → `0x15` 阶段 → 心率 |
| 双击 BOOT | 切换模拟体重档位：50.00 / 64.50 / 80.00 kg |
| 长按 5 秒 | 恢复出厂（擦 NVS，重启后可重新添加） |

## spec 属性表（服务端读属性用）

按 ms116 spec 铺了 2 / 3 / 4 / 9 / 10 / 13 的常用项；字符串类（设备型号、SN、MAC、固件版本）
走 `s800_get` 兜底。首次绑定后按串口日志里 `spec GET x.y` / `spec ACTION siid=..` 补齐插件真正要的项。

## 绑定注意

- 继承 pir2s 的 **-28 修复**：连接期间保持「请求配对」形态广播（非连接 beacon）；
- 继承 `src/miot_auth.c` 的 **device info 150ms 重发**兜底（防 -55）；
- 若首次绑定报 `-28` / `-55`：先彻底杀掉米家后台重试一次，再看固件（见 docs/troubleshooting Q12 / Q13）。

## 构建 / 烧录

```bash
bash tools/build_all.sh s800_adv
cd esp32/s800_adv/build && esptool --chip esp32c3 --port COM6 write-flash @flash_args
```

> 芯片是 **ESP32-C3**（不是 C6）；串口 tap 占着 COM6 时先停 tap 再烧。
