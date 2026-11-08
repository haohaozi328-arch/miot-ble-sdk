# 米家 BLE Spec v2 协议逆向文档

> 来源：S400 体脂秤模拟固件 + 米家 Android App 反汇编（com.miot.spec 包）+ 空中抓包互证。
> 所有格式均已实测验证：以设备端串口日志与米家 App 的实际行为（绑定完成、属性读写
> 生效、数值刷新、事件触发）互相印证，不依赖任何对 App 的修改或注入。

## 1. 总体链路

```
米家 App                                        设备（模拟器）
─────────────────────────────────────────────────────────────
GATT write  0x0019  ──标准认证/绑定──►          0x0019 (注册/ECDH)
GATT notify 0x0019  ◄──认证事件──

GATT write  0x001A  ──ag2 帧──►
                    （内含 k61 加密 spec 帧）
GATT notify 0x001A  ◄──ag2 帧（k61 加密 spec 应答/事件）──
```

- **0x0019**：标准认证通道（REGISTER 绑定 + LOGIN 会话密钥派生）
- **0x001A**：spec 数据通道（App 只在此通道收 notify；0x001B 的 notify 路由到
  IDLE 推送会话直接丢弃 —— 实测）
- **0x001B**：App→设备第二写通道 / 设备推送会话

## 2. ag2 可靠传输层（0x001A 帧格式）

```
控制帧: [00 00][type][...]
数据帧: [seq16 LE][payload ≤18B]        payload = 完整 k61 加密帧

type 0 START : [00 00][00][pkgType][count LE16]   声明随后 count 个数据帧
type 1 ACK   : [00 00][01][status][seq16...]*
type 2 SINGLE: [00 00][02][pkgType][payload]      整帧一次到达
type 3 SACK  : [00 00][03][code]
type 5 SYNC  : [00 00][05][seq16...]*             丢失帧重发请求
```

ACK status 语义（认证通道实测）：
- App 发 START → 设备 ACK `status=1`（就绪）
- App 数据帧收齐 → 设备 ACK `status=0`（确认）
- App 只认对应状态才推进状态机；START 后无 ACK 会 6 秒重试

**重组缓冲必须 ≥512B**：App 的 sync action（userinfolist 含 string）>149B，
160B 缓冲第 9 帧即溢出，包组不齐 → App 收不到应答 → 页面卡住。

发送分帧：每帧最多 18B 载荷（与 App 认证通道分帧一致）。

## 3. k61 加密帧（spec 通道载荷）

```
[seq16 LE][ciphertext][tag 4B]
```

- AES-CCM，tag 4 字节，无 AAD
- nonce(12B) = `iv(4B) || 00 00 00 00 || seq32 LE`
- 会话密钥 40B（标准认证 HKDF 派生）：
  - `E = key[0:16]`（设备→App 加密 key）
  - `D = key[16:32]`（App→设备 解密 key）
  - `ivRx = key[32:36]`，`ivTx = key[36:40]`

## 4. Spec v2 明文帧（k61 解密后的内容）

```
[len16][tid16 LE][opcode][payload...]

len16: 高 4 位 = 协议版本(=2)，低 12 位 = 帧总长（含 len16 自身）
tid  : 事务 ID，应答原样带回
```

### 4.1 opcode 一览

| op | 名称 | 方向 | 帧体 |
|----|------|------|------|
| 0  | set_properties 请求 | App→Dev | `[count]([siid][piid16][type16][len][value])×count` |
| 1  | set_properties 应答 | Dev→App | `[count]([siid][piid16][result16])×count` |
| 2  | get_properties 请求 | App→Dev | `[count]([siid][piid16])×count` |
| 3  | get_properties 应答 | Dev→App | `[count]([siid][piid16][result16][type16][value])×count` |
| 4  | properties_changed | Dev→App | `[count]([siid][piid16][type16][len][value])×count` |
| 5  | action 请求 | App→Dev | `[siid][aiid16][argc]([piid16][type16][len][value])×argc` |
| 6  | action 应答 | Dev→App | `[respCode16]` |
| 7  | event 事件 | Dev→App | `[siid][eiid16][argc]([piid16][type16][len][value])×argc` |
| F0 | 版本交换 | 双向 | 无（`[05 20][tid][F0]` 镜像应答） |

### 4.2 ⚠️ 四个决定性的格式细节（踩坑实证）

1. **op=7 事件每个参数前必须带 16 位 piid**
   缺失时 App 端 `ud5` 解析器返回 NULL，事件全部被丢弃（早期 75 包全 NULL）。
   参数 piid 取值按 eiid 语义回填（如 S400 8.1 事件唯一参数 piid=1、8.2 唯一参数 piid=2）。
