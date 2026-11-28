# ir_bus —— 红外下行 + BLE 总线（1 + N 参考实现）

> **先读 [DESIGN.md](DESIGN.md)**：三种架构（小爱直发 / ESP32 翻译 / 自定义学习）、
> 品类可行性（电视 · 机顶盒 · 风扇 · **空调** · 自定义红外）、组合选型清单、
> 能替代什么 WiFi 设备、替代不了什么。这份 README 只讲怎么跑。

把**小爱音箱的红外**当作下行信道（云 → 设备），把 **BLE** 当本地总线，
拆成「1 个 Hub + N 个节点」：Hub 只管听红外，节点只管干活。

```
米家语音 / 自动化
      │
      ▼
  小爱音箱（内置红外，40kHz）
      │  红外，单向，无回执
      ▼
  ┌─────────────┐
  │  HUB (×1)   │  只做：红外解码 → 去抖 → 总线下发
  └─────────────┘
      │  BLE GATT（本参考实现）
      ├──────────▶ NODE A（继电器 + 状态回报）
      └──────────▶ NODE N
```

## 为什么总线用 BLE 而不是 ESP-NOW

因为**手头只有一块 ESP32**，需要一个"不用第二块板就能验证"的对端：

- PC：`bleak`（Python 脚本已提供）
- 手机：nRF Connect

所以 Hub 和 Node **都做成 GATT 服务端（peripheral）**，各自单独烧录就能测。

> 生产环境若要让 Hub 主动推给 N 个节点，把 Hub 改成 central（扫描并连节点），
> 或换回 ESP-NOW 广播 —— **上层 payload 不用变**，`common/ble_bus.h` 里只定义语义。

## 目录

```
ir_bus/
├── common/ble_bus.h        # 总线协议：UUID + 指令字 + payload 结构
├── ir_lib/
│   ├── ir_codes.json       # 索尼 SIRC 电视码表（46 键，含中文名）
│   └── ir_protocol.md      # SIRC / NEC 协议要点 + 踩坑清单
├── hub/                    # "1"：红外接收 + BLE 下发
├── node/                   # "N"：BLE 接指令 + 继电器 + 状态回报
├── node_miot/              # "N"（上云版）：伪装门窗传感器3，状态经网关回米家云
├── tools/ble_peer.py       # PC 端对端模拟器（scan / listen / drive / sniff / watch）
├── tools/serial_node.py    # node_miot 串口控制台（发 on/off，读事件日志）
└── build.sh                # 编译/烧录（Docker + IDF v6.0）
```

## 两个 Node 版本的区别

| | `node/` | `node_miot/` |
|---|---|---|
| 目的 | 验证 BLE 总线本身 | **验证状态能不能上米家云** |
| 指令入口 | 自定义 GATT 服务 `…0002`（真无线） | 串口 CLI（先减少变量） |
| 状态出口 | 自定义 GATT `…0003` + 广播状态字节 | MiBeacon 加密广播 `0x480C` → 网关 → 云 |
| 米家 App | 不出现 | 显示成**门窗传感器3**（MCCGQ03HL） |
| 绑定 | 不需要 | 需要手机绑定一次（N 个节点 N 次） |


## 总线定义

| 项 | 值 |
|---|---|
| 服务 | `1a2b0001-9e3d-4c5f-8b7a-0d1e2f3a4b5c` |
| CMD | `1a2b0002-…` Hub→Node 写/通知，payload `[cmd, arg, seq]` |
| STATE | `1a2b0003-…` Node→Hub 读/通知，1 字节（0 断 / 1 通） |
| 设备名 | Hub = `IR-HUB`，Node = `IR-NODE` |

指令字直接沿用索尼码表的 cmd，日志和红外对照方便：

| 指令 | 值 | 语义 |
|---|---|---|
| `BUS_CMD_ON` | `0x12` (VOL+) | 开（置高，**绝对指令**） |
| `BUS_CMD_OFF` | `0x13` (VOL−) | 关（置低，**绝对指令**） |
| `BUS_CMD_OFF_ALT` | `0x2F` (PWR_OFF) | 关（兼容旧映射） |
| `BUS_CMD_TOGGLE` | `0x15` (POWER) | 翻转（和真电视电源键一致，仅调试） |

**为什么不用电源键当开关**：米家索尼虚拟遥控只有一颗电源键（toggle），
实测「打开电视」和「关闭电视」发出来**都是 `0x15`**，光靠它分不出开还是关。
而 VOL+ / VOL− 是两个独立键，正好当绝对指令：`0x12` 一定开、`0x13` 一定关。

