# 小米水浸卫士2（xiaomi.flood.oh83w / 0x6375）小白版 —— ESP32-C3

**一根 Type-C 线、几条串口命令就能用。**

- 目标芯片：ESP32-C3（USB-Serial/JTAG 直连）
- 模拟型号：`xiaomi.flood.oh83w`，PID **0x6375**
- 上报：底部浸水 / 顶部淋水 / 电量 → **beacon→网关→云端**
- 通道：MiBeacon V5 加密广播 + 0xFE95 GATT（绑定）

> 骨架同 `leak_adv`。原理见 [`../t3pro_adv/README.md`](../t3pro_adv/README.md)。

## 一、串口命令

| 命令 | 作用 |
|---|---|
| `bot 0/1` | 底部浸水（2.1006 → 0x4806 bool） |
| `top 0/1` | 顶部淋水（2.1123 → 0x487B bool） |
| `batt 95` | 电量 %（3.1003 → 0x4C03 u8） |
| `fire 0/1/2` | 只发指定槽事件帧 |
| `info` / `factory` | 状态 / 恢复出厂 |

## 二、编译烧录

```bash
. E:\ESP\esp_env.ps1
cd E:\ESP\c3supermini\miot_sdk\esp32\flood2_adv
python E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py build
python E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py -p COM36 flash
```

## 三、米家添加

1. **+ → 添加设备**（菜单第一项）
2. 发现设备 → 点它 → 绑定完成

## 四、对象号

| 属性 | siid.piid | 对象 | 类型 |
|---|---|---|---|
| 底部浸水 | 2.1006 | **0x4806** | bool 0/1 |
| 顶部淋水 | 2.1123 | **0x487B** | bool 0/1 |
| 电量 | 3.1003 | **0x4C03** | u8 0-100 |

公式：`eid = 0x4000|(siid<<10)|(piid-1000)`。
