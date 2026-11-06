# legacy PSK 认证（`ble_psk_*`）帧级底稿

> **状态：握手已实现（`src/miot_auth.c`），单测通过，尚未实机验证。**
> 本文件记录从米家 Android App（`com.xiaomi.smarthome`，`security/pskauth/` 包）
> 静态分析确认的事实、已落地的代码位置，以及只有真机能回答的两个问题。
>
> 用途：驱蚊器智能版（WX08ZM）/ 夜灯2 重绑 这类走 legacy PSK 的型号，
> 在**官方原版 App**下绑定（见 [clean-app-matrix.md](clean-app-matrix.md)）。
> 代码开关与实测步骤见 §6、§7。

---

## 1. 一句话结论

PSK 不是「预共享密钥 HMAC 挑战」，而是**和标准认证同构的一次 ECDH 握手**，
只是换了 HKDF 标签、换了通道类型号，并多了一步云端签名（`dev_sign`）。
**全程没有证书链**——这是它和 security-chip 门锁的本质区别，也是它可被模拟的原因。

**关键发现：设备 → App 的 5 种帧里，有 4 种本 SDK 已经在发**（见 §4.2）。
真正的增量只有一帧 `0x04 dev_sign`。所以这不是「重写一套协议」，
而是「在现有 REGISTER 状态机上加一个分支 + 换两个 KDF 标签」——
最终落地约 260 行（含 trace 与重连复用），无新依赖。

---

## 2. 加密构造（已确认）

从 `securitychipauth/Oooo000` 的 HKDF 调用点反推（`_apk_dex/d10_full.txt` 约 2928689 行）：

```
eShare = ECDH(P-256, 设备公钥, App 私钥)          // 32B
ikm    = eShare
salt   = "smartcfg-setup-salt"                    // 字面量字节，长度 18
info   = "smartcfg-setup-info"                    // 字面量字节，长度 18
out    = HKDF-SHA256(ikm, salt, info, L=64)       // dev_key(32) || app_key(32)
```

对照本 SDK 现有标准认证（`src/miot_auth.c:156`）：

```c
miot_hkdf_sha256(secret, 32, zeros16, 16, "mible-setup-info", 16, out, 64);
```

**差异只有两处**：salt 从「16 字节 0」换成字面量 `smartcfg-setup-salt`，
info 从 `mible-setup-info` 换成 `smartcfg-setup-info`。
`miot_hkdf_sha256()` 函数本身不用改。

标签族全集（App 内共 4 对，均已确认存在）：

| 用途 | salt | info |
|---|---|---|
| 绑定 | `smartcfg-setup-salt` | `smartcfg-setup-info` |
| 登录 / 重连 | `smartcfg-login-salt` | `smartcfg-login-info` |
| 分享 | `smartcfg-share-salt` | `smartcfg-share-info` |
| 主密钥 | `smartcfg-masterkey-salt` | `smartcfg-masterkey-info` |

数据面（session_key + AES-CCM）与标准认证完全一致，可原样复用。

---

## 3. 云端接口（已确认）

App 在 PSK 绑定过程中依次调用：

| 顺序 | 接口 | 参数 | 时机 |
|---|---|---|---|
| 1 | `POST /v2/device/ble_psk_auth` | `dev_pub`（设备公钥）、`io`、`oob` | 收到设备公钥后 |
| 2 | `POST /v2/device/ble_psk_bind` | `did`、`dev_sign`（设备签名）、`pdid` | 收到设备签名后 |
| 3 | `POST /v2/device/ble_psk_prov_done` | `result`、`did`、`mac` | 收到设备最终结果后 |

**关键：云端在这一步给设备建档并分配 did**，与标准认证的 `bindDidToServerV2`
是同一件事的两套接口。所以 PSK 绑定成功后设备同样有云端档案、能上云，
不是「只能本地用」。

> ⚠️ 三个都是**老接口**。是否仍在国区开放，只有实测能回答（见 §7）。

---

## 4. 连接器步骤（已确认的骨架）

### 4.1 绑定：`BlePSKAuthRegisterConnector`（App 内类 `pskauth/OooOO0`）

| 步骤 | 方法 | 方向 | 说明 |
|---|---|---|---|
| 1 | `sendRegStart` | App → 设备 | 写 26B 启动帧（含版本/能力字节） |
| 2 | `recvDeviceInfo` | 设备 → App | 通道类型 **0**，载荷同 `device info` |
| 3 | `recvDevRandom` | 设备 → App | 通道类型 **13 (0x0D)** |
| 4 | `recvDevPubKey` | 设备 → App | 通道类型 **3 (0x03)**，`dev_pub` |
| 5 | `recvDevConfirmation` | 设备 → App | 通道类型 **12 (0x0C)**，App 校验 confirm |
| 6 | `recvDeviceSignature` | 设备 → App | 通道类型 **4 (0x04)**，`dev_sign`（base64 上报云端） |
| 7 | `writeServerPub` | App → 设备 | 写 16B 服务端公钥 |
| 8 | `recvDeviceResult` | 设备 → App | 结果帧（`result`/`did`/`mac`），与标准认证共用 `A4` 结果 |
| 9 | `writeBindResultToDevice` | App → 设备 | 写 13B 绑定结果 |