2. **op=4 properties_changed 帧体首字节是 count**
   `gyb` 解析器首先读 count；把 siid 当首字节会被解析成 count=11 的残包。
3. **op=6 action 应答不能带 siid/aiid，respCode=0 时可带 out 参数**
   `kt4` 格式：`[06][respCode16][count][piid8][type16][value]...`（count 为 out
   参数个数，piid 为 8 位）。无 out 参数时仅 `[06][respCode16]`。
   把 `[siid][aiid]` 放在 respCode 位置会被误读为 `respCode=269` → App 判定动作
   失败 → 无限重发（如 13.sync 的 aiid 实为 0x0501=1281，out=[6,7,8,9,10]，
   必须按 kt4 带齐 5 个 out 参数，App 的同步流程才会推进）。
4. **App→设备的 SET 条目 type16 实际也是 `(type<<12)|len`（v65/v66 串口实测）**
   v65 抓包推翻了早期结论：App 下发 10.1 `set mode=bool(false)` 时 type16 = `0x1001`
   = `(0<<12)|1`，即与设备→App 方向**相同**的 `(SpecValueType << 12) | len` 格式。
   早期按「纯类型号 + len8」解析会把 len 读成 0 → 拒绝（result=1，rc=-2/-4）。
   固件解析建议做双格式判别（v66 起）：
   ```c
   uint16_t rawt = pt[p+3] | (pt[p+4] << 8);
   if ((rawt >> 12) != 0) {        /* 新：(type<<12)|len，len 12 位 */
       vtype = rawt >> 12;  vlen = rawt & 0xFFF;  val = pt + p + 5;  adv = 5 + vlen;
   } else {                        /* 旧兼容：纯类型号(高4位为0) + len8 */
       vtype = rawt;  vlen = pt[p+5];  val = pt + p + 6;  adv = 6 + vlen;
   }
   ```
   边界：若某属性真以 type=0（bool）下发 SET，`(type<<12)|len` 的高 4 位也为 0，
   会落入旧格式分支而被误读。实测未触发该情况——App 对 10.1（bool 属性）的
   set 实际发的是 `0x1001`（type=1/uint8，len=1）。

### 4.3 value 编码

设备→App 方向：`type16 = (SpecValueType << 12) | len`（与米家 spec JSON 的
`SpecValueType` 枚举一致），16 位字段一律小端。

| SpecValueType | 类型 | 备注 |
|---------------|------|------|
| 0 | bool | |
| 1 | uint8 | |
| 2 | int8 | |
| 3 | uint16 | |
| 4 | int16 | |
| 5 | uint32 | |
| 6 | int32 | |
| 7 | uint64 | |
| 8 | int64 | |
| 9 | float | |
| 10 | string | |

> ⚠️ 不要把 SDK/spec 内部自定义的类型枚举直接当 type16 发送：枚举值错位后
> App 会按错误类型解析（实测 uint32 发成 4 被当 int16，时间戳 1793491239
> 变成 -32353）。
>
> ⚠️⚠️ **枚举到 10 就结束，没有 FLOAT64**（`bool=0 … int64=8, float=9, string=10`）。
> 本 SDK 曾多定义了一个 `MIOT_TYPE_F64=10`，把 `STRING` 顶到 11 —— 结果体脂秤
> 8.1/8.2 事件的 type16 发成 `0xB0xx`，App 按未知类型把**整帧丢弃**，页面上
> 表现为「点测量后 4 秒变回重新上秤」，传输层却全是 ACK，极具迷惑性。
> 判定方法：抓自己的 tx 帧看 type16 首字节，string 必须是 `0xA0xx`（len 在高 12 位）。
> 2026-11-08 实测：改成 10 之后同一套序列立刻出数。

result 语义：0=成功；非 0=错误码（未认识的属性回 1）。

### 4.4 实测帧样本（App 解析成功）

```
事件 8.1 "0,0"（16B）:
10 20 b6 00 07 08 01 00 01 01 00 03 a0 30 2c 30
└len┘└tid─┘ └op┘└si┘└eiid─┘ └c┘└piid─┘└type16┘└"0,0"┘

事件 8.2 九参 CSV（35B）:
23 20 b9 00 07 08 02 00 01 01 00 16 a0 30 2c 30 ... 2c 30
└len┘└tid─┘ └op┘└si┘└eiid─┘ └c┘└piid─┘└type16=(10<<12)|22┘└CSV...┘

action 应答（7B，respCode=0）:
07 20 0a 01 06 00 00
└len┘└tid─┘ └op┘└resp┘
```

