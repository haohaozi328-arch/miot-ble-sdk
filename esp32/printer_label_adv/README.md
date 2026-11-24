# 米家标签打印机 可绑定固件 —— ESP32-C3

**可以直接编译烧录、被米家 App 绑定、设备页完整渲染的固件。**
2026-11-24 实测：米家「添加设备」发现「米家标签打印机」→ 绑定 `REGISTER COMPLETE 0x11`
→ 设备页完整渲染（素材广场/消息管理/我的标签/打印历史/按键标签）。

- 目标芯片：ESP32-C3（USB-Serial/JTAG）
- 模拟型号：米家标签打印机 `xiaomi.printer.label`，PID **0x34FE**（pdid 13502）
  - 官方产品库 `resolve_type=3` 广播可识别（米家自家）
  - spec：`urn:miot-spec-v2:device:printer:0000A060:xiaomi-label:1`（7 属性 + 3 action）
- 通道：MiBeacon V5 加密广播（4 帧轮播 state/paper/fault/battery → 网关）
  + 0xFE95 GATT（手机设备页，12 特征值）

## 属性与控制

| siid.piid | 名称 | 类型 | 实测 |
|---|---|---|---|
| 2.1 | state | u8 | ✅ 初值推送 + 串口 `state N` → properties_changed 2.1 |
| 2.2 | source-material | string | 走 on_get/on_set 兜底回调（App 下发打印内容入口） |
| 2.3 | copies | u8(1-99) | ✅ 初值推送 |
| 2.4 | paper-status | bool | ✅ 初值推送 |
| 2.5 | cover-status | bool | ✅ 初值推送 |
| 2.6 | fault | u8 | ✅ 串口 `fault N` → properties_changed 2.6 |
| 2.7 | time-set | u16 | ✅ 初值推送 |
| 3.1 | battery-level | u8(1-100) | ✅ 串口 `batt N` → properties_changed 3.1 |

action 2.1 start / 2.2 continue / 2.3 end：通过 `miot_demo_app_action_hook`
抓 App 打印动作，start→state=1、end→state=2 并回推。

## ★关于"BLE 传图片/打印内容"

标签打印机的**打印内容不走标准 spec 属性**：
- `source-material`(2.2) 是状态描述字符串，不是图片数据入口；
- 真实打印流程 = App 从云端 HTTP 拉素材 → 转成私有打印指令 → 走私有 BLE 通道下发，
  需要逆向 App 的打印指令生成逻辑（独立课题，本固件未覆盖）。

本工程已实现 string 属性的 on_get/on_set 兜底，若 App 通过标准 SET 2.2 下发内容，
固件会完整抓取并打印到串口（`App set 2.2 source-material len=N data=...`）。

## 编译 / 烧录

```bash
cd esp32/printer_label_adv
idf.py build
idf.py -p COM36 flash        # 不要 erase-flash（丢绑定身份）
```

## 米家添加

米家 → **+** → **添加设备** → 发现列表出现「米家标签打印机」→ 点卡片 →
`REGISTER COMPLETE 0x11` → 完成房间/命名。
实测 did `blt.3.1qv9bmrrk4400`，MAC `10:00:3B:CE:03:05`。

## 串口命令

| 命令 | 作用 |
|---|---|
| `state 0\|1\|2\|3` | 打印状态（待机/工作/完成/挂起） |
| `fault 0\|1\|2\|3` | 故障（正常/缺纸/未合盖/高温） |
| `batt N` | 电量 1-100 |
| `info` / `factory` | 状态 / 擦身份重绑 |

BOOT 键：短按切换 state；长按 5s 恢复出厂。

## 工程来源

GATT/sdkconfig 来自 `carlight_adv` 完整修复版；含 SDK 全量 I8/I16 编码 + BOOL SET
新格式解析修复（见 cooler_adv PR）。