补充事实：

- PSK 基类 `pskauth/OooO00o` 里有一条日志 `receive A4 result` —— **结果步骤与标准认证相同的 `A4`**，说明最后一步可以复用现有实现。
- 设备 → App 的通道类型集合是 **{0, 3, 4, 12, 13}**（`OooOOo0(I,[B)V` 的 switch 分支：0 / 3 / 4 / 12 / 13，其余走 `recv unknown package`）。
- App → 设备在收到 random 后发的帧类型是 **11 (0x0B)**（`OooOo00([B, 11, cb)`）。
- 通道读取接口为 `IBleChannelReader.onRead(String, byte[], int)`，第三个参数就是上表的通道类型。

### 4.2 客户端操作码对照（重要）

App 侧 `onRead` 收到的第三个参数就是下表「设备 → App」一列的操作码。
把它和本 SDK `src/miot_auth.c` 现有实现逐条对齐：

| 操作码 | 语义（App 侧日志） | 本 SDK 现状 | 位置 |
|---|---|---|---|
| `0x00` | `recvDeviceInfo` | ✅ 已发（`send_device_info`） | `miot_auth.c:117` |
| `0x03` | `recvDevPubKey` | ✅ 已发（64B 裸点） | `miot_auth.c:162` |
| `0x0D` | `recvDevRandom` | ✅ 已发（16B） | `miot_auth.c:171` |
| `0x0C` | `recvDevConfirmation` | ✅ 已发（32B HMAC） | `miot_auth.c:172` |
| `0x04` | `recvDeviceSignature` | ✅ 已实现（ECDSA-P256，唯一增量帧） | `psk_make_sign` |
| App → 设备 | `0x0B` appRandom / `0x0A` appConfirm | ✅ 已在 LOGIN 分支处理 | `miot_auth.c:321-325` |

也就是说：**PSK 的握手骨架 = 本 SDK 现有 REGISTER 那三帧 + 一帧签名。**
差别只在 KDF 标签（§2）和后续流程（等 16B server pub → 13B bind result，
而不是等 92B 加密 did）。

### 4.3 重连 / 重绑：`BlePSKAuthLoginConnector`（`pskauth/OooO0OO`）

Step 1 ~ Step 7，含 `recvLoginDevicePubKey`、`session_key`，KDF 用
`smartcfg-login-*` 标签。**重绑（设备已在别人账号下）走这条**，
与标准认证的 LOGIN 分支同构。

---

## 5. 只有真机能回答的 2 项（已做成可切换变体）

原先列的「3/4/12/13 分别是什么」已由 §4.2 用现有实现对齐解决。剩下的只有两项：

| # | 未知 | 为什么是硬卡点 | 怎么定 |
|---|---|---|---|
| 1 | **`0x04 dev_sign` 签什么、用什么签** | 云端拿 `dev_pub` 验签，签错必然 `ble_psk_bind` 失败。候选：对 `appConfirm` / 对 `devRand‖appRand` / 对 ECDH 共享密钥，算法可能 ECDSA-P256 或 SM2 | 读 `recvDeviceSignature` 分支（`d10_full.txt` 2914380 附近）里 `Base64.encode` 之前的入参来源；或抓一帧真签名用 `dev_pub` 验算 |
| 2 | `sendRegStart` 那 26B 的确切构成 | 决定要不要校验启动帧、以及第一帧应答时机 | 读 `sendRegStart`（`d10_full.txt` 2914540 附近）的字节赋值；或抓帧 |

**第 1 项是全项目的 go/no-go**，所以代码没把它写死成猜测值，而是做成三个
运行时可切换的候选（`miot_auth_set_psk_options` 的 `sign_variant` 参数）：

| 值 | 签名对象 | 宏 |
|---|---|---|
| 0（默认） | `devRand ‖ appRand` | `MIOT_PSK_SIGN_DEV_APP_RAND` |
| 1 | `devRand` | `MIOT_PSK_SIGN_DEV_RAND` |
| 2 | `shared ‖ devRand ‖ appRand` | `MIOT_PSK_SIGN_SHARED_RAND` |

三者都是 ECDSA-P256(SHA256)，输出 `r‖s` 64B。实测时逐个换，看 `ble_psk_bind`
哪次放行——一次真机就能定死，不需要改结构。
第 2 项（启动帧 26B 布局）代码**不解析、只原样留存并打印**，因此不影响可行性。

**关于第 1 项的一个有利事实**：`recvDeviceSignature` 分支里 App **只做
`Base64.encode` 后原样上报云端，自己不做任何验签**（`d10_full.txt` 2914360 附近）。
也就是说签名内容由「设备 ↔ 云端」的约定决定，App 不参与判定。这留下两种可能：

