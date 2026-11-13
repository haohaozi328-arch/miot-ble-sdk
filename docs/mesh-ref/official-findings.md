# 米家 BLE Mesh 官方资料要点（2026-11-13 整理）

来源：`D:\workspace\米家资料\miot_docs_md\`（小米 IoT 开放平台官方文档镜像）+
`广嵌物联 GQ-MKM6 米家Mesh零火开关方案手册`。补充此前从 miio_open GitHub 镜像拿到的
《米家 BLE Mesh 接入介绍/产品开发》结论，本文档汇总对「S400 复刻打通 Mesh」直接相关的全部硬情报。

## 1. 协议栈与形态

- 米家 BLE Mesh = **SIG Mesh + 米家 Profile**。网关把云端 spec 命令转成 Mesh 消息发给设备，
  设备上行走 Mesh → 网关转 spec → 云端。设备端只管 Mesh 侧，翻译全在网关。
- 应用层模型：**MIOT Spec Server (0x038f0000) + Mijia Server (0x038f0002)**（米家 CID `0x038F`），
  自定义服务走 vendor 透传。属性读写回调按 siid/piid 分发（`on_property_set/get`），
  上报用 `miio_mesh_properties_changed(siid, piid, value)` / `miio_mesh_event_occurred(...)`。
- 上行过滤：网关按 interval/delta 缓存属性上报（默认 interval=86400 或 delta=1 才上报云端），
  事件不缓存立即上报。网关重启清缓存。
- **传感器品类被平台禁止用 BLE Mesh 接入**（basic_config.md 明文）——体脂秤虽在 App 显示
  「蓝牙 Mesh」，但秤品类是云端特批或非传感器归类，复刻时注意云端档案以 S400 为准（PID 0x30D9）。

## 2. 配网（关键门槛）

- 配网方式：标准 **GATT 直连配网** + **Mesh 入网**（小米私有 Provision，网关和 App 都可发起）。
  私有 Provision 的实现只在官方 SDK（partner-gitlab.mioffice.cn，需申请账号）。
- **模组 flash 出厂烧录认证证书链**：首次绑定后模组存储「证书链 + PID + MAC」关联，
  任意一项变更/丢失都会导致配网失败（错误码 **-32** PID/MAC 变更、**-40** 模板未选或证书链丢失）。
  证书链不可恢复，只能换新模组。——这是复刻设备用官方 App/网关配网的最大硬门槛。
- 功能定义必须选「产品方案」模板（固件端对应 `MI_MESH_TEMPLATE_*` 宏），否则配网报 -40；
  平台改模板后需等 ≥15min 生效。
- **设备未验收上线前，不能被小爱触屏类音箱发现和绑定**（product-dev.md）。
  非触屏音箱（如小爱 Play 增强版）规则未明说，但音箱需在米家 App「我的-蓝牙网关」中
  开启「蓝牙 Mesh 网关」功能，且音箱固件需较新（Play/Redmi Play 不能同时 Mesh 网关 + 蓝牙播放）。
- **Mesh 2.0 快速配网**：BLE 扩展广播 + 三元组双向认证，秒级配网；
  未配网状态通过 **MiBeacon 广播数据的 mesh 状态字段（MI_MESH_STATE_UNREG）** 表达
  （`mibeacon_mesh_adv_data_init(MI_MESH_STATE_UNREG)`）——证实米家 mesh 设备的
  「未配网举手」是 **MiBeacon(0xFE95) mesh 状态位**，不止 SIG Unprovisioned Beacon。
  需 100% 扫描窗口、音箱 Pro 或米家 App ≥10.6 配合，宏 `CONFIG_BT_MESH_PSK`。

## 3. 广播/在离线

- 网关 1.5min 收不到设备广播即判离线（近场）；远场（中继）42min。
- 设备掉电 90s 离线；GATT 直连期间（如 OTA）设备无法被 Mesh 网关控制。
- 发包规范：周期属性上报最小间隔 10 分钟；自定义属性 10 分钟内 ≤25 次，超限 SDK 屏蔽。
- 单网关建议 ≤50 设备；多网关时设备连最近的。

## 4. SDK 与模组（官方分支）

- 官方 SDK：`partner-gitlab.mioffice.cn/miot/bsp/`，三个分支系列：
  - **Realtek** RTL8762C/E（模组 MHCB05P-B / MHCB09P-B，`mijia_ble_mesh` 仓库 realtek 分支）
  - **Silabs** EFR32XG22（`mijia_ble_mesh` silabs_BG22 分支）
  - **Telink** TLSR8278（`miot-tlsr827x-b11s-sdk`）
- 低功耗：`MI_MESH_LOW_POWER_NODE` 宏；开启后中继关闭。
- 广嵌物联 GQ-MKM6 方案即 Telink SOC 方案（引脚 PA/PB/PC/PD 命名），基于小米蓝牙 Mesh 模组做开关。

## 5. 对本项目（ESP32-C3 复刻 S400）的含义

1. 我们已实现：MiBeacon(0xFE95) + SIG 未配网 beacon（PB-ADV/PB-GATT）+ 米家 vendor model 壳，
   Mesh 栈在 NimBLE 上跑通（commit 19d734e）。
2. **待补**：未配网 MiBeacon 的 mesh 状态位改为 `MI_MESH_STATE_UNREG` 形态（此前 fc 58/59/5B
   实验已摸到该字段位置）；真正收到 Provision PDU 后的私有配网协议（证书链认证）。
3. **硬门槛**：私有 Provision 含证书链认证，复刻设备没有 S400 的证书链。
   可能出路：a) 申请小米 GitLab 账号拿 SDK 看开发/测试证书机制；b) 逆向 App 配网路径
   （-40 只在校验模板/证书时报，先复现到能收到 Invite 才能确认在哪被拒）；
   c) 观察音箱网关是否对「无证书」的未配网 beacon 也发起配网（理论上有可能直接拒）。
4. 现状测试：固件未配网 beacon 已在发（15:20 起），`_mesh.log` 盯
   `PROV LINK OPEN` / `VND rx`；米家 App 删除重加 / 音箱重启是两个触发入口。

## 附：相关文档

- `docs/known-issues.md` §2 第 6 条：网关不连 GATT 的终局定论
- `docs/mesh-ref/08-mesh-intro.md`、`09-mesh-dev.md`：miio_open 镜像官方文档
- `docs/mesh-ref/gw3_mesh.py`：XiaomiGateway3 网关侧 mesh 数据库层解析
- 官方资料本地镜像：`D:\workspace\米家资料\miot_docs_md\`（17 篇 mesh 相关）

## 6. 移植层崩溃复盘（2026-11-13 16:20）

现象：挂上 Mesh 后设备每 ~3 分钟重启一次，断言 `npl_freertos_event_init npl_os_freertos.c:263`。

根因链：
1. IDF v6.0 freertos NPL 的 `ble_npl_event` 必须经 `ble_npl_event_init` 分配内嵌对象；
   Mesh 源码里 `static struct ble_npl_event ev = { }` 与 adv_pool 的 ev 从未 init →
   `set_arg` 静默 no-op → `net_buf_put` 的 `get_arg` 断言。
2. 修复时若"每次分配都 init"或"整池一次 init 20 个"，会吃掉事件池
   （`BLE_TOTAL_EV_COUNT` 只按 host 栈算，约 20 个对象），池空即 `BLE_LL_ASSERT(event)` 重启。
3. 正解：adv 缓冲**按需惰性 init**（每缓冲首次使用占 1 个事件）+
   事件池扩容 `BLE_TOTAL_EV_COUNT + 40`。

四个构建期补丁（`esp32/multi_dev/patch/`，见 `build_mesh.sh`）：
| 文件 | 改动 |
|---|---|
| `adv_legacy.c` | adv_update 静态事件 init 一次（guard） |
| `adv.c` | adv_alloc 惰性 init（`s_ev_ready[id]`） |
| `glue.c` | net_buf_put 自愈：arg 为空时补 init |
| `npl_os_freertos.c` | 事件池 `BLE_TOTAL_EV_COUNT + 40` |
