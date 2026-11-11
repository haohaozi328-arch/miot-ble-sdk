# 夜灯2（MUE4094RT / PID 0x03DD）接入交接单

> 截止 2026-11-10 23:20。状态：绑定链路全绿，详情页登录卡在 ag2 device info 之后。

## 一、一句话现状

设备能被米家识别、能完成绑定、能满足 ag2 建链（channel READY），但**已绑定状态下打开详情页仍然超时**：
App 收到 device info 后完全沉默，840ms 后 1 秒断开，从不发标准认证命令 `A2` / `24`。

## 二、已确认的硬结论（不要再重试）

### 1. 绑定链路（legacy p21）已完全跑通
```
AUTH_INIT(90 CA 85 DE) → 写 12B token → ACK → BOND → 写 20B DeviceID → GETRAND → LOGIN 校验通过
```
- token 与 DID 都已落入 NVS（`miot_auth_set_did()` + `port.save()`）。
- 当前绑定态：`bound=1`，token `827F925F...`，DID 20B 有效，MAC `10:00:3B:CE:40:07`。

### 2. legacy 登录路径 = 死路（8 种应答全试完）
GETRAND 之后无论回什么，App 都不写 LOGIN，固定 1 秒断开（0x213）：
| 尝试 | 结果 |
|---|---|
| RC4(token, rand) 单帧 @0x0001 | 无 LOGIN |
| bind_key 16B / bind_key[4..] | 无 LOGIN |
| 明文 rand | 无 LOGIN |
| HKDF("mible-login-info") 派生 | 无 LOGIN |
| token/rand 字节序反转、双重 RC4 | 无 LOGIN |
| 完全静默 | 无 LOGIN |

**结论**：legacy 只服务「添加设备」的绑定流程。已绑定状态下 App 写 GETRAND 只是探测，
真正的登录走 ag2 新通道。不要再在 GETRAND 应答上耗时间。

### 3. ag2 建链已打通到 `channel READY`
```
设备主动 mng       00 00 04 00 06 12   （订阅 001A/001B 时发，spec 2 探针）
App 回 mng_ack     00 00 05 00 06 12   ×2
设备回测试包       00 00 05 01 + 16×0x12
App 回执同款 20B   → channel READY
```
- 应答面必须走「控制帧写进来的那条特征」（`s_ag_h`），固定发 001A 会投错信箱。

### 4. device info 三个坑（已全部修掉）
| 坑 | 现象 | 修法 |
|---|---|---|
| 身份未落盘 | `did=0B`（0 字节），device info 空身份 | `miot_auth_set_did()` 转存 + NVS |
| 超出 MTU | `notify len=28 > mtu-3=20, dropped` | 改走 `ag_parcel` 分帧 |
| 发送节奏 | START + 数据帧连推，App 未就绪全丢 | `miot_auth_poll` 驱动，80ms/帧 |

修复后 device info 完整送达：`did=20B, 24B payload`，START + 20B 帧 + 8B 帧全 `rc=0`。

## 三、当前卡点

device info 已完好送达，App **没有任何应答**：
- 不回 code=1 回执
- 不发 `A2`（建链）/ `24`（登录）
- 840ms 后主动 `cancelOpen()` → 1s 超时断开

可能原因（未验证，按性价比排序）：
1. **device info 的头部字节不对**：当前是 `02 00 00 00` + did。参考工程与真机可能有
   `ver / caps / 长度` 的不同布局，App 解析后判定为非法设备。
2. **App 期望设备先发 CMD_LOGIN(0x24)** 而不是 device info。
3. **广播 FC 的 authMode** 把 App 引到了另一条认证分支（当前 `0x59` = v5|registered|authMode=2）。

## 四、明天第一件事

**抓手机侧 logcat 看 App 对 device info 报了什么错。** 这是唯一能一击定位的动作，
比继续盲试帧格式便宜得多。命令见下。

## 五、环境与命令备忘

```bash
# 编译（工作目录挂载点必须是 /project）
MSYS_NO_PATHCONV=1 docker run --rm -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/multi_dev espressif/idf:v6.0 idf.py build

# 烧录（先 kill 占用 COM6 的 python）
cd esp32/multi_dev/build && python -m esptool --chip esp32c3 -p COM6 -b 460800 \
  --before default-reset --after hard-reset write-flash --flash-mode dio \
  --flash-size 4MB --flash-freq 80m "@flash_args"

# 串口抓取（必须 run_in_background，用 (cmd &) 会被回收假死）
python -u tools/serial_tap.py COM6 _nightlX.log _nightl_cmd.txt

# 手机日志
adb logcat -c && adb logcat -v time > _lc.txt

# 串口命令写入（追加到 cmd 文件即执行）
printf 'spec 2\ninfo\n' > _nightl_cmd.txt
```

### 关键串口命令
| 命令 | 作用 |
|---|---|
| `info` | 打印 bound / token / DID / MAC |
| `spec 2` | 开启 ag2 主动建链探针（订阅即发 mng） |
| `spec 0` | 关闭探针，回退 legacy |
| `strat N` | GETRAND 应答策略（0=单帧 token，8/9/10=多算法探测） |
| `gr 0` | GETRAND 完全静默 |
| `push 1\|0` | 订阅 0x0001 后主动推 v5 数据帧 |
| `advu 1\|0` | 强制对外广播为「待绑定」形态（保留 token/DID） |
| `frames` | 打印当前广播帧十六进制 |
| `fc XX` | 改广播帧 FC 高字节（22/50/58/59） |

## 六、已知工程坑（血泪）

1. **ninja 不重编**：源码改了但 mtime 未变时 bin 停在旧版。改完必须
   `touch <file>` 或直接看 `multi_dev.bin` 时间戳确认。
2. **后台任务**：串口 tap 和 adb logcat 必须用 `run_in_background` 启动；
   用 `(cmd &)` 会在工具调用结束时被回收，表现为"日志不再增长"的假死。
3. **烧录前必须 kill python**（占用 COM6），否则 `could not open port`。
4. **MTU 全程 23**，ATT 单包上限 20 字节，任何 notify 超过会被静默丢弃（只留一行日志）。
5. **设备卡死**（主机不响应、广播还在发）：esptool 两种复位都无效时，只能拔插 USB。

## 七、关键代码位置

| 文件 | 职责 |
|---|---|
| `esp32/multi_dev/main/gatt_miot.c` | GATT 表（9 特征）、写入分派、legacy p21、spec 探针、GETRAND 应答 |
| `src/miot_auth.c` | ag2 状态机、`ag_parcel` 分帧、`send_device_info`、HKDF 派生 |
| `esp32/multi_dev/main/app_main.c` | 串口命令解析、广播帧构造 |
| `include/miot_ble/miot_auth.h` | 认证层结构体与 API |

