# 米家口袋照片打印机 Pro 可绑定固件 —— ESP32-C3

从 `printer_label_adv`（标签打印机 0x34FE）派生的**口袋照片打印机 Pro** 固件。

- 目标芯片：ESP32-C3（USB-Serial/JTAG）
- 模拟型号：米家口袋照片打印机 Pro `xiaomi.printer.ricott`，PID **0x5F6D**（pdid 24429）
  - 官方产品库 `bt_bind_style=2`（BLE 直连）、`resolve_type=3`（广播可识别）
  - spec：`urn:miot-spec-v2:device:printer:0000A060:xiaomi-ricott:1`
- 通道：MiBeacon V5 加密广播（空对象心跳帧，ricott 无业务属性）+ 0xFE95 GATT

## 实测结果（2026-11-25）

1. 米家「+ → 添加设备」发现列表出现 **「米家口袋照片打印机Pro」**，点卡片完成 3/3 绑定
2. 云端设备记录：`xiaomi.printer.ricott` / did `blt.3.1qvon88q04o00` / MAC `10:00:3B:CE:05:08`
3. 设备页完整渲染：打印任务 / 经典照片 / 动态照片 / 模板 / AR照片 / 留声照片

## ★关于打印通道（2026-11-25 已定性）

照片打印的**图片数据不走标准 spec 属性**。ricott 的官方 spec 只有 device-information
（5 个只读 string）+ indicator-light / battery 两个空服务。

**打印通道不走 BLE**：绑定完成后，插件对设备发起**经典蓝牙（BR/EDR）RFCOMM/SPP**
连接（UUID `00001101`，手机 logcat 实证），协议为 Hannto 家族 0x7E 分帧 +
DH 握手 + AES-128-ECB。ESP32-C3 无经典蓝牙 → 设备页「连接中」永远转圈，
**本工程（C3）能力上限就是绑定 + 设备页渲染**，打印通道需要 ESP32 经典款
（WROOM-32 双模）重做固件。

完整分析（协议栈细节 / logcat 证据 / 实现路线图 / 验证清单）见
[`docs/ricott-print-protocol-analysis.md`](../../docs/ricott-print-protocol-analysis.md)。
参考实现：`refs/xiaomi-photo-printer/`（tuat-yate，Pro 海外版全协议 Python）、
`refs/mizink/`（basil 逆向，传输层全景）。

## 编译 / 烧录

```bash
cd esp32/printer_ricott_adv
idf.py build
idf.py -p COM36 flash        # 不要 erase-flash（丢绑定身份）
```

## 米家添加

米家 → **+** → **添加设备** → 发现列表出现「米家口袋照片打印机Pro」→ 点卡片 →
`REGISTER COMPLETE 0x11` → 完成房间/命名。

## 串口命令

| 命令 | 作用 |
|---|---|
| `info` / `factory` | 状态 / 擦身份重绑 |

## 工程来源

`app_main.c` / GATT / sdkconfig 继承自 `printer_label_adv`（PR#21），仅改 PID、MAC、
NVS 命名空间、广播对象（清空为心跳）与设备名。