## 5. App 侧分发链（插件收不到事件时的排查路径）

```
GATT notify (0x001A)
  → ag2/agg 传输层（App 对同一帧双发 ACK/notify 是设计行为）
  → k61 解密（com.miot.spec）
  → SpecManager: 广播 action "action.miot.receive.specv2.ble.data"
  → SpecReceiver.onReceive 解析（ud5/gyb/kt4 按包型分派）
  → 成功: PluginApi.broadcastToRn("com.xiaomi.smarthome.ble.spec.notify", json)
  → TinyEventReceiver → RN 事件 BLESpecNotifyActionEvent_<pid>
  → 插件 JS: subscribeReceivedMessage 按 spec topic (siid/eiid) 匹配
```

调试路径（设备端判据，无需改动 App）：

1. 设备发出帧后，看 App 是否回 ag2 ACK、是否紧接着发下一条请求 —— 回 ACK 说明帧
   结构正确，无 ACK 则是分帧/长度问题；
2. 推送属性后，看设备是否收到 App 的 get 回读（串口 `spec req` 行）—— 有回读说明
   属性已进入 App 缓存；
3. 插件是否消费，静态对照插件 JS bundle 的 topic 匹配表即可，不必修改运行中的 App。

## 6. 旧版 beacon 鉴权（token 加密广播）

- RC4 + mixA/mixB 密钥派生：m = 设备 MAC 空中序 6B，pid 16 位小端
- 广播帧里的 token 用 mixA/mixB 派生密钥 RC4 加密

## 7. 标准认证 REGISTER（绑定，一次性）

```
app 订阅 0x0019 CCCD → 设备 notify [00][ver_lo][ver_hi][caps]
app 写 [15][ver_lo][ver_hi][00] → app 写 64B 公钥
                                → 设备 notify [03]+64B 公钥
app 写 16B encrypt-data → app 写 24B cloud-cert
                        → 设备 notify [11 00 00 00]（OK）
app 写 [13 00 00 00]（bind result）→ 设备存 token/bind_key，绑定完成
```

注意：ECDH P-256 密钥对要在开机阶段预生成（ESP32-C3 上 ecp_gen_keypair 约
390ms，放在 0x15 处理里会导致 device info 迟到，App 以 -51 断开）。

## 8. 标准认证 LOGIN（会话密钥派生，纯对称）

全部按反编译 standardauth/OooO0OO.java 对齐：

```
phone -> 0x0010  24 00 00 00                       （CMD_LOGIN）
phone -> 0x0019  START pkg=0x0B DATA [appRand 16]
dev   -> 0x0019  START pkg=0x0D DATA [devRand 16]   （App 用它算 salt）
dev   -> 0x0019  START pkg=0x0C DATA [devConfirm 32]（App 用 E[0:16] 校验）
phone -> 0x0019  START pkg=0x0A DATA [appConfirm 32]（App 用 E[16:32] 算）
dev   -> 0x0010  21 00 00 00                        （登录成功）
```

```
ikm  = token12
salt = appRand(16) || devRand(16)
E    = HKDF-SHA256(ikm, salt, "mible-login-info", 64)   /* 64B */
devConfirm = HMAC-SHA256(E[0:16],  devRand || appRand)
appConfirm = HMAC-SHA256(E[16:32], appRand || devRand)  /* 两边数据顺序相反，照抄 */
```

关键坑：
- devConfirm 必须重发（首发延后 120ms，之后每 400ms 补发，最多 4 次），
  否则 App 偶尔收不到 0x0C，15s 后按 notify timeout 断开
- 登录成功码 {0x21,0,0,0} 必须发在 **0x0010**；0x0019 上的通知只进通道层
- 会话密钥布局（E 的 64B）：Ekey[0:16]=设备加密 / Dkey[16:32]=设备解密 /
  ivTx[32:36] / ivRx[36:40]；k61 nonce 用 ivTx 或 ivRx 对应取

## 9. REGISTER（绑定）细节补充

- a2（0xA2 = 0x22|0x80）后 App 一定等 device info（channel data msgType=0，
  载荷 [ver_lo=2][ver_hi=0][caps=0][0]+did，**少一字节报 -47**）
- 设备公钥 64B 必须走 ag2 分片（pkg=3），单帧 68B 会被 SACK 丢弃
- 收到 64B 公钥后：ECDH → HKDF(secret, zeros16, "mible-setup-info", 64) →
  token=[0:12] bind_key=[12:28] dev_key=[28:44] → 回公钥+devRandom(0x0D)+devConfirm(0x0C)