---

# 追加（2026-11-10 23:15-23:55 夜间排查）

## 一、logcat 这条路走不通（已结论化）
- 米家主动写了完整本地日志，但**加密**：`/sdcard/Android/data/com.xiaomi.smarthome/files/logs/app/mijia_log_v3_YYYY-MM-DD`，
  文件头魔数 `q3>`，非明文、非 zstd，无密钥无法解。
- logcat 侧 App 进程（com.xiaomi.smarthome）只有 163 行 `BluetoothGatt` 系统调用日志，
  **没有任何业务层输出**；`plugin0/plugin1` 进程同样近乎静默。
- 结论：想从手机侧拿到「App 为什么拒绝」不可能，除非能解 App 私有日志格式。
  以后不要再在这上面花时间，改用「设备侧 A/B 对照 + 参考工程反编译结论」定位。

## 二、这轮修掉的真实 bug（都有日志佐证）

### bug1：parcel 被拆到两条通道上（关键）
现象：START 发到 `0x001B`，数据帧却发到 `0x001A`，App 连 ACK 都不回。
根因：`s_ag_h` 每收到一次写入就改一次，而 App 会**同时往 001A/001B 写重复的 mng_ack**。
修复：控制帧仍回「来路面」；但 **parcel（START `00 00 00` / SINGLE `00 00 02` / 数据帧）
一律走 `0x001A`** —— docs/protocol.md §2 实测：App 只在 0x001A 收 notify，001B 的会被当 IDLE 推送丢弃。

### bug2：device info 与测试包同毫秒发出
修复：`miot_auth_poll` 延迟 150ms 再发（`info_at_ms` 字段，1=pending）。

### bug3：GETRAND 回包投错面 + 多帧污染
现象：App 把 GETRAND 写在 `0x0010`，却订阅 `0x0001` 等回包；我们按「来路面」投到 0010，
或投 4 帧重复。
修复：单帧 + 在**已订阅的面**里选（来路面 → 0x0001 → 0x0010），并加「等订阅」门（50ms 轮询，
最多 1.2s）。修完确认：`auth 回包 #1 h=16 rc=0`，投递成功。

## 三、新的负面结论（都已排除）
| 假设 | 做法 | 结果 |
|---|---|---|
| App 会 read 特征取结果 | 查手机侧 `onReadCharacteristic` | **全程 0 次读**，假设作废 |
| 单帧 RC4(token,rand) 到正确通道 | 修完投递后实测 | 仍不写 LOGIN |
| 明文随机数单帧 | `strat 10` + `gr 1` | 仍不写 LOGIN |
| 无 ag2 干扰的纯 legacy | `spec 0` | App 写 GETRAND 后 1s 自行退订、断开 |

## 四、目前对 App 行为的完整画像（设备侧观测）
```
connect → subscribe 001B → subscribe 001A → subscribe 0001(attr16) → [subscribe 0010]
   ↓
写 mng_ack  00 00 05 00 06 12      （001B / 001A，可能各两次）
写测试回显  00 00 05 01 + 16×0x12  （20B）
写 GETRAND  00 BC 43 CD            （0x0010，不一定每轮都有）
   ↓ 约 1000ms 后
退订全部 → disconnect reason=0x213（Remote User Terminated）
```
**App 有严格的 ~1s 判定窗口**（页面级约 5s 重连周期）。device info 在窗口内送达也没用。

## 五、下一轮的两个方向（按性价比）
1. **反编译定位**：`/d/workspace/temp/_apk_dex/` 有 classes10 全量 smali 转储（`d10_full.txt` 239MB）
   和 `..._bluetooth_channel_OooO0O0.txt`。目标：找到「收 mng_ack(code=1) 之后的 READY 回调」
   到底还检查了什么（很可能检查 `dmtu`、`pkg` 版本或 beacon 里的 capability/authMode）。
   → 与此并行可做的低成本实验：改 `fc`（22/50/58/59）配合当前已修好的 ag2，看 authMode
   变化是否让 App 走标准认证。
2. **回到绑定链路交付**：保持「待绑定」广播（`advu 1`），每次通过「添加设备」进入（这条链路
   已反复跑通），先让设备可用，再继续磨详情页。

## 六、当前设备状态与固件
- 固件版本：`multi_dev.bin` 2026-11-10 23:37（含上述三个修复）
- `bound=1`、`token=827F925F...`、DID 20B 已持久化
- 探针状态：`spec 0`、`strat 10`、`gr 1`（**注意：这不是默认值，下次实验前先 `strat 0` + `gr 18`**）

---

# 追加（2026-11-11 07:50-17:10）真机行为对齐 + 手机侧确定证据

## 一、本轮修掉的 5 个真问题（均已实测确认）

### 1. 连接期间停播 → 改为持续广播（真机行为）
`app_main.c` CONNECT 分支原来 `ble_gap_adv_stop()`。参考工程实测结论：
已绑定设备在连接态必须继续发 beacon，App 的扫描管线靠它判断「设备在身边」。
- NimBLE 细节：`MAX_CONNECTIONS=1` 时对 **connectable** adv 直接返回 ENOMEM
  （`ble_gap_ext_adv_params_validate: connectable && !ble_hs_conn_can_alloc()`）。
  所以连接态必须用 `BLE_GAP_CONN_MODE_NON`（真机本来也只发非连接 beacon）。
- 断开时必须 stop+start 才能换回可连接形态，否则 `adv_start` 因「已在广播」直接 return，
  手机再也连不上。

### 2. 已绑定广播间隔 800(500ms) → 160(100ms)
参考工程真机恒定 160。App 判定窗口只有约 1s，500ms 间隔下一帧都收不到。

### 3. 设备主动发起 MTU 交换 → MTU 23 变 **247**
`ble_gattc_exchange_mtu()`（头文件是 `host/ble_gatt.h`，不是 ble_gattc.h）。
Android App 从不自己 requestMtu。实测 `mtu -> 247`，手机侧同样记录 247。

### 4. GATT 表对齐真机（关键）
之前为「压缩发现耗时」删掉了 0x0016/0x0017/0x0018，又给 0x0002/0x0004/0x0014
加了实验性 NOTIFY。**恢复真机属性后，App 的行为立刻变了：从「订阅完就断、一条命令都不写」
变成「主动往 0x0010 写 GETRAND」**。真机属性表（参考工程镜像）：
```
0001 W|WNR|N      0002 R          0004 R          0010 W|WNR|N
0013 R|W|WNR      0014 R          0016/17/18 R|W|WNR|N
0019 W|WNR|N      001A W|WNR|N    001B W|WNR|N
```
当前句柄：0001=16 0002=19 0004=21 0010=23 0013=26 0014=28 0019=39 001A=42 001B=45
（真机 token=16 / 0019=38 / 001A=41 / 001B=44，差 1 个句柄，不影响发现）