**为什么不用翻转当正式语义**：红外单向无回执，ESP32 不知道真实设备状态，
翻转迟早漂移（手动关过之后，"开"会变成关）。所以正式场景用 ON/OFF。

## 硬件

| 器件 | 引脚 | 接到 |
|---|---|---|
| VS1838B OUT | — | Hub **GPIO3** |
| 继电器 IN | — | Node **GPIO4** |
| 两者 VCC/GND | — | 3V3 / GND（继电器模块按需 5V） |

## 分开测试（单板即可）

### A. 只测 Node（PC 全程自动，不需要红外）

```bash
cd esp32/ir_bus
./build.sh node flash COM6              # 烧 Node
python tools/ble_peer.py drive --name IR-NODE --cmd 0x15   # 开
python tools/ble_peer.py drive --name IR-NODE --cmd 0x2F   # 关
```

预期串口 `>>> RELAY ON (GPIO4=1)`，PC 侧 `<<< STATE relay=1`。

### B. 只测 Hub（需要红外源：小爱音箱或带红外的手机）

```bash
./build.sh hub flash COM6               # 烧 Hub
python tools/ble_peer.py listen --name IR-HUB
```

然后说"小爱同学，打开电视"，PC 侧应出现：

```
<<< RX cmd=0x15 arg=0 seq=3  = POWER/电源/开关
```

### C. 只测 Node 的「发送」侧（PC 纯接收，不需要红外）

Node 现在会主动发两种东西，PC 都能收：

```bash
python tools/ble_peer.py sniff --name IR-NODE --duration 10   # 只扫广播，不连接
python tools/ble_peer.py watch --name IR-NODE                 # 连上只订阅，不写
python tools/ble_peer.py watch --name IR-NODE --cmd 0x12 --after 4   # 订阅 + 4s 后补发一条
```

实测输出：

```
# sniff：广播里就带状态，全程没建立连接
[   0.4s] AD IR-NODE  10:00:3B:CE:4D:02  rssi= -43  状态=通(1)

# watch：ESP 自己每 3s 推一次，PC 一个字都没写
[   4.8s] <<< STATE relay=0  (首次)
[   7.8s] <<< STATE relay=0  (心跳)
>>> TX cmd=0x12  = VOL+/音量 +
[   6.1s] <<< STATE relay=1  (变化)
[   7.8s] <<< STATE relay=1  (心跳)
```

广播里的状态字节格式（manufacturer data，7 字节 AD 结构）：

| 字节 | 值 | 含义 |
|---|---|---|
| 0-1 | `FF FF` | company id `0xFFFF`（自定义/测试） |
| 2 | `0x01` | tag：ir_bus 节点状态 |
| 3 | `0x00`/`0x01` | 继电器断 / 通 |

> 意义：**不连接也能知道节点在不在、是通还是断**。N 个节点各自广播，
> 一个 PC/手机扫一圈就能收集全部状态，不用逐个连——这是后面做「状态总览」的基础。

### D. 手机当对端（nRF Connect，不用 PC）

**测 Node：**

1. 烧 Node，打开 nRF Connect → SCAN → 找到 `IR-NODE` → CONNECT
2. 展开服务 `1a2b0001-…`，找到 `…0003`（STATE）→ 点 **读** 图标，应显示 `0`/`1`
3. 找到 `…0002`（CMD）→ 点 **写** 图标 → 类型选 `Byte Array` → 填 `150001`
   （`0x15` 开 / `2F0001` 关 / `140001` 翻转）
4. 先点 STATE 右边的 **三个箭头（订阅）** 再写，就能看到值实时变化

**测 Hub：**

1. 烧 Hub，连 `IR-HUB`，对 CMD（`…0002`）点 **订阅**
2. 说"小爱同学，打开电视"，通知栏会冒出 `15 00 01`

### E. 合起来

Hub 与 Node 同时上电，Hub 需要**主动连** Node 才能下发 —— 本参考实现里 Hub 是
peripheral，所以合测阶段用 PC/手机当"中央"桥接，或按上面说的把 Hub 改成 central。

> 手机当桥：nRF Connect 连 `IR-HUB` 订阅 CMD，收到 `15 00 01` 后，断开、再连
> `IR-NODE` 写同样的三个字节。一条命令两头跑通，就是完整的 1→N 链路。

## 本参考实现做到哪一步

