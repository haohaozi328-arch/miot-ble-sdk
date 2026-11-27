# 08 · 真机实测：米家 Mesh 1.0 灯泡（0x4707）配网全过程

> 日期：2026-11-26　设备：`yyzn.light.wy0a04`（PID `0x4707` / 18183）
> 目的：拿一颗**带合法证书链的成品 Mesh 1.0 灯**，把「未配网 → 网关配网 → 入网」这段
> 在本仓库一直被写成"硬墙"的流程，换成**自己实测出来的时序**。

## 1. 设备与接线

| 项 | 值 |
|---|---|
| Model | `yyzn.light.wy0a04` |
| PID | `0x4707`（18183） |
| MAC | `8C:D0:B2:69:96:35` |
| DID（出厂） | `1099933978` |
| 固件 | `Version:2.1.2_0002`，build 戳 `726422_2L2_231215` |
| 串口 | 模组 UART，115200 8N1，3.3V 电平（只接了 3V3 / TX / RX / GND） |
| 焊盘 | `3V3 / GND / TX / RX / SWS / C / W / AC` —— **无 RESET、无第二组 UART** |
| 通道 | `C` / `W` = 冷白 / 暖白两路 LED 驱动（双色温灯），`AC` = 市电输入 |
| 调试 | 只有 `SWS`（Realtek 专有单线调试，标准 J-Link/ST-Link 不支持） |
| 配网网关 | 小爱音箱 Play 增强版（「蓝牙 Mesh 网关」已开启） |

固件启动自报还会打印 `Magic:55cc55cc` 与 `PA4[R] / PC3[B] / PB4[G] / Dri A`——
**这颗模组自己驱动 RGB 三路**，也就是说"灯逻辑在模组里"，不存在外挂 MCU 跑业务的情况。

## 2. 串口时序：它只在启动初期开 MCU 协议口

上电后的完整日志（`[t]` 为模组自己的毫秒时钟，单位秒）：

```
[ 0.0] Reset by Power
[ 0.0] start
[ 0.0] miio_system_restore
[ 0.0] Firmware for app
[ 0.0] DID:1099933978 / MAC / PID:18183 / Model / Version:2.1.2_0002
[ 0.0] Magic:55cc55cc
[ 0.0] PA4[R] 1 / PC3[B] 1 / PB4[G] 1 / Dri A
[ 1.0] real pid:18183, firmware pid: 18183
[ 1.0] System Init Succ
[ 2.0] 726422_2L2_231215
[ 2.0] [user_state_process:335] [user_state_process:337]
[ 5.0] wd feed succ 0 / Heap left:1648      ← 之后每 5s 一次
[12.0] close uart                            ← 关掉 MCU 串口
```

两个实测结论：

1. **`close uart` 是真的关**。窗口关掉之后再发 `model` / `mcu_version` / `get_down`
   一律没有回应，串口上只剩 `wd feed` / `Heap left` 心跳。
2. **窗口长度不固定**：同一天两次上电分别观察到 `12s` 和 `31s` 关闭，说明它是
   "启动后等 MCU 活动一段时间、没有合法握手就关"，不是固定超时。

> 这段窗口内按《模组串口指令手册》发 `model yyzn.light.wy0a04` + `mcu_version 0001`
> 也**没有** `ok` 回应（脚本 `tools/mesh/handshake_on_boot.py` 卡着
> `System Init Succ` 抢窗口试过多次）。结论：**这版灯固件不实现 MCU ASCII 协议**，
> 通信只走 BLE（MiBeacon + Mesh），UART 纯日志口。

## 3. 广播：未配网 → 配网窗口 → 配网完成

抓取工具：`tools/mesh/watch_bulb.py 8C:D0:B2:69:96:35`（PC 自带蓝牙，服务数据 `0xFE95`）。