### 5. ag2 分片改为「START 后立即连发」
参考工程 `ag_parcel19` 就是 START + 全部数据帧一次性推（64B 公钥、整条证书链都这么发，
真机验证）。我们之前加了 80ms/帧的节流 + 150ms 延迟，导致 device info **永远赶不上**
App 的判定窗口。已全部去掉。

## 二、手机侧拿到的硬证据（本轮最重要）

### App 的连接生命周期（logcat -s BluetoothGatt,BtGatt.GattService）
```
onConnected
+797ms  onSearchComplete (status=0)        服务发现成功
+40ms   setCharacteristicNotification 001B
+65ms   setCharacteristicNotification 001A
+90ms   setCharacteristicNotification 0001
+65ms   cancelOpen()  ← 订阅完 0x0001 后 65~130ms 直接放弃
        refresh() + close()
```
**App 从 connect 到 cancelOpen 固定约 990ms**，而且订阅完 0x0001 后只等 65~130ms。
这不是「等超时」，是**判定式放弃**。真机同一动作能连 97s+，说明某个前置条件没满足。

另外 50% 的重连会拿到 **status=133**（GATT_ERROR），交替出现。

### device info 确实送达了，但 App 不理
- 设备侧：`ag tx START pkg=0 cnt=2` + 20B + 8B，rc 全 0
- 手机侧：`onNotify handle=42 length=6 / length=20 / length=8` 全部出现
- 结果：**App 65ms 后 cancelOpen，不回 0x15、不写 A2**
→ 印证参考工程那句警告：「已配对设备由 App 自己写 a2 请求 device info，
  **不要抢跑**，否则它在 a2 之前收到 device info 会被丢弃」。
  App 的通道状态机不在 WAIT_INFO 态，主动发的 pkg0 一律丢弃。

### GETRAND 算法用真机数据做过逐字节校验（结论：我们是对的）
真机串口日志给出的三元组：
```
token(human) = E18CBBE3972D330DAB504821
rand         = AF2DD050
resp         = F307AB5C   ← 真机实际发出
```
用 RC4(token_human, rand) 计算 = **F307AB5C**，完全吻合；用 air 序算 = 93074632，不符。
→ GETRAND 回包 = `RC4(token, rand)`，key 用 **human 序**。
本机 token = `721D3EC2AE7287881E0E3D11`，rand=CDDC47E4 → 固件输出 D88C07D8，
Python 独立计算一致。算法、字节序、通道（0x0001）都对。

## 三、当前卡点（已排除大量假设）

| 假设 | 结论 |
|---|---|
| GETRAND 回包算法/字节序错 | 用真机数据验证**正确** |
| 回包投错通道 | 投 0x0001，与真机一致 |
| 回包只发 1 帧不够 | 改 6 帧无效 |
| App 需要读特征 | 手机侧全程 **0 次 read** |
| MTU 23 限制 | 已协商 247，无效 |
| 连接态停播导致判离线 | 已持续广播，无效 |
| device info 格式/分片 | 已确认送达 App，仍被丢弃 |
| 广播 authMode | 0x59(已绑定,v5,authMode=2) 为真机形态 |

**剩下的唯一解释**：App 拒绝在「未经过标准认证(0x0019/ECDH)」的会话上做登录，
且它不会主动写 A2 —— 说明触发 A2 的条件还没满足（很可能与云端 isOnline
或 beacon 的 bindkey 加密态有关）。继续推进需要完整实现 ECDH + k61 会话密钥，
属于整块新工程。

## 四、当前设备状态
- 固件：2026-11-11 16:5x（含本轮全部 5 项修复）
- `bound=1`，token=`721D3EC2AE7287881E0E3D11`，bkey=`2EA269E4...`，DID 20B 已持久化
- 探针：`spec 2`（ag2 mng 建链开）、`push 0`、`strat 0`、`gr 6`
- 串口 COM6 / adb 均可用；抓取脚本：serial_tap.py + `adb logcat -s BluetoothGatt:V BtGatt.GattService:V`

## 五、16:50 实时复核（修正上一轮的定性）

连抓 `_lc4.txt`（手机）+ `_nl15.log`（设备）两分钟，App 处于**无限重试**：
每 ~6s 连一次，每次约 2s，全部 `reason=0x213` 主动断开，已连续 12+ 轮。

单轮的真实时序（设备 ms / 手机 wall clock 对齐）：
```
设备 396052  App 订阅 001B → 设备发 link probe mng(00 00 04 00 06 12) → h45
设备 396102  App 订阅 001A → 设备再发一次 link probe            → h42
手机 43.168/43.213  写 CCCD 001B / 001A（enable=true，均成功）
手机 43.297         写 CCCD 0001（enable=true，成功）
设备 396142/396162  App 写 001B 00 00 05 00 06 12 ×2 → channel READY
设备 396192         App 订阅 0001 → auth chan=h45
手机 43.386         写 001B 20B = 00 00 05 01 12×16（测试包回执，rc=0）
设备 396432/390322  device info sent（6B + START pkg0 cnt=2 + 20B + 8B，rc 全 0）
手机 43.391         cancelOpen()  ← 距最后一次订阅仅 65~130ms
```

**修正**：上一轮说「App 对 device info 完全沉默、不回执」并不准确。App **会**回测试包
（部分轮次），也在 65ms 内收下了 device info，只是**永不进入认证态**（不写 A2/24、不写 0x15）。
所以缺的不是「帧送不到」，而是「App 不肯把这条会话当可信会话」。

两个观察（未验证，作为下一轮候选假设）：
1. **link probe 发了两遍**（h45 与 h42 各一次）。真机是设备连上后**主动宣告一次**，
   我们把「宣告」绑在了「每有新订阅」上，可能让 App 建立两个通道对象、主通道状态被打乱
   → 改成「每连接只宣告一次」是低成本实验。
2. **广播 authMode=2 + v5（FC 0x59）** 宣告「走标准认证」。若 App 依此选 ECDH 路径而
   我们的 0x0019 没有可用证书/公钥，就会在认证前置检查处直接放弃 —— 这与「0 次 read、
   0 次 A2」的现象吻合。降级到 authMode=1/0 的形态是第二个低成本实验。

