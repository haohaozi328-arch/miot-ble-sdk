# 拿 WiFi 灯固件当参考：Mesh 设备端怎么落到我们代码上

> 参考源：`work/esp32-wzj201-re/MESH_REFERENCE_from_wifi_lamp.md`（`nvcsmt.light.wzj201` 的
> `app0.bin` 符号实证）+ `official-findings.md`（官方文档镜像）+ `esp32/multi_dev` 现状
> 日期：2026-11-15

## 0. 结论先行

1. 米家把「网关」拆成**控制面**（上行云、联动、查询）和**承载面**（GATT/mesh 收发）两层。
   WiFi 灯两半都有；**Mesh 设备只需要承载面的从侧**，所以不必去造网关。
2. 我们已有的 `miot_spec_parse / miot_spec_encode_value` 就是 Mesh vendor model 的
   实现体 —— **同一张属性表可以吃两种承载**（spec 直连 / Mesh model），这是最省力的复用点。
3. 真正缺的只有两块：**私有配网（证书链）** 和 **key + seq 持久化**。
   可推进的实验全在「让真网关来配我们」这一侧，不在「我们去当网关」那一侧。

## 1. WiFi 固件的分层 → Mesh 的对应关系

| WiFi 固件里的符号 | 它干的事 | Mesh 里的对应物 | 我们有吗 |
|---|---|---|---|
| `mible_master_connect/_read/_write/_notify` | GATT client 会话 | provisioner / client 侧 | 不需要（网关干） |
| `mible_prph_*` + `master_opcode_send` / `opcode_send` | GATT server 上按 **opcode 分发** | Mesh model 回调（opcode 表 → handler） | 部分：`gatt_miot.c` 的 opcode 分发可直接改成表驱动 |
| `mible_beacon` / `mible_beacon_legacy` | MiBeacon 解析 | **未配网举手**（`MI_MESH_STATE_UNREG`） | 已有 MiBeacon 发送器，只差状态位 |
| `mible_net_construct_from_package_data` / `_register` / `_state_callback` | 上行到云的封装 | 由网关负责，设备不碰 | — |
| `mible_linkage_get_key` / `_check_counter` | 设备间联动 + **防重放** | net/app key + **seq number** | 缺（这是 Mesh 必须补的） |
| `mible_gateway_*` / `user.bt_gateway_enable` | 云端可开关的网关实体 | 只有网关类 PID 才有 | 拿不到（云端不开，实测无开关） |
| `mible_fastpair_*` / `mible_secure_fastpair` | 给子设备配网 | PB-ADV / PB-GATT + **私有 provision** | 缺（被证书链卡住） |
| `mible_band_handler/_parse/_send` | 小米手环 | — | 旁证：**opcode 表是数据驱动，不是硬编码** |

读法：一张表里只有第三列的后半段需要我们写代码；前两列的「会话 / 上行 / 网关」全是网关的活。

## 2. 三条可直接照搬的设计决定

**(a) 一套属性表，两种承载。**
Mesh 的 `MIOT Spec Server (0x038f0000)` 收到的是 siid/piid TLV，和 spec 直连的
`get_properties / set_properties` 载荷**结构相同**。所以 model 回调里只做搬运：

```
model_recv(payload) → miot_spec_parse() → 同一个 props 回调（已在各工程里写好）
上报 → miot_spec_build_props_changed() → model publish
```

这也解释了为什么 WiFi 灯能把 `mible_master` 复用到手环：**上层是表，下层是可换的管道**。

**(b) key 与 seq 必须落 NVS，且要和「已绑定」状态绑定。**
`mible_prph_check_counter` 的存在说明米家自己也怕重放。Mesh 的 seq 一旦回绕或重置，
网关会判重放直接丢包 —— 表现就是「设备在线但控制无响应」，很容易被误判成信号问题。

**(c) 控制面不要自己实现。**
`mible_net_*` 那一层是给「已授权网关」用的。我们若在 ESP32 上照抄，云端仍按 PID 拒绝，
这与 `nvcsmt.light.wzj201` 拿不到网关开关是同一堵墙（见参考文档结论 2）。