| 阶段 | 帧头 | payload | 解读 |
|---|---|---|---|
| 未配网（等添加） | `fc=B054` pid=`0x4707` ver=5 auth=1 **reg=0** **mesh=1** | `00 35 96 69 B2 D0 8C 08 02 00` | MAC 小端在 payload 里；`mesh=1` 自报 Mesh 设备 |
| App 开始配网的瞬间 | 同上 | `… 08 0E 00` | 尾部状态字节 `0x02 → 0x0E`（bit1 → bit1+2+3），配网窗口打开 |
| 配网完成后 | 同上 | `… 08 0E 00` | **帧头完全不变，`reg` 仍为 0** |

⚠️ 一个和笔记里老结论冲突的点：**`reg` 位（fc_hi bit0）在这颗 mesh 灯上从头到尾都是 0**，
配网成功后也没有变 1。所以**不能用 `reg` 位判断 mesh 设备是否已配网**；目前可观测的
"配网进行中"信号是 payload 尾部那个字节（`0x02` → `0x0E`）。

## 4. 配网瞬间的模组日志

配网成功那一刻，串口（同一条日志口）打出：

```
[200.0] [user_state_process:341]
[203.0] [user_state_process:344] [user_state_process:367]
[203.0] 726422_2L2_231215
[203.0] [user_state_process:367]
[204.0] [user_state_process:420]MIBLE_USER_GAP_CB_DISCONNECTED
[210.0] Heap left:1584        ← 从常驻 1648 掉 64B，配网确实分配了网络/密钥状态
```

`MIBLE_USER_GAP_CB_DISCONNECTED` 出现在配网结束的同一秒 —— 与 §07 里
"配网走 GATT 连接、结束后立刻断开"的观察一致。`user_state_process` 的 341/344/367/420
四个行号就是配网状态机的迁移点，可作为下次抓别的 mesh 型号时的对照锚点。

## 5. 复现步骤

1. 模组接 3.3V（**独立供电，别用 USB-TTL 的 3V3，带不动会 1 秒一次复位**）+ GND + TX/RX
2. `python tools/serial_tail.py COM5 300 logs/mesh10_boot.log` 挂日志
3. `python tools/mesh/watch_bulb.py <MAC> 300 logs/mesh10_adv.log` 挂广播
4. 米家 → `+` → 附近设备，完成配网
5. 对照 §3 表格：`08 02 00` → `08 0E 00`，以及 §4 的 `user_state_process` 四行

## 6. 这次实测拿到了什么 / 还缺什么

**拿到**：

- 一颗 **Mesh 1.0 成品灯可以正常被米家配网**（带证书的模组，App 侧一路绿灯），
  证明"Mesh 设备不需要自造证书链"这条正路是通的，只是**证书在模组里**。
- 未配网广播的**真实帧形**：`FE95 / pid 0x4707 / ver5 / auth1 / mesh=1 / reg=0 /
  payload 尾部 08 02 00`，可以直接拿去对照 `esp32/meshbulb` 自造的广播。
- 配网窗口的**可观察信号**（`08 02`→`08 0E`）与**模组日志锚点**（341/344/367/420）。

**还缺**：

- 配网报文本身（App ↔ 网关 ↔ 设备的 PB-ADV/PB-GATT 帧）没有抓到 —— 需要手机侧
  HCI snoop（见 `docs/mesh-capture-plan.md`）或嗅探硬件。
- 控制链路（App 开灯 → 网关 → mesh 广播 → 设备）在本地不可观测；模组 UART 不给 `down`。
- 这颗模组的调试与固件提取：焊盘只有 `3V3/GND/TX/RX/SWS/C/W/AC`。UART 是纯日志口
  （应用协议里没有读 flash 的指令，且启动后 `close uart`），要读固件只能走 `SWS`——
  Realtek 专有单线调试，需官方 SWS 调试器/MP Tool，标准 J-Link 不吃；且 RTL8762 的
  flash 读保护状态未知，误操作会触发擦除、证书链永久丢失（变 `-40` 废片）。
  **结论：工具门槛 + 废片风险 vs. 一份不可复用的私钥，投入产出不成立，不做。**
- 供电注意：`AC` 与 `3V3` **不可同时接**（板载 AC-DC 输出会被顶）；实验时只用 3V3 供电。