## 六、方向选择（待定）

| 路线 | 内容 | 代价 | 成功率 |
|---|---|---|---|
| A 定点实验 | 每连接只宣告一次 + authMode 降级，2~3 轮固件 | 1~2 小时 | 中低 |
| B 标准认证全实现 | 证书链 + 0x0019 ECDH + k61 会话密钥 + A2/24 登录 | 数天 | 高但工程量大 |
| C 按现状交付 | 保持待绑定广播，走「添加设备」进入，详情页不做 | 0 | 已可用 |

---

# 七、详情页打通（2026-11-11 17:30）

## 7.1 根因：缺 Nordic UART 服务，不是认证问题

用 LSPatch + 自建模块（`D:/workspace/temp/xpmod`）抓 App 侧，发现夜灯2 加载的插件是
**`philips.light.nlight`**（飞利浦系），它只走 Nordic UART：

```
CALLBLE cmd=6  service 6e400001-…  char 6e400003-…  → code=-1（47ms，零 BLE 交互）
CALLBLE cmd=15 service 6e400001-…  char 6e400003-…  → code=-1（55ms，同上）
```

我们的 GATT 只有 FE95，插件本地就找不到通道 → 页面永远转圈。
补上 `6E400001`（`6E400002` 写 / `6E400003` 通知+读+写）后，**详情页立即正常打开**。

> 关键经验：`cmd=6/15` 返回 `-1` 且**设备端零字节**，说明失败发生在 App 本地
> （服务发现结果里没有目标 UUID），不是协议/认证/超时问题。以后遇到同样的
> 「50ms 内 -1」先查 GATT 表是否缺 App 要的服务。

## 7.2 插件协议：NUS 上的纯文本

抓到完整命令集（空格分隔，无校验、无帧头）：

```
get_prop power bright lux snm dur     属性轮询（10 次/会话）
get_s_prop scene                       场景属性
notify_ol                              订阅确认
apply_fixed_scene 1 / 2                应用固定场景
set_bright 58 / 31                     设亮度
set_duration 23000                     设感应时长（ms）
```

设备端目前**只记录日志、不回应**（`nus 手工回包` 计数 0）。页面能操作是因为插件
乐观更新本地状态，但重进页面数值可能回落 —— 待补应答。

## 7.3 「不能连网关 / 小爱控制不了」是型号限制，不是固件缺陷

米家官方产品 FAQ（`home.miot-spec.com/p/yeelink.light.nl1`）原文：

> **米家夜灯2 蓝牙版支持小爱同学语音控制吗？不支持。**
> 这款夜灯是**条件设备，能够触发家里的其他智能设备，不能够被其他智能设备触发**。

机制（米家 BLE Mesh 官方文档）：

> 小爱音箱语控下发的基于 Spec 的命令，经过云端后由 **BLE Mesh 网关**转换成 Mesh 消息发送给设备。
> BLE Mesh 设备通过网关能够上、下行消息，因此也支持小爱语控。

夜灯2 是 **BLE 直连设备**（GATT + 私有文本协议），不在 Mesh 网络里，云端没有下发路由。
网关对它唯一的作用是当 **人体 / 光线传感器** 用（上报事件 → 米家传感器日志 + 自动化触发条件）。

## 7.4 补上传感器上报（本轮固件）

之前夜灯2 的广播只发 `0x100A`（电量），网关收到了也只看得到电量。
现在改成三帧轮换：

| 帧 | 对象 | 值 | 帧长 |
|---|---|---|---|
| 0 | `0x000F` 移动+光照 | 2B：b1=移动 b2=光照 | 23B |
| 1 | `0x1018` 光照明暗 | 1B：0=暗 1=亮 | 22B |
| 2 | `0x100A` 电量 | 1B | 22B |

新串口命令：

```
motion      触发一次「有人移动」，立刻广播，1.5s 后自动回落发 0
lux 0/1     设环境光 暗/亮（影响自动化条件「环境光较暗时」）
```

### ★广播长度硬约束（本轮踩到的坑）

```
主广播包 = uuids16 列表(AD头2 + UUID2 = 4) + svc_data(AD头2 + UUID2 + 帧 N) = 8 + N ≤ 31
                                                     → 帧长上限 23B

帧 = 11(FC2 + PID2 + cnt1 + MAC6) + 对象(2 + 1 + 值) + ctr 3 + MIC 4 = 21 + 值
                                                     → 值最多 2B
```

真机 `0x000F` 是 **3B**（移动/光照/保留），照抄会得到 24B 帧 → `adv_set_fields rc=4`
（`BLE_HS_EMSGSIZE`），**整帧发不出去**，串口只打一行错误、广播保持上一帧不变。

之前一直没暴露，是因为夜灯2 只发电量帧（22B），恰好卡在上限内。

> ⚠️ 同一约束适用于所有 `beacon_kind` 档案：人体传感器（`0x000F` 3B）、甲醛、
> 温湿度计3 mini 等如果对象值 ≥3B，现在也会 `rc=4`。要逐个核对帧长。

## 7.5 现场验证

```
>>> motion
W  sensor: 已请求一次「有人移动」，下一轮广播 0x000F
W  sensor: 0x000F motion=1 (1.5s 后回落) light=1
I  sensor: 0x000F motion=0

>>> frames
I  slot 0 battery 23B  58 59 DD 03 F3 03 40 CE 3B 00 10 | 63 C7 1E F5 CD | 05 00 00 | E3 F0 F9 35
                        FC=0x5958 PID=0x03DD cnt MAC(air)  0x000F+len2+值    ctr      MIC
```

无 `adv_set_fields rc=`，帧正常发出。

## 7.6 待验证 / 待办

| 项 | 说明 |
|---|---|
| 网关接入 | 需要账号下有蓝牙网关（小爱音箱 / 多模网关 / 中枢网关）。米家「我的 → 中枢与网关」可查。接入后等 1~5 分钟自动连 |
| 自动化触发 | 网关上线后，用 `motion` 触发，看米家自动化是否执行 |
| 光照对象语义 | `0x1018` 是门窗传感器的光照对象，夜灯2 是否认这个 id 待确认；若不认，光照条件只能依赖 `0x000F` 的 b2 |
| NUS 应答 | `get_prop` 仍无回答，页面数值靠插件乐观更新；重进页面可能回落 |
| 其他档案帧长 | 见 7.4 的警告，人体/甲醛/温湿度3mini 可能已被 `rc=4` 影响 |

---

# 八、详情页成功 & 摆脱 hook 的路线（2026-11-11 17:5x）

## 8.1 结论一句话