## 3. 缺口清单（按「代价 / 信息增益」排序）

| # | 缺口 | 怎么补 | 代价 | 成功信号 |
|---|---|---|---|---|
| 1 | 未配网举手的 mesh 状态位 | MiBeacon 里加 `MI_MESH_STATE_UNREG`（字段位置此前 `fc 58/59/5B` 实验已摸到） | 极低，1 个字段 | 米家「附近设备」出现该设备 / 网关日志出现该 UUID |
| 2 | vendor model 的 opcode 表 | 从 spec 命令 ↔ 官方文档属性表对照，先覆盖开/关/亮度这种单属性 | 低 | 网关下发 `VND rx` 且 payload 能被 `miot_spec_parse` 解析 |
| 3 | **私有 provision（证书链）** | 三条出路见 §4 | 高 | 能收到 `Prov Invite`，或明确看到 `-40 / -32` |
| 4 | net key / app key + seq | NVS 分区块，写入即 `nvs_commit`；seq 预留跳变余量 | 中 | 重连后控制仍生效（不判重放） |
| 5 | 上行/联动 | **不做**，交给网关 | 0 | — |

## 4. 私有配网这道坎，只有三条路

官方文档（`official-findings.md` §2）已写明：私有 Provision 只在官方 SDK 里，
且模组出厂烧了**证书链**，首次绑定后「证书链 + PID + MAC」三者绑定，变更即
`-32`，证书丢失即 `-40`，不可恢复。所以：

- **路 A（正）**：走小米合作渠道拿 `partner-gitlab.mioffice.cn` 账号与开发/测试证书；
- **路 B（逆向）**：把「App 发起配网」这条链抓全 —— 目标不是伪造证书，而是先看到
  `Prov Invite` 到底发不发、在证书校验前还是后被拒（`-40` 只在选模板/校验证书时报，
  能复现到 Invite 就说明校验点在后面）；
- **路 C（观察）**：小爱音箱（盲网关）对「无证书的未配网 beacon」是否也发起配网。
  这条最便宜，一次实验就能证伪。

三条路都指向同一个动作：**把自己放在「被配网」的位置上**，而不是去当配网者。

## 5. 验证阶梯（每步只留一个可观测信号，失败即停）

```
S0  广播能扫到，且 MiBeacon 带 MI_MESH_STATE_UNREG      → 见「附近设备」
S1  网关/音箱发起 PB-ADV 或 PB-GATT（串口 PROV LINK OPEN）→ 证明它认这台设备
S2  收到 Prov Invite → 之后才可能遇到 -32/-40            → 定位证书校验点
S3  provision 完成、拿到 net key（设备侧能看到 key 写入） → 真正跨过门槛
S4  model 收发通（VND rx/tx 且 spec 解析成功）           → 属性可控
```

S0/S1 之间不要跳步：**只要 S1 不出现，S2 以后的所有工作都是猜。**

## 6. 与 `-27` 的关系（Mesh 侧的坑位）

Mesh 绑定路径上的 `-27` 只可能出在 **PB-GATT / Proxy 的 notify 环节**，
判别方法与 spec 直连一致，见 `docs/troubleshooting.md` §Q11 附 2：

- 连上后**零 write** 即断 → PID 档案不是 Mesh 承载，改代码无效（`bulb_gatt` 已实测）；
- 有 write、无 notify → B1/B2（特征选错 / 没应答）；
- 有 notify、`rc=0`、App 无日志 → B3（与 CCCD 写入抢时序），首帧前留 10–20 ms。

## 7. 明确不建议

1. 在 ESP32 上复刻 `mible_gateway` 全套 —— 云端按 PID 授权，代码写到 100% 也不会放行。
2. 买「多模网关」以外的硬件来试 Mesh 1.0 —— 盲网关（小爱 Play 增强版）就够，
   中枢网关只在 Mesh 2.0 快速配网场景才必需。
3. 用别的产品的 pdid 顶替 —— `bulb_gatt` 用 `0x06EB` 的结果是 593 ms 内零 write 被踢。
