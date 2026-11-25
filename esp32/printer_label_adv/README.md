# 米家标签打印机 可绑定固件 —— ESP32-C3

**可以直接编译烧录、被米家 App 绑定、设备页完整渲染、真实打印任务端到端接收的固件。**
2026-11-24 实测：米家「添加设备」发现「米家标签打印机」→ 绑定 `REGISTER COMPLETE 0x11`
→ 设备页完整渲染（素材广场/消息管理/我的标签/打印历史/按键标签）。
2026-11-25 实测：米家 App 编辑标签点打印 → **私有打印通道全链路打通**，
4764B 位图（96 点宽 × 397 行）加密下发、固件解密还原出用户手绘内容，App 显示「传输成功」。

- 目标芯片：ESP32-C3（USB-Serial/JTAG）
- 模拟型号：米家标签打印机 `xiaomi.printer.label`，PID **0x34FE**（pdid 13502）
  - 官方产品库 `resolve_type=3` 广播可识别（米家自家）
  - spec：`urn:miot-spec-v2:device:printer:0000A060:xiaomi-label:1`（7 属性 + 3 action）
- 通道：MiBeacon V5 加密广播（4 帧轮播 state/paper/fault/battery → 网关）
  + 0xFE95 GATT（手机设备页，14 特征值，含打印通道 001F/0020）

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

## ★私有打印通道（已打通，2026-11-25 实测）

标签打印机的**打印内容不走标准 spec 属性**，`source-material`(2.2) 只是状态描述字符串。
真实打印走 0xFE95 服务下的私有 GATT 通道 + 私有二进制协议，协议栈如下：

- 通道：`001F` Write-NoRsp（App→设备）+ `0020` Notify（设备→App）
- 外层帧：`A3 20 <len LE16> <AES-128-CBC 密文> <CRC32 LE>`
  - AES-128-CBC：KEY `99B829436CDD5647AADB8816F73E8644`，IV `0001020F3CF899ABABCD25318DF446B1`，零填充
  - CRC32：zlib 多项式表算法，初值 `0x76953521`（错则被设备静默丢弃）
- 内层命令：`11 <grp> <cmd> <len LE16> <body>`
  - grp01 控制：`1E` 连接握手 / `0E` / `13` 状态 / `19` 校时
  - grp05 打印：`0B` start（宽/长）→ `0D` 位图块（idx,total,≤1800B/块）→ `0C` 提交（more_pages）
  - 提交后设备回 `21 05 0C 00 00`（准确语义是「提交已应答」，artrix 实机证据；
    App 另需状态通知里的 flags bit7 翻转判断打印生命周期）
- 心跳：设备→App `A3 00` 明文帧 + TLV（tag1 buffer LE32、tag2 flags、tag3 电量）
  - tag1 buffer = 打印队列空闲 RAM，插件流控阈值 >500 才继续发下一块
  - tag2 flagsA：bit0 盖开 / bit1 缺纸 / bit2 卡纸 / bit3 过热 / **bit7 正在打印**
  - tag2 flagsB：bit0 刀具 / bit1 字库 / bit2 PSRAM / bit3 纸校准 / bit4-5 关机 / bit6 电池过热

**打印状态机（2026-11-25 二版，对齐 artrix 实机时序）**：

```
空闲(bit7=0, buffer 16384)
  → 05/0B    bit7=1「打印中」，buffer 满格
  → 05/0D ×n buffer 按块占用递减（模拟队列消化，保持 >500）
  → 05/0C II=1  多页：保持 bit7=1，等下一轮 05/0B
  → 05/0C II=0  ACK 后保持 bit7=1 一小段（出纸延时，按行数 2~8s）
  → 到点     bit7=0 回空闲，buffer 复位  ← App 这才算「打印完成」
```

断连即复位状态机（flags/buffer/定时器全清）。

实测会话时序：App 订阅 5 个 notify → 001F 握手（1E/0E/13/19）→ 05/0B →
3× 05/0D（1800+1800+1164B）→ 05/0C → 固件解密出完整位图并串口输出 ASCII 预览。
一版实测：App 显示「传输成功」；约 12s 后 App 重发同任务，推测就是在等
bit7 翻转的状态通知（二版已补）。

实现见 `main/print_channel.c`（组帧/解密/分发）与 `main/gatt_miot.c`（GATT 表 14 特征值）。

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