详情页能打开，靠的是**补上 Nordic UART 服务**；能长期可用，还得让固件**不依赖 hook**——
目前唯一的 hook 依赖是 `keepgatt`（拦住 App 的提前断连），要靠「GETRAND 回包提速」替掉。

## 8.2 hook 依赖清单（必须清零）

| hook 开关 | 作用 | 没有它会怎样 | 固件侧替代方案 |
|---|---|---|---|
| `debug.wbhook.keepgatt=1` | 认证窗口内吞掉 core 的 `disconnectFromRN` | App 写 GETRAND 后约 37ms 就断，我们的 150ms 回包永远送不到 → 页面转圈超时 | **GETRAND 回包压到 ≤40ms**（改动见 8.5） |
| `debug.wbhook.stdlib=0` | 不劫持连接方式 | 无影响（走 App 原生路径） | 无需处理 |
| SSL pinning 关闭 | 只影响云端 HTTPS 抓包 | 无影响（设备侧无关） | 无需处理 |

> 判定方法：hook 还装着，但 `setprop debug.wbhook.keepgatt 0`，
> 若详情页仍能打开 → 无 hook 也能用。这是本轮之后的第一验证项。

## 8.3 NUS 协议真相（从插件 JS 反解，不需要 hook 也能复刻）

证据来源：手机上 dump 出的插件包 `D:/workspace/temp/xpdump/xp_dump/15_main.bundle`
（327 KB，`nlight` 出现 107 次，`get_prop` / `set_bright` 全在里面）。

**UUID**（与固件现有实现一致）：

```
6E400001-B5A3-F393-E0A9-E50E24DCCA9E   服务
6E400002-…   App → 设备   写（ASCII 文本）
6E400003-…   设备 → App   通知（ASCII JSON）
插件轮询周期 pollInterval = 6000ms
```

**App 下发的命令（纯 ASCII，空格分隔）**

```
notify_ol
get_prop power bright lux snm dur
get_s_prop scene
set_bright <1..100>
set_duration <5000..60000>          (ms)
set_bridur <bright> <dur_ms>
apply_fixed_scene <1..4>
set_scene <id> <bright> <dur_ms>
```

**设备必须回的格式（关键，之前完全不知道）**

插件收到通知后：`hexToString` → `_receiveBluetoothData`，其中：

```js
if (data.length < 15) return;        // 短于 15 字符直接丢弃
if (!this._isJSON(value)) return;    // 必须是合法 JSON
var jsonValue = eval('(' + value + ')');
if (!jsonValue.flag) return;         // 必须有 flag 字段
```

所以回包是 **ASCII JSON 字符串**：

```json
{"flag":"get_prop","result":{"power":1,"bright":58,"lux":1,"snm":0,"dur":30000}}
{"flag":"get_s_prop","result":{"scene":[{"bright":60,"dur":25000},{"bright":20,"dur":30000},{"bright":100,"dur":10000},{"bright":40,"dur":5000}]}}
```

`get_prop` 的 `result` 必须有 `snm`、`dur`、`bright`（缺一个就 `return`，页面不动）；
`dur` 会被夹到 5000~60000，`bright` 夹到 1~100。
`get_s_prop` 的 `scene` 是长度 4 的数组，元素 `{bright,dur}`，顺序对应 4 个固定场景
（浴室 60/25s、卧室 20/30s、厨房 100/10s、走廊 40/5s）。
未知 `flag`（如 `set_bright` 的 ack）走 `else {}`，**不会报错**，可以放心回。

> 这一份是从插件代码直接读出来的，不依赖 hook、不依赖抓包，可长期作为实现依据。

## 8.4 网关 / 小爱（用户已确认账号下有网关）

- 小爱语控：**型号不支持**（官方 FAQ：条件设备，只能触发别人，不能被触发）。真机同样如此。
- 网关用途：只作人体 / 光照传感器上报。本轮固件已把广播改成 `0x000F`(移动+光照) /
  `0x1018`(光照明暗) / `0x100A`(电量) 三帧轮换，`motion` 命令可手动触发一次「有人移动」。

## 8.5 本轮改动（固件）

1. `gatt_miot.c`：GETRAND 回包时延 150ms → **40ms**（`gatt_miot_set_gr_delay()`，串口 `grd <ms>` 可调），
   目的是让回包落在 App 自己的断连判定之前，从而不需要 `keepgatt`。
2. `gatt_miot.c`：NUS 从「只记录」升级为**完整文本协议实现**（8.3 的收发格式 + 4 个场景 + 状态机）。
3. 串口新增：`grd <ms>`、`nlstat`（打印 nlight 状态）、`nl <text>`（本地模拟一条 App 命令）。

## 8.6 恢复原版米家（收尾步骤，勿忘）

```bash
# 1) 备份补丁包与模块（已存在）
#    D:/workspace/temp/patch_nl3/origin-487-lspatched.apk
#    D:/workspace/temp/xpmod/build/aligned.apk
# 2) 卸载补丁版 + 装回原版（原版 APK 在 _origbase/origin.apk，md5 eadbd057…）
adb uninstall com.xiaomi.smarthome
adb install -r D:/workspace/temp/_origbase/origin.apk
# 3) 重新登录米家（原版签名，云端数据不变）
```

> 注意：卸载会清掉本地缓存与登录态，设备绑定关系在云端，重新登录后自动同步。

## 8.7 验证顺序（下一步）

1. `keepgatt 0` + 补丁版 App → 详情页能开、数值能回读 → 证明**不依赖 hook**
2. 装回原版 App（8.6）→ 再开详情页 → 无 hook 环境下的最终确认
3. 网关接入 → `motion` → 米家自动化触发

---

# 九、无线开关双键（XMWXKG01YL / PID 0x1949）闭环（2026-11-11 19:30-20:15）

## 9.1 一句话结论

物理按压 → MiBeacon 广播 → 米家「智能日志」显示「左键单击」，**1:1 打通**（3 次按压正好 3 条记录，
无漏报、无 ghost）。自动化条件也已出现，条件是**随设备 spec 落到本地库**的，与固件无关。

## 9.2 设备身份（真机取证）

```
model      = yeelink.remote.contrl
productId  = 6473
spec       = urn:miot-spec-v2:device:remote-control:0000A021:yeelink-contrl:1
did        = blt.3.1qrb4eed50401
MAC        = 10:00:3B:CE:43:03
```

## 9.3 按键对象与帧预算

| eid | 语义 | 载荷 button-type |
|---|---|---|
| `0x4E0C` | 单击 | 1=左 2=右 3=同时 |
| `0x4E0D` | 双击 | 同上 |
| `0x4E0E` | 长按 | 同上 |

