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

## ★关于传图

照片打印的**图片数据不走标准 spec 属性**。ricott 的官方 spec 只有 device-information
（5 个只读 string）+ indicator-light / battery 两个空服务，**没有任何可上报的业务属性**，
打印内容走 App 私有 BLE 通道下发（与标签打印机的 L2CAP PSM 0x80 同类，私有协议另行逆向）。

本工程应对方式：绑定后只发**空对象心跳帧**维持在线，不挂标签打印机的
state/paper/fault/battery 对象号（ricott spec 里不存在，App 会整帧丢弃判异常）。

设备页点「连接」后仍停在「连接中...」——私有 RN 打印插件的连接触发条件尚未逆向出来，
属独立课题，本固件未覆盖。

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
