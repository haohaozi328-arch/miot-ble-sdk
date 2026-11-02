# 米家 BLE Spec v2 协议逆向文档

> 来源：S400 体脂秤模拟固件 + 米家 Android App 反汇编（com.miot.spec 包）+ 空中抓包互证。
> 所有格式均已实测验证（App 端 hook 观察到解析成功与广播转发）。

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

### 4.2 ⚠️ 三个决定性的格式细节（踩坑实证）

1. **op=7 事件每个参数前必须带 16 位 piid**
   缺失时 App 端 `ud5` 解析器返回 NULL，事件全部被丢弃（v62c 之前 75 包全 NULL）。
2. **op=4 properties_changed 帧体首字节是 count**
   `gyb` 解析器首先读 count；把 siid 当首字节会被解析成 count=11 的残包。
3. **op=6 action 应答只有 respCode16，不能带 siid/aiid**
   `kt4` 解析器读 op 后的前两个字节当 respCode。带 `[siid][aiid]` 会被误读为
   `respCode=269`（LE `0d 01`）→ App 判定动作失败 → 无限重发用户资料。

### 4.3 value 编码

`type16 = (type << 12) | len`，16 位字段一律小端。

| type 码 | 类型 |
|---------|------|
| 1 | bool |
| 2 | uint8 |
| 3 | uint16 |
| 4 | uint32 |
| 5 | int32 |
| 9 | float |
| 10 | string |

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

调试建议：hook `SpecManager` 日志与解析入口（观察 `EventOccuredPacket` 是否生成），
再对照插件 JS bundle 的 topic 匹配表。

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