帧长：11B 头 + 对象(2+1+1) + ctr 3 + MIC 4 = **21B ≤ 23B**（23B 是主广播包硬上限）。

固件实现（`app_main.c`）：`s_btn_obj` / `s_btn_req` / `s_btn_until`，事件保持 2.5s 后回落电量帧 `0x100A`。
串口触发 `btn <eid> <key>`；**BOOT 固定发左键单击**（`s_btn_key = 1`，不受上一次 `btn x 2` 影响）。

判据日志：

```
W MIOT: btn: 0x4E0C 单击(左键)，重复广播 2.5s
I MIOT: btn: 事件结束，回到电量帧
```

## 9.4 自动化条件的真实来源（本轮最重要的结论）

不是「米家不支持」，也不是云端 spec 缺失。条件来自**本地库** `databases/device_tca_config.db`：

```sql
select sc_id,name,l_did from device_launch_configs_v3 where model='yeelink.remote.contrl';
-- 11674 单击左键   11675 双击左键   11676 长按左键
-- 11677 单击右键   11678 双击右键   11679 长按右键
-- 11680 同时单击左右两键
```

每条条件的触发映射：

| 条件 | event key | filter |
|---|---|---|
| 单击左/右 | `event.yeelink.remote.contrl.3.1012` | `prop.3.1`(button-type)=1 / 2 |
| 双击左/右 | `..3.1013` | button-type=1 / 2 |
| 长按左/右 | `..3.1014` | button-type=1 / 2 |
| 同时单击 | `..3.1012` | button-type=3 |

固件发的 `0x4E0C` + 载荷 `1` 正好是 **button-type=1**，语义对齐。

**时间线解释**：`device_config_v3_access.ts = 1794396237658` → **2026-11-11 19:23** 才落库。
用户 18:5x 查看时库里是空的，所以条件列表为空；重进一次自动化页后再看就出现了。

取证命令：

```bash
adb exec-out "run-as com.xiaomi.smarthome cat databases/device_tca_config.db" > tca.db
adb exec-out "run-as com.xiaomi.smarthome cat files/mmkv/mmkv_id_local_control" > id_local_control.bin
```

spec 侧同源证据：`files/spec_support_cache_v5/128/<hash>` 是 gzip JSON，
`services[3]` = `remote-control`，含 `3.1012 click / 3.1013 double-click / 3.1014 long-press`（参数 button-type）。

## 9.5 唯一剩余待验证：端到端自动化

1. 建自动化：**如果「双键无线开关 → 单击左键」→ 就「夜灯 → 开」**
2. **关掉手机蓝牙**（强制事件只走网关）
3. 按一次 BOOT
4. 看夜灯是否亮

不通则问题在**网关侧解码/上报**（MiBeacon MIC / eid），不在米家配置，也不在固件广播。

## 9.6 端到端已通过（2026-11-11 20:5x 实测）

用户在原版米家（**无任何 hook**）上建了自动化：**开关按键 → 发送通知**，触发后手机收到消息。
结论：**按键事件 → 广播 → 米家（网关/直连）→ 自动化执行** 整条链路闭环。
这条同时证明 10.2 的「去 hook」成立：开关侧不需要 hook。

## 9.7 附带发现：一颗芯片同一时刻只能扮演一个型号

夜灯2 详情页在原版 App 下超时的根因**不是 hook**，而是 MAC 随档案派生（`app_main.c:1443`）：

```c
uint8_t base[6] = { 0x10, 0x00, 0x3B, 0xCE,
                    (uint8_t)(0x40 + (s_sel & 0x0F)),   /* 第 5 字节 = 档案号 */
                    (uint8_t)(0x01 + (s_macv & 0x3F) * 2) };
```

| sel | 档案 | 广播 MAC |
|---|---|---|
| 0 | NIGHTL 夜灯2 (0x03DD) | `10:00:3B:CE:40:03` |
| 3 | BTN2 无线开关双键 (0x1949) | `10:00:3B:CE:43:03` |

当前固件在 **sel=3**，只广播 `43:03`。米家里「夜灯2」这条记录对应的是 `40:03`，
设备不在广播 → App 连不到 → 页面上报「连接超时」（设备侧串口 0 行日志可佐证：**从未有连接事件**）。

`dev N` 切换档案会**擦 NVS 身份**（`sel_store` + 清绑定），所以：
- 切到夜灯2（`dev 0`）= 开关需要重新配对一次；
- 想让夜灯2 与开关**同时在线**，只能上第二块板子（每块板一个档案、一个 MAC）。

判定要点：**串口 0 行 = App 根本没连上来**（MAC 不对），
**有 connect 无 GETRAND = GATT 服务/句柄问题**（那才是固件侧）。

---

# 十、去 hook 收尾（2026-11-11 20:20）

## 10.1 当前安装状态

```
安装包       = D:/workspace/temp/patch_nl3/origin-487-lspatched.apk (245,253,143 B)
真机 base.apk 同尺寸，versionName=11.9.623，安装于 2026-11-11 18:13
管理器       = org.lsposed.lspatch（local 模式，xpmod 已内嵌，无独立模块包）
原版备份     = D:/workspace/temp/_origbase/origin.apk (240,842,536 B)
               md5 = eadbd057830c53166082589c2e7c9444
```

## 10.2 固件侧已消除的 hook 依赖（逐条）

1. GETRAND 回包时延 150ms → **40ms**，且回**明文 rand**（`s_strat=10`），不依赖 hook 改包；
2. **NUS 服务完整实现**（`6E400001/2/3`），不再需要 hook 伪造 GATT；
3. `legacy_reply_handle()`：回包优先落在「来路面已订阅的句柄」，其次 `0x0001`，最后 `0x0010`，
   解决连接静默 35 秒的问题（夜灯2 成功轮回在 `h=16`）；
4. 已验证：`keepgatt=0`（等价无 hook）下夜灯2 详情页正常，数值 60% / 25s 为真值。

## 10.3 恢复原版（清除 hook）

```bash
adb uninstall com.xiaomi.smarthome
adb install -r -d D:/workspace/temp/_origbase/origin.apk
```

> 卸载会清掉本地缓存（含 `device_tca_config.db`、登录态、spec 缓存）。
> 设备绑定关系在云端，重新登录后自动同步。
> 若之后还要 hook 调试：重装 `patch_nl3/origin-487-lspatched.apk`，需重新配对（NVS 未清时可能免配对）。

## 10.4 装原版后的验收清单

