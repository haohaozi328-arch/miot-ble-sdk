# 排障决策树 —— 官方原版 App，无任何模块

> 前提：手机是国区小米账号，米家 App 为官方原版（未 root、未装任何模块），
> 设备固件由本 SDK 构建。**不适用**于"改 App 绕过"的场景，本仓库也不提供这类手段。

---

## Q1. 添加设备时红字「手机网络异常」/「设备绑定账号失败 (104,did=…)」

**不是网络问题。**

配网页三个步骤的含义：

| 步骤 | 走的通道 | 通过意味着 |
|---|---|---|
| 连接设备成功 | BLE GATT（本地） | 广播与 GATT 服务结构正确 |
| 普通认证成功 | BLE 本地认证 | 设备端协议实现正确 |
| 设备绑定账号失败 | **云端**（要联网） | App 拿 did 去云端查产品档案，查不到 |

米家把第 3 步的所有云端失败统一渲染成「手机网络异常」，真实原因在括号里。
服务端原始响应形如：

```json
{"code":-8,"message":"cant find did <did>","result":null}
```

### 为什么会查不到

本 SDK **不设置 did**。BLE 设备的 did 由云端在 REGISTER 阶段通过 92B 加密帧下发
（`docs/protocol.md` §7），固件解密后存 NVS 再回传。所以只可能是下面几种情况：

| 情况 | 判断方法 | 处理 |
|---|---|---|
| 固件自己填了 did | did 不是 `blt.3.*` 形态 | 删掉硬编码，did 只能由云端下发 |
| model/PID 与档案不符 | 特别是国区/海外 PID 混用 | 用档案里的真实 PID（温湿度计3 mini 国区 `0x55B5`，海外 `0x5BEA` 扫不到） |
| 92B 帧没解开 | 串口出现 `REGISTER: encrypted did decrypt FAIL` | 记录 App 版本 + 该行日志反馈；属版本差异 |
| 该 did 已被别的账号绑定 | 响应通常是"已绑定"类错误，不是 cant find did | 解绑或换设备 |

### 不要做

换网络、重启路由器、清 App 缓存、重装 App —— 前两步都过了，这些一律无效。

---

## Q2. App 扫不到设备

按顺序查：

1. **广播里有没有 PID**（温湿度计2 = `0x055B`，字节序 `5B 05`）。
   App 是靠 PID 匹配云端产品库来决定显示哪台设备的，没有 PID 就没有卡片。
2. **绑定前的 FC 是否是 `30 58`**：
   - `0x30` = MAC(0x10) + Capability(0x20)，未加密
   - `0x58` = version 5 + authMode 2（标准认证）+ registered 0
   写成加密形态（`0x58`）或漏掉 Capability，App 会当成"已绑定/不支持"而忽略。
3. **广播有没有带 MAC**：iOS 与部分安卓机型按帧内 MAC 做归属判定。
4. **长度**：`flags(3) + [len][0x16][95 FE](4) + 帧` 总计 ≤ 31B。
   串口若出现 `adv_set_fields rc=4`，就是超长（`4` = `BLE_HS_EMSGSIZE`）。
5. **GATT 服务是否注册了 `0xFE95`** 与 `0x0010/0x0019/0x001A`：扫到但连不上多半是这里。

---

## Q3. 扫到了，一直「正在连接」然后超时

九成是 Q2 第 4 条的后果：`adv_set_fields` 失败后仍处于"广播已停止"状态。

正确做法：

- 帧超预算就拆帧（一个对象一帧），不要停广播；
- `set_fields` 失败时用最小帧头重建，**绝不能留在 stop 状态**；
- 加一个 5s 定时兜底：`!ble_gap_adv_active()` 就重新拉起。

---

## Q4. 绑定过程中 App 断开，串口无 device info

`device info sent (did=0B)` 这行没出现，或出现得很晚：

- `miot_auth_prepare()` 没调用，或放在了 `0x0015` 写回调里。
  ESP32-C3 生成 P-256 密钥对约 **390ms**，放在回调里会让 device info 迟到，
  App 以 `-51` 断开。必须在 `app_main` 里预生成。
- 检查 `miot_crypto_set_ops()` 是否在 `miot_auth_prepare()` 之前调用。

---

## Q5. 绑定成功，但设备页没有数值 / 数值不刷新

| 检查 | 期望 |
|---|---|
| 属性 siid/piid | 直接用 `thermometer_init()`，不要自造；温湿度计2 是双 spec 兼容（温度 float 2.1001 / 3.1001） |
| 是否 LOGIN 成功后再上报 | 串口要先有 `STD LOGIN COMPLETE`，否则 `send_props_changed` 返回 -1 |
| 上报后有没有 get 回读 | 串口出现 `spec req` 说明链路通 |
| 是否连了网关 | 走手机直连时只在设备页可见；网关首报要等一轮轮询 |

---

## Q6. 「未连接蓝牙网关」

环境侧问题，不是固件问题：插拔网关电源 + 重启米家 App，通常恢复。
多台设备都复现过（见 `docs/testing.md` 结果汇总表）。

---

## Q7. 断开重连后要求重新绑定 / 重启后设备丢了

`miot_auth_port_t` 的 `save` / `load` 没实现。把
`miot_auth_identity_t`（含 `did`、`token`、`bind_key`、`dev_key`）写进 NVS，
开机 `load` 回来即可。参考 [AGENTS.md](../AGENTS.md) S3.1。

---

## Q8. `LOGIN: app confirm MISMATCH`

`devConfirm` 的重发机制没生效。要求：首发延后 120ms，之后每 400ms 补发，最多 4 次
（`miot_auth_poll()` 每 50~100ms 调一次）。少发会让 App 偶尔收不到 `0x0C`，
15s 后按 notify timeout 断开。

---

## Q9. 想换别的设备档案

先看 [clean-app-matrix.md](clean-app-matrix.md)。有些型号走 legacy PSK 认证，
本 SDK 未实现 PSK，**官方原版 App 下绑不上**——这不是 bug，也不是靠调参能解决的。