| 项 | 状态 |
|---|---|
| 小爱音箱 → VS1838B → SIRC12 解码 | ✅ 实测（`dev=0x01 cmd=0x15/0x12`） |
| 码表与 SIRCS 公开标准一致 | ✅ 三来源交叉核对 |
| 8 帧连发去抖 | ✅ 实测折叠成 1 次触发 |
| Hub：红外 `0x15` → BLE 通知 PC | ✅ 实测（`<<< RX cmd=0x15`） |
| Node：`0x15`/`0x2F`/`0x14` → 继电器 → 状态回报 | ✅ 单板 + PC 实测 3/3 稳定 |
| Node：未映射指令（如 `0x12` 音量+）忽略且不误动作 | ✅ 实测（串口 `未映射的指令`） |
| Node → PC 纯接收（订阅 + 3s 心跳） | ✅ 实测（`首次` / `心跳` / `变化` 三种标注） |
| Node 广播带状态，PC 不连接即可读 | ✅ 实测（`状态=通(1)`，rssi -43） |
| Node 事件日志（`RX`/`MAP`/`EXEC` 带毫秒时间戳） | ✅ 实测（0x12→relay=1、0x13→relay=0 两条全链留痕） |
| 状态回传米家云（伪装门磁3） | ✅ 实测（`node_miot`：绑定成功，串口 `on`/`off` → App 门态跟着变） |

两点范围说明，看代码前先知道：

- **本版没有让 Hub 自己扫描节点**：中间的"中央"由 PC / 手机充当（`tools/ble_peer.py`），
  一块板就能把两半分别验掉。要让 Hub 直接连 N 个节点，只需把 Hub 改成 central，业务侧不用动。
- **多节点编址靠红外键分配**，见下节"映射表与多节点编址"。当前 Node 是写死几条，因为只验了一个节点。

## 状态回传米家云：伪装选型

本地回报（BLE notify / 广播）只能让 PC 和手机看到。要让**米家云端**认这笔状态，
节点还得冒充一个云端已支持的广播型号，让青萍网关解密上云。

**选定：门窗传感器3（`xiaomi.magnet.03` / PID `0x8C05`）**

| 候选 | PID | 语义契合 | 仓库验证 | 结论 |
|---|---|---|---|---|
| **门窗传感器3** | `0x8C05` | 开/关 ↔ 继电器通/断，一对一 | ✅ 开、关、电量上云 | **选它** |
| 烟感卫士2 | `0x7ABA` | 正常/告警，偏"异常"语义 | ✅ 双向 | 适合报故障，不适合报通断 |
| 无线开关 | `0x1949` | 只有单击/双击/长按，无状态 | ✅ | 只能当触发，表达不了通断 |
| 水浸传感器 | `0x0863` | 漏水/正常，事件型 | ✅ | 语义错位 |

对象号（门窗3 走新版公式 `0x4000 | (siid<<10) | (piid-1000)`）：

| 属性 | spec | 对象号 | 载荷 |
|---|---|---|---|
| 开合 contact-state | 2.1012 | `0x480C` | `0` = 开（继电器通）、`1` = 关（继电器断） |
| 电量 battery-level | 3.1107 | `0x4C6B` | **枚举** `0`=Fully / `1`=Low，不是百分比 |

> 电量发百分比（如 `95`）会被网关**整帧丢弃**，云侧恒为 null —— 这是仓库里踩过的坑。

### 三条硬约束（必须先认清）

1. **广播和 GATT 连接互斥**：NimBLE 一旦被连上，广播就停。所以 Hub 连节点执行完
   要尽快断开，节点恢复 4s 轮播，网关才有机会收到新状态。这也意味着**状态上报有延迟**，
   不是实时的。
2. **每个节点要绑一次**：手机连一次完成 ECDH 绑定，拿到 bindkey 之后才能发加密广播。
   这是 N 个节点里省不掉的 N 次操作。
3. **App 里会显示成"门窗传感器"**：借用语义的代价，用户看到的是"门开了/门关了"，
   实际是继电器通/断。

### `node_miot` 实测（2026-11-28）

工程 = `door3_adv` 已验证的绑定/广播骨架 + 继电器执行层。为了把变量降到最少，
**这一版没有加自定义 BLE 服务，指令源是串口**（`on` / `off` / `toggle`）。

改动只有四处：
- `NVS_NS` 改 `nodemiot`、派生 MAC 改 `0x70:0x08`（避免和 door3 / node 撞）
- 新增继电器 GPIO4（`RELAY_ACTIVE_LOW` 可切换高低电平触发）
- `trigger_door()` → `relay_set()`：继电器状态直接决定 `contact-state`
- 事件日志统一 `[ 毫秒ms] RX/EXEC/ADV/EVT` 格式

状态映射：

| 继电器 | `0x480C` | App 显示 |
|---|---|---|
| 通电 `1` | `0` | 门已打开 |
| 断电 `0` | `1` | 门已关闭 |

实测日志（板子串口）：