- 92B 加密 did：AES-CCM(dev_key, iv=0x10..0x1B, aad="devID", tag4)，试偏移 0/2/4；
  明文 88B = did(20)+cloud_sign(64)+utc(4)。**必须解出并持久化**，否则 App
  每次连接都当新设备反复绑定
- did 回传（device info）前去掉前后导 0x00（云端 did 是左补零到 20B）
- 云证书 24B（旧版）/163B（11.9.623）收到后回 {0x11,0,0,0}（0x0010），
  否则 App 一直显示「设备初始化」
- 通道建链（0xA4 on 0x0010）→ 回 mng(code=0) num=6 dmtu=18；dmtu 给 0 会让
  App 算出负长度测试包 → 整条建链卡死
- ag2 0x0019 数据帧按帧号判收齐（d[0]==expect），不能按计数 —— 163B 证书
  22 帧实测计数会少 1

## 10. S400 体脂秤（yunmai.scales.ms103）数据面

spec 树是「标准 MIoT（siid 1/2/11）+ 云米/yunmai 私有服务（siid 6/8/9/10/13/14）」
的合并树。插件（plugin_id 1013486）实际订阅：

| spec | 载荷 | 插件行为 |
|------|------|----------|
| 8.1 事件 weight-upload（args=[1]） | 字符串 `"体重(0.01kg),状态"` | 实时称重页：体重÷10=kg；状态 0=weighting 1=bodyWeight 2=weighingHeart 3=overweighing 4=timeout 5=smallWeightCompleted 6=done |
| 8.2 事件 upload-body-data（args=[2]） | 28 字段 CSV 字符串 | 测量结果：按插件 uploadBodyDataObj 下标取值（weight=3、heartRate=4、bfp=7、slm=8…），keyNeedDivide10 字段需 ÷10 |
| 11.1022 事件 | 权限位 | 测量完成收尾 |
| 10.1 属性 work-mode | uint8 | 页面第一道判断：0=normal 1=baby 2=balance 3=visitor 4=single |
| 13.sync action | aiid=0x0501(1281)，out=[6,7,8,9,10] | 历史同步，kt4 应答必须带 out 参数 |

关键字段（8.2 CSV 下标 5）`weightDoneStatus`：
`0`=normalData（直接入库）/ `2`=userError（弹「是你吗」）/ `4`=multipleUsers（弹选人）。

云端入库：插件按 `/eco/common/scale/add` 上报（时间戳取 App 侧 `Date.now()`）。
**实测坑**：App 重启后插件可能把 10.1 mode 写成 3/4（visitor/single），此后
每轮测量都走「访客单机」分支——报告页时间戳变 1970、不调云端。固件应在每次
BLE 连接建立时把 work-mode 复位为 0（App 随后的 set 仍可正常覆盖）。

入库成功判据：报告页时间戳为真实测量时间、记录挂在正确使用者名下且历史列表可见。

## 11. spec 双层权限：access vs gatt-access（⚠️ 入库死锁根因，2026-11-04 定案）

云端 spec JSON 里每个属性带**两层权限字段**，App 对两层的执行点不同：

| 字段 | 含义 | App 执行点 | 后果 |
|------|------|-----------|------|
| `access` | 云/miot 全局权限 | 插件 JS 框架：`setProps` 前校验 | `access:[]` → setProps **静默失败，连 BLE 帧都不构造**（成功/失败回调均不触发） |
| `gatt-access` | BLE GATT 通道权限 | App native SpecMgr：收到设备推送时校验 | 无 `notify` → 设备推的 op=4 properties_changed **整帧丢弃**（传输层 ACK 但 SpecMgr 0 解析） |

S400 的 10.1 work-mode 实测：`access:[]` + `gatt-access:["read","write"]` ——
三重死锁：
1. 插件 JS setProps（四键跳转）被 access 拦 → 无法写 mode；
2. 设备推 op=4（mode=0）被 gatt 权限过滤（无 notify）→ 缓存无法刷新；
3. 设备伪装 GET 应答（opcode 3）也被丢 —— App SpecMgr 只处理「自己发起请求的
   应答（tid 匹配）」和 opcode 7 事件；收包白名单实测仅 op7 + f0。

**结论：属性缓存只能经「App 发起请求的应答」或「事件（opcode 7）」更新。**
属性若无 gatt-access:notify 且插件不发 GET，其订阅值在连接期间永远不变。

opcode 7 事件是唯一可靠推送通道（8.1/8.2 每 30s 稳定到达并转发 RN）。