1. 登录成功，设备列表里设备在线（蓝牙范围内）
2. 夜灯2 详情页：开关 / 亮度 / 延时可用，数值为真值而非 0
3. 无线开关：按 BOOT → 米家「智能日志」出现「左键单击」
4. 自动化条件：设备出现在「如果 → 设备」列表，点进去 7 条条件齐全

## 10.5 纯原版（无任何 hook）实测通过 —— 2026-11-11 20:30

`dev 0` 切到夜灯2 档案（MAC `10:00:3B:CE:40:03`，`bound=0`），在**原版米家**重新完成绑定，
串口全程留证（`_nl2.log`）：

```
 57.605 I  connected conn=1 mtu=23
 59.415 W  legacy 4B 兜底应答 E2 E2 5A FD -> 22 16 7F 58 (src=23 回=16)   ← 回在已订阅句柄
 71.144 W  legacy GETRAND(注册) strat=10 rand=EF9DAA03 -> resp=3DCE9152
 71.185 W  auth 回包 #1 h=16 raw-rand rc=0                                ← 40ms 内送达
 72.318 I  disconnected reason=0x213 -> adv resumed（App 主动断开，正常）
 77.074 I  connected seq=2 -> adv KEPT (bound, beacon alive)              ← 绑定成功
 78.207 W  NUS rx h=49 len=9: notify_ol
 78.222 W  NUS rx h=49 len=33: get_prop power bright lux snm dur
 78.228 W  NUS tx h=51 len=41 rc=0
```

**结论：GETRAND 40ms + 明文 rand（strat=10）+ NUS + `legacy_reply_handle()` 全部在固件侧，
不需要任何 hook。** 补丁版可以彻底退役。

## 10.6 夜灯2 的自动化条件：只有 1 条「夜灯亮起」（2026-11-11 22:03 截图实测）

米家设备页 →「支持设置自动化」→ **支持的触发条件：夜灯亮起**（状态类条件，1 条）。

修正此前「灯类没有条件」的推测：米家对灯类给的是**状态变化**条件（"亮起"），
而不是事件类（单击/双击那种）。所以：
- 「如果」里可以选夜灯，但只有「夜灯亮起」这一条；
- **没有**「有人经过 / 光照变化」这类触发——因为夜灯2 的红外感应是**本地行为**
  （自己亮灯），不上报云端，spec 里也没有对应 event。

想用「有人经过」当触发，两条路：
1. 固件自带的**人体传感器2 档案**（sel=8, RTCGQ02LM, 0x0A8D）——sensor 品类，才有事件条件；
2. 用已闭环的**无线开关**做触发（单击/双击/长按共 7 条条件）。

## 10.7 「出现多个夜灯」的原因与清理（2026-11-11 22:06）

**广播侧检查结果（Windows BLE 扫描 12s，`bleak`）**：只有 **1 台**在广播，完全正常：

```
10:00:3B:CE:40:03  name='MUE4094RT'  rssi=-63
  service: 0000fe95-0000-1000-8000-00805f9b34fb  (FE95，小米 MiBeacon)
  mfg 0x038F: 3029 dd03 6b 0340ce3b0010 08        ← PID 0x03DD，MAC 反序
  svc_data  : 5859 dd03 6b 0340ce3b0010 0e95 …     ← 加密帧 + MIC
```

**米家侧却有两条夜灯记录**（logcat 22:06:03 `WidgetIcon` 行直接给出 did）：

| 名称 | did |
|---|---|
| 飞利浦蓝牙夜灯 2 | `blt.3.1qrcncnc90k00` |
| 飞利浦蓝牙夜灯 | `blt.3.1qrcnhg6h0400` |

两者图标路径相同（`f8f78177d83a813e`），确认是**同一型号的两条独立云端记录**。

**成因**：米家蓝牙设备的 did 由 MAC 派生，而本固件的 MAC 随两件事变化：
1. `dev N` 切档案 —— MAC 第 5 字节 = `0x40 + sel`（见 9.7）；
2. `newmac` / `setmacv N` —— 末字节 = `0x01 + 2*macv`，用于绕 DeviceBlackList。

每次 `dev N` 都会**擦 NVS**，设备变成"全新未绑定"；重新绑定时米家就多一条记录。
历史上切过档案 / 换过 MAC，于是留下多条夜灯（同样的问题也会出现在开关上）。

**清理办法**：
1. 打开两台夜灯，**能连上、有亮度数值的那条是当前 MAC（40:03）对应的**；
2. 另一条点开必然超时/提示离线 —— 进设置页**删除设备**；
3. 以后换档案前，先在米家删掉旧记录，或绑定后用 `setmacv` 回到原 MAC 复用同一 did。

> 广播本身没有问题：一台设备、一个 PID、一个 MAC，没有重复广播。

## 10.8 根因修复：身份按档案隔离（2026-11-11 22:20，已烧录验证）

**病因**：`dev N` 的实现是 `nvs_flash_erase()` + `sel_store(N)` + 重启，
也就是**切一次档案就清空整个 NVS**，设备回到 `bound=0`（全新未绑定）。
米家下次连它时按「新设备」处理，重新走绑定并在云端新建记录 —— 于是：
**每超时/重绑一次，就多出一台夜灯（或开关）**。

**改法**（`esp32/multi_dev/main/app_main.c`）：

| 项 | 旧 | 新 |
|---|---|---|
| 命名空间 | 全部共用 `"miot"` | 每档案一个：`sel=0 → "miot"`，`sel>0 → "miot<sel>"`（`ns_build()`） |
| `dev N` | `nvs_flash_erase()` + 擦身份 | **只写 `devsel` 并重启**，身份保留 |
| `factory` | 整片擦除 | 只 `nvs_erase_all()` 当前档案的命名空间 |
| `ensure_pid_match()` | 不匹配时 `nvs_flash_erase()` 整片 | 只擦当前命名空间 |
| `ident / fcnt / devpid / macv` | 全局共用 | 跟随当前档案（`s_ns`） |
| `devsel` | 在 `"miot"` | 移到全局 `"miotcfg"`，读不到时回落读 `"miot"`（兼容旧固件） |

**验证（烧录后串口实测）**：

```
烧录后首次启动     : sel=0 tag=NIGHTL pid=0x03DD bound=1     ← 旧绑定完好
dev 3              : sel=3 tag=BTN2   pid=0x1949 bound=0     ← 开关档案独立（需绑一次）
dev 0              : sel=0 tag=NIGHTL pid=0x03DD bound=1     ← 切回来绑定还在
```

关键点：**切档案不再丢身份**，同一档案的 MAC/did 恒定，米家认的就是同一台设备，
不会再因为「重新绑定」堆出新记录。