- 云端在**首次绑定**时只记录 `dev_pub` 与 `dev_sign`（TOFU），那就近似「发什么都行」；
- 云端会验签，那就必须签对。

两者只能用一次实测区分。**成本最低的第一次尝试**：用设备的 ECDH 私钥对
`devRand ‖ appRand`（或仅 `devRand`）做 ECDSA-P256，输出 64B 作为 `0x04` 帧发出，
看云端 `ble_psk_bind` 是否通过。这一步能直接判定属于上面哪种情况。

---

## 6. 实测步骤（固件已内置 trace，不需要任何 App 侧工具）

PSK 是 **App 主动发起**的，所以只要把收到的原始帧打出来，就能对照代码看走到哪一步。
trace 默认关闭，开一行即可：

```c
miot_auth_init(&auth, &port);
/* 想强制只走 PSK（不靠自动判定）就加这行；默认 AUTO 收到启动帧会自动切 */
miot_auth_set_mode(&auth, MIOT_AUTH_MODE_PSK);
/* trace=1 打印每帧 hex；第二参数是签名输入变体（0/1/2，见 §5） */
miot_auth_set_psk_options(&auth, 1, MIOT_PSK_SIGN_DEV_APP_RAND);
```

步骤：

1. 烧录驱蚊器档案（PID `0x040A`）
2. `idf.py -p <PORT> monitor`
3. 官方原版 App 扫描 → 添加设备
4. 串口按下面这张表读

| 串口输出 | 含义 | 下一步 |
|---|---|---|
| `PSK: start frame 26B captured` | 判定成 PSK 设备了，启动帧已留存 | 看下一行 |
| `PSK: ECDH ok` + `handshake sent (info/0D/03/0C/04)` | 五帧全发出，含签名 | 等 App 反应 |
| 只有 `(info/0D/03/0C)` 没有 `/04` | 后端没实现 `ecdsa_sign` | 补 `port` 的 `ecdsa_sign` |
| `PSK: server pub received` | App 认可握手 | 等绑定结果 |
| `PSK: *** BOUND, did stored` | **成了**，did 已落盘 | 回填矩阵，删掉「待实测」 |
| `PSK: unhandled <n>B (psk_state=…)` | 有帧没识别 | 看上面 `[psk] 0019` 那行的 hex，按长度归类 |
| 无任何 `PSK:` 行 | 没被判成 PSK | 确认 PID 是 `0x040A`；看 `[psk] 0019` 是否有 26B 帧 |
| App 报错但串口走到 `handshake sent` | 云端拒了签名 | 换 `sign_variant` 0→1→2 逐个试 |

**签名变体的试法**：`ble_psk_bind` 失败时，把 `psk_sign_variant` 换成 1 重烧，
再试 2。三次之内必定覆盖 §5 的候选集。每次只需改一个数字。

**这一步同时回答「老接口还开着吗」**：只要 App 会发启动帧，说明 App 侧这条链还在；
接口是否下线，要看走到 `ble_psk_auth` 时云端返回什么——那就已经超出设备侧能观测的范围了。

---

## 7. 实现位置（已落地）

| 部分 | 位置 |
|---|---|
| 状态机与帧收发 | `src/miot_auth.c` §`legacy PSK`（`psk_*` 系列函数） |
| 模式与开关 | `include/miot_ble/miot_auth.h`：`miot_auth_set_mode` / `miot_auth_set_psk_options` |
| 签名原语 | `include/miot_ble/miot_crypto.h` 的 `ecdsa_sign`（可为 NULL） |
| ESP32 后端 | `port/esp32/miot_port_esp32.c`：`esp_ecdsa_sign`（mbedTLS） |
| 单测 | `test/test_psk.c`（帧序列 / 模式分流 / KDF 标签），`ctest` 一条命令跑 |

设计要点：

- **两种链共存不互斥**：默认 `AUTO`，标准认证路径一行未改；PSK 由启动帧触发。
- **缺原语也不崩**：后端没有 `ecdsa_sign` 时，PSK 只是不发 `0x04` 帧并打日志，
  标准认证与已绑定的其他型号完全不受影响。
- **重连复用 LOGIN**：PSK 重连与标准 LOGIN 的挑战应答同构，只换 ikm（`psk_key`）
  与 KDF 标签（`smartcfg-login-*`），没有另写一套。
- **不确定项全是开关**：签名输入 3 变体、启动帧不解析只留证，实测改数字即可。

## 8. 相关工作与边界

- 本文只记录协议事实，不涉及任何对 App 的修改、注入或绕过。
- 不引导使用他人设备的凭据：PSK 的公私钥由设备**自己生成**，云端在绑定阶段记录，
  与标准认证的信任模型一致（首次绑定即信任）。
- 若云端已下线 `ble_psk_*`，本方案作废且无替代路径——届时应把该型号从设备清单移除，
  而不是继续维护一个不可能通过的实现。