```
[143122ms] INFO pid=0x8C05 mac=10:00:3B:CE:70:0A bound=1 relay=0 door=1 batt=0
[147033ms] ADV idx=0 obj=0x480C val=1 len=16B          ← 4s 轮播已加密帧
[151033ms] ADV idx=1 obj=0x4C6B val=0 len=16B

> 串口发 on
[157922ms] RX cmd=ON
[157922ms] EXEC relay=1 door=开门 (0x480C=0)
[157923ms] ADV idx=2 ev_left=3 obj=0x480C val=0 len=16B
[157932ms] EVT 补发 relay=1 剩 2 次
[157933ms] ADV idx=2 ev_left=2 obj=0x480C val=0 len=16B
[158332ms] EVT 补发 relay=1 剩 1 次
[158333ms] ADV idx=2 ev_left=1 obj=0x480C val=0 len=16B
[158733ms] EVT 补发 relay=1 剩 0 次

> 串口发 off
[202722ms] RX cmd=OFF
[202722ms] EXEC relay=0 door=关门 (0x480C=1)
[202723ms] ADV idx=0 ev_left=3 obj=0x480C val=1 len=16B   ← 命中同一套连发逻辑
```

**结论**：门磁3 的上云链路可复用，继电器通断能映射成开合状态并让 App 跟着变。
`on`/`off` 各连发 3 帧（400ms 间隔）覆盖丢帧，之后回到 4s 交替轮播。

串口命令：

| 命令 | 作用 |
|---|---|
| `on` / `off` / `toggle` | 继电器动作 + 发 `0x480C` 事件帧 |
| `info` | PID / MAC / bound / relay / door / batt |
| `obj 480C` / `batt 4C6B` | 临时改对象号（排查用） |
| `bval 0` | 电量枚举值（0=Fully 1=Low，**不能填百分比**） |
| `factory` | 擦身份重启，可重新添加 |

PC 侧工具：`python tools/serial_node.py on --listen 12`
（内部把 DTR/RTS 置 False，避免一开串口就把 C3 复位）

指令源也可以从串口换成 BLE：在 `node_miot` 上叠加 `common/ble_bus.h` 的 CMD 服务，
Hub 就能直接推指令，同时保留门磁上云回传。本版刻意留在串口，是为了把"上云"这一个
变量单独验干净。


## 映射表与多节点编址（思路）

Node 现在是**写死几条**：`0x12` 开、`0x13` 关、`0x2F` 关（兼容）、`0x15` 翻转，其余一律忽略。
多节点时每个节点需要一张**自己的映射表**，否则所有节点会同时动作。

一种做法（不改 payload，只改 Node 侧）：

| 节点 | 认哪几个红外键 | 用途 |
|---|---|---|
| Node A（风扇） | `0x12` / `0x13`（VOL+ / VOL−） | 音量键 = 风扇开关 |
| Node B（灯） | `0x10` / `0x11`（CH+ / CH−） | 频道键 = 开关灯 |
| Node C（插座） | `0x10*` 之外的冷门品牌键 | 换品牌叠加码空间 |

索尼码表 46 个键 → 一个 Hub 就能单独寻址 **46 个节点**，
再加一个冷门品牌（如"大金空调"）的码又是 40+ 条，空间完全够。

## 坑

1. **ESP32-C3 的 USB-JTAG 控制台在芯片复位时会重新枚举**，串口监听必须自动重连，否则一按复位键脚本就退出。
2. **去重指纹要用解码后的 dev/cmd**，不能用原始微秒——抖动 ±60µs 会把同一帧算成不同帧。
3. **B 频道和红外互不干扰**，但 BLE 连接期间广播会停；如果节点以后要同时跑 MiBeacon 上报，别在同一时刻做长连接。
4. SIRC 载波 40kHz，VS1838B 是 38kHz：近距离没问题，远了丢帧，换 40kHz 头或加导光。
5. **bleak 订阅 STATE 后不能立刻写 CMD**：CCCD 在主机侧生效有延迟，会漏掉第一条 notify。
   脚本里已加 `sleep(0.3)`；用 nRF Connect 手动测时同理，先订阅、等一秒再写。
6. **诊断要看板子串口**：PC 侧"没收到通知"不代表设备没发，`[node] RX` / `>>> RELAY`
   行在 UART 上才是权威（调试时靠它排掉过一次"PC 没收到"的假故障）。
7. **用 pyserial 打开 COM6 会复位板子**（默认拉 DTR/RTS，ESP32-C3 的 USB-JTAG 会当成
   复位时序）。所以"抓串口日志"和"测继电器状态"不能同时做——一开串口，relay 就回 0、
   广播也重发一遍。测状态请用 `sniff`/`watch`，别开串口。