> 一次性影响：`sel=3`（开关）之前的身份已被旧逻辑擦掉，需要重新配对**一次**；
> 之后无论怎么切，两边都各自保留。

---

## 十一、重新绑定与「连接超时」的真相（2026-11-11 22:2x）

### 11.1 「扫描不到」= 设备侧仍带 token

`factory` 之前设备是 `bound=1`，广播里 BondAbility 字节为 **0x29（已绑定）**：

```
mfg 30 29 dd03 ...      ← 米家「添加设备」只认 0x20，0x29 的帧直接过滤
```

在 App 里删设备**不会**清设备侧 token，所以删完再扫永远扫不到。
正确顺序：设备端 `factory`（bound→0，广播切 0x20）→ App 再扫。

```
factory → info : sel=0 tag=NIGHTL pid=0x03DD bound=0
扫描确认      : mfg 30 20 dd03 ...  name='MUE4094RT'
```

### 11.2 绑定耗时线（本次重绑，串口时间轴）

| 时刻 | 事件 |
|---|---|
| 16.5s | 连接 seq=1，App 发 AUTH_INIT（静默不应答，符合旧设备行为） |
| 17.7s | `legacy AUTH_INIT` 后写 4B（XOR/rand 两种 kl 各一次）→ token 落盘 → `bind state -> 1` |
| 18.1s | did 从 0x0013 读出并入库（20B） |
| 29.7s | GETRAND(注册) strat=10 明文 rand → 回包落在 h=16（已订阅） |
| 30.1s | NUS 起来：`notify_ol` / `get_prop` / `get_s_prop` 全部应答 |
| 45.7s | App 主动断开（0x213），广播恢复可连接 |

### 11.3 页面「连接超时」是瞬时状态，不是故障

绑定完成后 45.8s → 139.4s 之间**设备侧一个连接请求都没收到**（App 在「添加成功」页停留），
这段时间详情页显示「连接超时」；139.4s 连上后一直 6s 轮询。

之后实测双向控制（这就是闭环证据）：

```
284.870  NUS rx h=49 len=13: set_bright 35
284.892  NL tx: {"flag":"set_bright","result":{"bright":35}}
293.457  NL tx: {"flag":"get_prop","result":{"power":1,"bright":35,"lux":1,"snm":0,"dur":41000}}
```

页面亮度从 60 改到 35，设备侧状态跟着变，回读也是 35 —— 读写都通。

**结论**：`连接超时` = 页面打开时那一次 GATT 连接窗口没抢到，插件自动重试后恢复；
若以后长时间不恢复，抓 logcat 看 GATT status（133/timeout）再对症处理。

---

## 十二、网关与「谁能收」的三道门槛（2026-11-11 22:4x）

一次事件能被云端看到，要同时过三道关：**有 token（能解密）→ 认识该 PID（能解析）
→ 有上报通路（网关/手机 → 云）**。

| 接收端 | 有 token | 认识 PID | 上报通路 | 结果 |
|---|---|---|---|---|
| 手机 App | ✓ 绑定即持有 | ✓ 按 did 拉 spec | ✓ App 转报 | 只在手机附近生效 |
| 青萍网关 | ✓ 云端下发 | ✓ 通用 MiBeacon 解析 | ✓ 网关转报 | 可 7×24 |
| 小爱 Play 增强版 | ✗ 只给白名单品类 | ✗ 仅 Mesh + 少数品类 | ✓ | 只认温湿度计一类 |

- 小爱 Play 增强版的网关官方定位是**蓝牙 Mesh 网关**，Mesh 走配网流程，与 MiBeacon
  广播不是一套；对 MiBeacon 只有极小品类白名单，PID 不在表内时扫描阶段即丢弃。
- **小爱同学的自动化场景可以添加**（更正早期「该型号不支持小爱」的结论）；
  能否触发仍取决于当时是否有上报源。
- 待验证：关手机蓝牙 → 按 BOOT，看青萍网关是否上报。设备侧无法判断（广播是单向的）。

---

## 十三、脱离手机的远程触发已实测通过（2026-11-11 23:0x）

### 13.1 实测方法

| 步骤 | 操作 |
|---|---|
| 1 | 手机**关闭蓝牙 + 关闭 Wi-Fi**（不只是关蓝牙，Wi-Fi 也断，彻底剥夺手机的上报通路） |
| 2 | 等设备在米家里显示离线 |
| 3 | 按一次 BOOT（发 `0x4E0C` 单击左键，载荷 1） |
| 4 | 听/看小爱同学自动化场景 `播放「欢迎回家」` |

### 13.2 结果：**成功**

手机 BT 与 Wi-Fi 同时关闭时，场景仍然触发并播报。这只有一种解释：
**事件经某个蓝牙网关解密、解析并上报到云端**，再由云端下发场景执行。
手机在这条链路里完全缺席。

同时验证了两件附带结论：
- **不存在重复触发**：一次按压 → 一次播报（此前担心的「2.5s 重复广播被网关逐帧上报导致连击」没有发生）；
- 已绑定帧（BondAbility `0x29`）在网关侧正常被接收，说明 token 已下发到网关。

### 13.3 上报源 = 青萍网关（已确认）

断电对照完成：**青萍网关断电后不再触发** → 上报源是青萍，小爱 Play 增强版不承担
MiBeacon 上报，只做执行端（播报音频、放「欢迎回家」）。

职责划分可以这样记：

| 角色 | 设备 | 干什么 |
|---|---|---|
| 听（上报） | 青萍网关 | 解密 MiBeacon → 解析 PID/事件 → 上报云端，7×24 |
| 说（执行） | 小爱 Play 增强版 | 接收云端下发的场景动作 → 播报/播放 |
| 临时替补 | 手机 | App 在旁时也能上报，离开即失效 |

> 第十二章「小爱 Play 只认白名单品类」由此得到实证：它确实不解析我们的 PID
> （0x1949 / 0x03DD），所以整条链路必须依赖青萍。

### 13.4 设备侧旁证（串口失联期间的替代手段）

测试当时串口控制台静默（USB CDC 假死，见 13.5），改用 BLE 扫描旁证设备存活与绑定态：

```
10:00:3B:CE:43:03  name='XMWXKG01YL'  rssi=-66
mfg 30 29 49 19 83 03 43ce3b0010 08      ← 0x29 已绑定，PID 0x1949
```

### 13.5 串口控制台假死的处理

现象：`info` / 复位脉冲均 0 行输出，但 BLE 广播正常、自动化正常。
判断是 USB CDC 侧假死而非固件崩溃。
处理：**拔插 USB**（或断电重启）即可恢复控制台，不影响已保存的绑定与档案。
