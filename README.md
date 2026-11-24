# miot-ble-sdk

> ⚠️ **本项目仅供学习与研究，禁止商用**（详见 [LICENSE](LICENSE)）。
> 协议格式来自对米家 App 与 BLE 通信的独立逆向研究，仅作互操作性技术记录。
> **与小米公司无关联，未获授权或背书**；`MIoT`/`米家` 为其商标，此处仅作描述性指称。
> 使用前请阅读 [NOTICE.md](NOTICE.md)（用途边界与禁止行为）。
>
> **本项目不含、也不需要任何 hook / Xposed / Magisk / LSPatch 模块，不修改、不注入
> 米家 App。** 设备能否被接受，只取决于设备端协议是否正确；用户路径上只有
> **官方原版 App**。仓库内已含可编译烧录的 ESP32 工程
> （[`esp32/multi_dev`](esp32/multi_dev)，三条命令烧完），无需任何模块。

米家（MIoT）BLE Spec v2 设备端 SDK —— 从实测互通验证的 S400 体脂秤模拟固件中抽象而来。

核心价值：**所有协议格式均通过米家 App 反汇编（com.miot.spec 包 ud5/gyb/kt4 解析器）+ 空中抓包互证实测验证**，包括多个公开资料从未记载、决定 App 能否解析帧的关键格式细节（见 [docs/protocol.md](docs/protocol.md) §4.2）。

```
┌──────────────────────────────────────────────────────────┐
│  你的产品逻辑（体重计 / 温湿度计 / 灯 / 任意 MIoT 设备）      │
├──────────────────────────────────────────────────────────┤
│  miot_profiles 已验证设备档案（29 台设备，见下方设备清单）     │
│  miot_beacon   MiBeacon v5 加密广播帧（不建连接的传感器）      │
├──────────────────────────────────────────────────────────┤
│  miot_device   组合层：加密通道+分派+属性表+主动上报           │
│  ├─ miot_spec   Spec v2 明文帧编解码（纯函数，可host测试）     │
│  ├─ miot_props  属性表驱动执行器（get/set 自动处理）           │
│  ├─ miot_ag2    ag2 可靠传输层（START/ACK/DATA 分帧）         │
│  └─ miot_crypto 加密抽象（RC4/HKDF/ECDH/AES-CCM，后端可替换）  │
├──────────────────────────────────────────────────────────┤
│  miot_auth     标准认证（REGISTER 绑定 + LOGIN 会话密钥）      │
├──────────────────────────────────────────────────────────┤
│  miot_port  移植层接口（notify / 时间 / RNG / 日志）           │
├──────────────────────────────────────────────────────────┤
│  你的平台：ESP32+NimBLE / Zephyr / Linux BlueZ / …            │
└──────────────────────────────────────────────────────────┘
```

## 特性

- **零依赖核心**：纯 C11，不依赖 OS、BLE 协议栈、加密库
- **加密后端可替换**：`miot_crypto_ops_t` 函数指针对接 mbedTLS / OpenSSL / 任意实现（参考 [port/esp32](port/esp32)）
- **host 单元测试**：用空中抓包实证帧做黄金样本，`cmake && ctest` 即可验证
- **表驱动属性**：注册一张 `miot_prop_t` 表，get/set 自动处理（类型转换/范围夹取/权限校验/副作用回调）
- **完整认证链路**：REGISTER（ECDH P-256 + 绑定）与 LOGIN（纯对称 HKDF）状态机全部内置

## 目录结构

```
include/miot_ble/   SDK 公共头（auth / device / spec / props / ag2 / crypto / beacon / profiles / port）
src/                对应实现（纯 C11，无 OS 依赖）
port/esp32/         ESP32 + NimBLE + mbedTLS 移植参考
examples/           每台设备一个可运行演示（host 编译，打印帧/解析结果）
test/               黄金样本单测（样本即真机抓包）
tools/              build_all.sh（全量编译门禁）/ read_serial.py / serial_tap.py
docs/               protocol.md（协议）/ testing.md（逐设备实机验收 + 踩坑）
docs/mesh-ref/      BLE Mesh 参考资料 + provisioning/（配网协议逆向笔记 7 篇）
tools/mesh/         mesh 侧工具：多帧重组熵分析 / btsnoop 解析 / 私钥派生验证
```

## 快速开始

### 只想验证协议（不需要硬件）

```bash
cmake -B build && cmake --build build && ctest --test-dir build
./build/example-thermometer    # 打印一帧 properties_changed
```

### 想烧到板子上（自带工程，三条命令）

仓库里已经有一个**可直接编译烧录**的 ESP-IDF 工程：[`esp32/multi_dev`](esp32/multi_dev)，
内置 12 台设备的档案（默认启动体脂秤 S400），`sdkconfig.defaults` 已把目标芯片、
NimBLE、mbedTLS、栈大小等全部预设好，克隆下来直接 build 就行，不需要改配置。

装好 ESP-IDF **v6.0**（`idf.py --version` 能出版本号），然后：

```bash
cd miot-ble-sdk/esp32/multi_dev
idf.py build                            # 首次约 3~5 分钟
idf.py -p COM5 flash monitor            # Windows 写 COM5，macOS/Linux 写 /dev/tty.usbmodem*
```

就这三条。烧完串口会每 5 秒打印一次运行状态，接下来在官方米家里「添加设备」：

```
I (405) MIOT: host synced, 米家体脂秤 S400  PID=0x30D9  MAC 10:00:3B:CE:42:03  bound=0
```

`bound=0` → 设备处于未绑定广播状态，App 能扫到；绑定成功后变 `bound=1`，
App 建立会话后 `login=1`。三态的含义见 [docs/troubleshooting.md](docs/troubleshooting.md)。

**换一台设备**：串口发 `dev` 看 12 个序号，`dev 1` 切到温湿度计2，设备会擦身份自动重启。
**板载 BOOT 键**：短按 = 触发一次称重（等价于真机上「人踩上去」）；长按 5 秒 = 清 NVS 恢复出厂。

**只想要一台设备的最小工程？** 不用动 multi_dev —— 见下方 [设备清单](#设备清单) 的「工程」列，
每台设备都有独立工程（`app_main.c` + `gatt_miot.c`，不含 Mesh 栈，`idf.py erase-flash flash monitor` 直接烧）。
骨架同构（`thermo_gatt` 注释最全、最适合当模板；`door_adv` / `motion_adv` 是最小的小白版），
换型号只改三处：**PID、spec 属性表、广播对象号**。

人类一页纸版本：[QUICKSTART.md](QUICKSTART.md)；让 AI 代劳的版本：[AGENTS.md](AGENTS.md)。

绑定 + 会话建立时，串口按这个顺序打印（每一行都对应一个协议阶段，缺哪行就卡在哪）：

```
ECDH keypair ready                       ← 开机预生成 P-256 密钥
REGISTER COMPLETE (result 0x11 on 0x0010) ← App 下发的绑定请求已被接受
REGISTER: did stored (utc=...)            ← 身份写入 NVS，重启也不丢
*** STD REGISTER: bind result OK (bound=1) ***
*** STD LOGIN COMPLETE (0x21 on 0x0010) *** ← 会话密钥就绪，spec 通道打通
*** session established: spec channel ready ***
```

遇到问题先看 [docs/troubleshooting.md](docs/troubleshooting.md)，
别改 App、别装模块——那不在本项目的路径上。

### 最小设备代码

```c
#include "miot_ble/miot_profiles.h"

static int notify_0010(void *u, const uint8_t *d, size_t n) { return ble_notify(0x0010, d, n); }
static int notify_0019(void *u, const uint8_t *d, size_t n) { return ble_notify(0x0019, d, n); }
static int spec_notify (void *u, const uint8_t *d, size_t n) { return ble_notify(0x001A, d, n); }

void app_main(void) {
    miot_crypto_set_ops(&my_crypto_ops);        // 5 个加密原语，参考 port/esp32

    /* 认证层（0x0010 + 0x0019） */
    static miot_auth_t auth;
    profiles_wire_auth(&auth, NULL, notify_0010, notify_0019, NULL);
    miot_auth_prepare(&auth);                   // 开机预生成 ECDH（要 ~390ms）

    /* 设备层（0x001A）：注册档案后 spec 全自动 */
    static miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    scale_s400_init(&dev, &port, &tbl, &cnt);

    /* GATT 写回调转发 */
    void on_0010_write(d, n) { miot_auth_on_0010(&auth, d, n); }
    void on_0019_write(d, n) { miot_auth_on_0019(&auth, d, n); }
    void on_001A_write(d, n) { miot_device_on_rx(&dev, d, n); }
    void on_ble_disconnect(void) { miot_auth_on_disconnect(&auth); }

    /* 主循环 50~100ms 一次（LOGIN devConfirm 重发） */
    miot_auth_poll(&auth, now_ms());

    /* LOGIN 成功后接通 spec 会话密钥 */
    if (miot_auth_is_logged_in(&auth))
        miot_device_set_session_keys(&dev, miot_auth_session_keys(&auth));

    /* 测量完成 → 一行代码上报（App 实测解析通过） */
    scale_s400_report(&dev, 64500 /*0.01kg*/, 500 /*0.1Ω*/, unix_ts());
}
```

## 设备清单

> **范围说明**：下表 29 台全部是 **BLE GATT 直连**形态——手机 ↔ 设备建立连接后直连通信，
> 设备在米家 App 里可见、可绑定、可读写属性，但**不支持云端下发**（手机不在附近就控制不了）。
> 需要「网关转发 / 小爱语音 / 远程控制」的是 **BLE Mesh** 形态，那是另一套配网与身份校验，
> 本 SDK 不覆盖，协议分析与实测边界见 [docs/mesh-ref/provisioning/](docs/mesh-ref/provisioning/)。
> 两者的完整对比见 [docs/mesh-ref/README.md](docs/mesh-ref/README.md)。

29 台设备，全部在米家 Android App 上做过实机验证（扫描添加 → 绑定 → 数据互通），仅 24 号（LED 灯泡）未通过、保留作协议研究记录：

- ✅ 实测通过　⚠️ 能用但带已知问题　❌ 未打通（保留作协议研究记录）
- 能否用**官方原版 App（无模块、未 root）**添加，逐条对照 → [docs/clean-app-matrix.md](docs/clean-app-matrix.md)
- 逐设备回归步骤与踩坑 → [docs/testing.md](docs/testing.md)

| # | 设备 | 型号 / spec | PID | 通道 | 状态 | 工程 |
|---|------|-------------|-----|------|------|------|
| 1 | 米家体脂秤 S400 | MJTZC01YM<br>`yunmai.scales.ms103` | `0x30D9` | spec | ✅ 实时页 + 身体成分报告 + 云端历史 | [`scale_adv`](esp32/scale_adv) · [`scale_s400`](examples/scale_s400) |
| 2 | 蓝牙温湿度计 2 | LYWSD03MMC<br>`cgllc-cgdk3` | `0x055B` | spec + 广播 | ✅ 2026-11-07 纯净 App 实测，页面实时读数 | [`thermo_gatt`](esp32/thermo_gatt) · [`thermometer`](examples/thermometer) |
| 3 | 夜灯 2 | MUE4094RT | `0x03DD` | 标准首绑 + NUS 控制 | ⚠️ 可用，3 个已知问题见下 | [`nightl2_adv`](esp32/nightl2_adv) · [`night_light`](examples/night_light) |
| 4 | 驱蚊器 智能版 | WX08ZM | `0x040A` | legacy PSK | ⚠️ 编译通过，绑定未实测 | [`wx08zm_gatt`](esp32/wx08zm_gatt) · [`repeller`](examples/repeller) |
| 5 | 米家无线开关（双键） | XMWXKG01YL<br>`yeelink.remote.contrl` | `0x1949` | 事件 | ✅ 绑定 + 单击/双击上云 | [`btn2_adv`](esp32/btn2_adv) · [`button_event`](examples/button_event) |
| 6 | 小米无线开关（单键） | XMWXKG01CM | `0x2387` | 事件 | ⚠️ 绑定 + 事件可用，待纯净复核 | [`btn1_adv`](esp32/btn1_adv) · [`button`](examples/button) |
| 7 | V5 无线八键开关 | `giot.remote.v58kwm` | `0x3F3A` | 事件（盲网关） | ✅ 单击/双击/长按全上云 | [`v5kwm_adv`](esp32/v5kwm_adv) |
| 8 | 甲醛检测仪 | JQJCY01YM | `0x02DF` | 广播 | ✅ 2026-11-14 添加一次过，重启仍绑定 | [`hcho_adv`](esp32/hcho_adv) · [`sensor_hcho`](examples/sensor_hcho) |
| 9 | 门窗传感器 2 | MCCGQ02HL | `0x098B` | 广播 | ✅ 开/关门事件上云 | [`door_adv`](esp32/door_adv) · [`sensor_door`](examples/sensor_door) |
| 10 | 门窗传感器 3 | `xiaomi.magnet.03` | `0x8C05` | 广播 | ✅ 2026-11-17 绑定 + 开合/电量上云<br>对象号按公式算（`0x480C`），电量是枚举非百分比 | [`door3_adv`](esp32/door3_adv) |
| 11 | 人体传感器 2 | RTCGQ02LM | `0x0A8D` | 广播 | ✅ 有人/无人 + 光照上云 | [`motion_adv`](esp32/motion_adv) · [`sensor_motion`](examples/sensor_motion) |
| 12 | 人体传感器 2S | XMPIRO25XS<br>`xiaomi.motion.pir1` | `0x3531` | 广播 + 确认配对 | ✅ 移动/光照上云<br>绑定需保持「请求配对」广播，否则 `-28` | [`pir2s_adv`](esp32/pir2s_adv) |
| 13 | 人体存在传感器 | XMOSB01XS<br>`xiaomi.sensor_occupy.03` | `0x4683` | 广播 | ✅ 存在状态/光照上云 | [`occ_adv`](esp32/occ_adv) |
| 14 | 水浸传感器 | SJWS01LM | `0x0863` | 广播 | ✅ 漏水事件上云 | [`leak_adv`](esp32/leak_adv) · [`sensor_leak`](examples/sensor_leak) |
| 15 | 温湿度计 3 mini | MJWSD06MMC | `0x55B5` | 广播 | ✅ 温/湿/电量轮播<br>国区 PID；海外 `0x5BEA` 国区扫不到 | [`thermo_t3m`](examples/thermo_t3m) · `multi_dev` 档案 `T3M` |
| 16 | 温湿度计 3 Pro | MJWSD07MMC | `0x9AB9` | 广播（按键唤醒） | ✅ 温/湿/CO₂ 三数同出，小爱可查 | [`t3pro_adv`](esp32/t3pro_adv) |
| 17 | 燃气传感器 | `mirsz.sensor_gas.ga500` | `0x392D` | 广播 | ✅ 浓度上云 + 场景页自动出「燃气超标」 | [`gas_adv`](esp32/gas_adv) |
| 18 | 烟雾传感器 | `mirsz.sensor_smoke.s500` | `0x392E` | 广播 | ✅ 火警上云 | [`smoke_adv`](esp32/smoke_adv) |
| 19 | 无线智能宠物饮水机 | `xiaomi.pet_waterer.002` | `0x399A` | 广播 | ✅ 缺水/滤芯/电量三属性双向<br>三对象同帧超 26B，必须逐帧轮播 | [`petwater_adv`](esp32/petwater_adv) |
| 20 | 夜灯 2 蓝牙版 | `yeelink.light.nl1` | `0x07F6` | 广播 | ✅ 无人时长双向（标准 spec，非私有） | [`nl1_adv`](esp32/nl1_adv) |
| 21 | 领普压力有无传感器 | `linp.senpres.ps1bb` | `0x3F4C` | 广播 | ✅ 有无压力 + 电量双向 | [`senpres_adv`](esp32/senpres_adv) |
| 22 | 花花草草监测仪 | `hhcc.plantmonitor.v1` | `0x0098` | 广播 | ✅ 土壤湿度/肥力/光照/气温四槽<br>土壤湿度用 `0x1008`，别用 `0x1006` | [`plant_adv`](esp32/plant_adv) |
| 23 | 空气检测仪（树色） | `shuse.airm.ss006` | `0x6651` | 广播 | ✅ 温/湿/AQI/CO₂/HCHO/TVOC 六属性双向<br>HCHO·TVOC 官方 range [0,2]，超范围打坏读口 | [`airm_adv`](esp32/airm_adv) |
| 24 | LED 灯泡（蓝牙 Mesh 版） | MJDP003 | `0x06EB`（推断） | Mesh spec | ❌ `-27`，拒绝点在 PID 类型层 | [`bulb_gatt`](esp32/bulb_gatt) |
| 25 | 烟感卫士 2 | `xiaomi.sensor_smoke.yg2` | `0x7ABA` | 广播 | ✅ status/电量双向<br>status 是 enum(1 正常/2 告警)，不是 bool；MAC 撞号会被米家按地址去重过滤 | [`smoke2_adv`](esp32/smoke2_adv) |
| 26 | 小米水浸卫士2 | `xiaomi.flood.oh83w` | `0x6375` | 广播 | ✅ 未浸水/未淋水/电量三属性双向<br>新 eid 0x4806/0x487B/0x4C03 | [`flood2_adv`](esp32/flood2_adv) |
| 27 | 闪极 100W 迷你充电器（蓝牙版） | `sharge.plug.sj01` | `0x1E1C` | spec 直连 | ✅ 开关 set→回推闭环<br>全库唯一 BLE 插座，控制走手机 GATT 不经网关 | [`outlet_adv`](esp32/outlet_adv) |
| 28 | 米家声波电动牙刷 T501 | `soocare.toothbrush.1501` | `0x1FF3` | 广播 | ✅ 电量/充电/刷头三属性双向<br>仓库首个牙刷品类；手环走 MiWear 私有栈不可做 | [`toothbrush_adv`](esp32/toothbrush_adv) |
| 29 | 米家伺服扫振电动牙刷 Pro | `xiaomi.toothbrush.24p` | `0x6800` | 广播 | ✅ 电量/充电/刷头三属性双向<br>旗舰款，UI 更丰富（本周刷牙/覆盖率/参数设置）；刷头范围 0~100 | [`brushpro_adv`](esp32/brushpro_adv) |
| 30 | 小米汽车磁吸补光灯 | `careco.nlight.o654` | `0x6266` | spec 直连 | ✅ 开关/亮度/色温/模式/对时 set→回推全闭环<br>仓库首个可调光+调色温本地控制类；亮度/色温滑块拖动实时 SET，控制走手机 GATT 不经网关 | [`carlight_adv`](esp32/carlight_adv) |
| 31 | PTX 无线开关（蓝牙版） | `090615.remote.btsw1` | `0x38BB` | 广播 | ✅ 绑定 + 单击/双击/长按/低电量四事件上报<br>事件对象号 0x4E0C/0x4E0D/0x4E0E/0x4C01；电量 ≤15% 快广播补发低电事件 | [`ptxsw1_adv`](esp32/ptxsw1_adv) |
| 32 | 米家恒温电水壶 Pro | `yunmi.kettle.v13` | `0x1809` | spec 直连 | ✅ 17 属性全读 + 档位/时长/水温双向<br>★3.3 官方标 R 但 App 当可写下发，必须注册 RW 并联动水温；水温由传感器回推 | [`kettle_adv`](esp32/kettle_adv) |
| 33 | 米家智能香氛机-杯托版 | `xiaomi.diffuser.xwczxf` | `0x39E6` | spec 直连 | ✅ 11 属性全读 + 浓度/香型双向<br>★官方标 R 但 App 当可写下发，全部注册 RW；浓度 u16 val=150=5档 | [`diffuser_adv`](esp32/diffuser_adv) |
| 34 | 小米磁吸散热背夹 | `xiaomi.cooler.r02p` | `0x52AA` | spec 直连 | ✅ 14 属性全读 + 模式/温度/灯效全控<br>★修复 SDK 两坑：I8 编码缺失致温度 `--`；BOOL SET 新格式帧被旧格式分支误判 rc=-4 卡死延时开关 | [`cooler_adv`](esp32/cooler_adv) |
| 35 | 米家标签打印机 | `xiaomi.printer.label` | `0x34FE` | spec 直连 | ✅ 7 属性 + 3 action；设备页完整渲染（素材广场/打印历史/按键标签）<br>★首个 string 属性设备，on_get/on_set 兜底抓取打印内容；打印图片走私有 BLE 指令未覆盖 | [`printer_label_adv`](esp32/printer_label_adv) |

**入门推荐**：蓝牙温湿度计 2 —— 标准认证、免网关、属性可验证，链路注释最全，最适合当模板改新型号。

### 三种通道（决定「能不能绑、数据怎么走」）

| 通道 | 建连接？ | 绑定 / 数据路径 | 对应设备 |
|---|---|---|---|
| **spec 连接型** | 是 | 标准认证（ECDH P-256 REGISTER + HKDF LOGIN）→ spec v2 通道收发 | 1 体脂秤 · 2 温湿度计 2 · 3 夜灯 2 · 4 驱蚊器 |
| **广播型** | 否（只在绑定时连一下） | MiBeacon v5 加密广播 → 网关解密上云 | 8~23 全部传感器 / 灯 / 家电类 |
| **事件型** | 可连（仅用于绑定） | 按键事件走加密广播，不进 GATT | 5 双键 · 6 单键 · 7 V5 八键 |

### 五个必踩的坑

| 坑 | 现象 | 出处 |
|---|---|---|
| **MAC 必须派生** | 直接烧物理 MAC，米家去重后扫不到（像是「设备已被添加」） | 各 `esp32/*_adv` README |
| **对象号别照抄同族** | 用错对象号＝能绑、能建自动化，但数据/按键静默丢弃。新式 spec 一律按公式算：`eid = 0x4000 \| (siid<<10) \| (piid-1000)` | [testing.md §23](docs/testing.md) · 门磁3 / 燃气 / 烟雾 / 压力 |
| **载荷类型比对象号更隐蔽** | 电量发 95% 被网关整帧丢弃（该属性是枚举 0/1）；类型错时云端恒 null，不报错 | [testing.md](docs/testing.md) 汇总表 #26（门窗 3） |
| **一帧一对象** | 多属性塞同一帧超 26B → `rc=-2`；三槽/四槽设备必须逐帧轮播 | 宠物饮水机 / 花花草草 README |
| **栈大小 / GATT 缓存** | `scale_adv` 需 `HOST_TASK_STACK_SIZE=8192`；`nightl2_adv` 重烧后手机须关/开蓝牙，否则 `status 133` | 对应工程 README |

另：**验证时先关掉 App 设备页**——页面打开会占住 BLE 连接，网关收不到广播，会误判「设备不上报」。

`multi_dev` 一个工程内置 **12 个档案**（串口 `dev` 查看、`dev N` 切换，切档自动擦身份重启），
适合对比同一套骨架在不同 PID 下的差异；上表 29 台里除 15 号（走 `examples/`）外都有独立工程。

> ⚠️ 两台带已知问题、**不要当「开箱即用」演示**：**夜灯 2**（详情页偶发超时 / 删设备重加前必须
> 先 `factory` / 无「如果」触发条件）与 **驱蚊器智能版**。完整记录见
> [docs/known-issues.md](docs/known-issues.md)、[docs/nightl2-ag2-handoff.md](docs/nightl2-ag2-handoff.md)。

## 已知边界（明确未适配）

| 项 | 结论 | 原因 |
|----|------|------|
| S400 抱婴称重 | ❌ 未适配 | 固件未实现婴儿称重语义（低量程/去皮与对应事件字段），本轮不做 |
| S400 平衡测试 | ❌ 未适配 | 未实现站姿/重心偏移数据 |
| 驱蚊器定时任务 | ❌ 未适配 | 固件对 `0x0003 TIMER_ID` / `0x0004 TIMER_CONTENT` 只记录不落表，`0x0006 TIMER_INDEXS` 恒返回 11×`0xFF`（空槽）；且设备无 RTC，`0x0005 LINUX_STAMP` 未用于走时 |
| **legacy PSK 绑定**（驱蚊器 / 夜灯重绑 / 部分老型号） | ⚠️ 已实现·未实测 | 与标准认证**同构**（同一 ECDH P-256，换 `smartcfg-*` HKDF 标签，多一帧 `0x04 dev_sign`）。代码在 `miot_auth`（`MIOT_AUTH_MODE_PSK`，默认 `AUTO` 由启动帧自动判定），`test/test_psk.c` 覆盖状态流转。**缺的是真机验证**，共三个未知：① App 扫描阶段认哪种广播形态（已做候选表轮播探测，[psk-auth.md](docs/psk-auth.md) §6.1）② `dev_sign` 签什么 ③ 云端 `ble_psk_*` 是否仍开放 → 实测结果请用 [PSK 实测 issue 模板](.github/ISSUE_TEMPLATE/psk-field-test.yml)，串口日志必填 |
| 「手机网络异常」/ `设备绑定账号失败 (104,did=…)` | ⚠️ 见 FAQ | 云端查不到该 did 的产品档案（自造 did、PID 不符、或 92B 帧未解出）。**与网络无关**，见 [docs/troubleshooting.md](docs/troubleshooting.md) Q1 |
| 门锁 / security-chip 类 | ❌ 硬墙 | 云端强制 X.509 芯片证书链（`securityChip=1`），无法借壳 |
| **BLE Mesh 形态**（要云端下发 / 远程控制） | ❌ 硬墙·另开一路 | mesh 配网要求设备用**私钥**签 `pkg4`，服务器验签；私钥产线随机烧录、空口不泄露、派生假设已证伪。协议细节与实测边界见 [docs/mesh-ref/provisioning/](docs/mesh-ref/provisioning/)，要做 mesh 产品只能走官方模组（自带合法证书） |
| 「未连接蓝牙网关」提示 | ⚠️ 环境侧 | 网关/云端设备缓存问题，插拔网关 + 重启 App 可解；固件侧无拒连记录（多台复现） |
| 3 Pro「未连接手机」/ 手机连不上 | ℹ️ 正常设计 | 真机 3 Pro 是**按键连接模式**：平时不发可连接广播，只有按键后才置 `solicited=1` 开 120s 唤醒窗口。App 平时显示「未连接手机」是常态，数据走广播→网关，**不依赖手机连接**；`esp32/multi_dev` 用 BOOT 键等价模拟按键 |
| App 打开设备页即重启（`MEPC=0`） | ✅ 已修 | `MIOT_SPEC_MAX_ARGS=6` 装不下 3 Pro 一次 GET 的 8 个属性，越界写覆盖返回地址。现为 12，且解析层强制钳制 `argc`。详见提交 `fix(spec): App 读属性即重启` |

## 接口一览

| 模块 | 职责 | 关键 API |
|------|------|----------|
| `miot_port` | 移植层回调 | `send_notify` / `time_now` / `random` / `log` |
| `miot_auth` | 标准认证 | `init` / `prepare` / `on_0010` / `on_0019` / `poll` / `on_disconnect` |
| `miot_device` | 组合层 | `init` / `attach` / `set_session_keys` / `on_rx` / `send_event` / `send_props_changed` |
| `miot_spec` | 帧编解码 | `build_event` / `build_props_changed` / `build_get/set_reply` / `build_action_reply` / `parse` |
| `miot_props` | 属性表 | `miot_prop_t` 注册表 + `find` / `encode` / `apply` |
| `miot_ag2` | 可靠传输 | `rx` / `send`（18B 分帧，512B 重组） |
| `miot_crypto` | 加密抽象 | `set_ops`（hmac/ccm/ecdh 5 原语）+ rc4/mixA/mixB/hkdf/ccm 工具 |
| `miot_beacon` | 广播帧 | `miot_beacon_frame_v5` + 对象编码（`miot_beacon_obj_*`），覆盖广播型传感器 |
| `miot_profiles` | 设备档案 | 12 个档案的 init/report 函数（见[设备清单](#设备清单)） |

## 已验证的运行环境

- ESP32-C3 + NimBLE + mbedTLS（S400 体脂秤模拟器，与米家 Android App 8.x 全流程互通：
  扫描添加 → 绑定（REGISTER）→ 设备初始化 → 登录（LOGIN）→ 用户资料推送 →
  spec 读写 → 事件上报 → App 端解析成功并广播到插件）

## 文档

- [AGENTS.md](AGENTS.md) —— **给 AI 的执行手册**：装环境 → 建工程 → 编译烧录 → 验收
- [QUICKSTART.md](QUICKSTART.md) —— 人类一页纸
- [docs/troubleshooting.md](docs/troubleshooting.md) —— 排障决策树（104 / 扫不到 / 连不上 / 无数据）
- [docs/clean-app-matrix.md](docs/clean-app-matrix.md) —— 官方原版 App 可绑定矩阵（标准认证 vs PSK）
- [docs/psk-auth.md](docs/psk-auth.md) —— legacy PSK（帧格式 / 已实现的状态机 / 实测开关）
- [docs/protocol.md](docs/protocol.md) —— 协议文档（认证流程 / ag2 / spec v2 帧格式 / S400 数据面）
- [docs/testing.md](docs/testing.md) —— 逐设备实机验证清单（§20 是全量构建回归门禁）
- [docs/mesh-ref/](docs/mesh-ref/) —— **BLE Mesh 资料集**：官方接入文档、协议要点汇总、网关侧解析脚本
- [docs/mesh-ref/provisioning/](docs/mesh-ref/provisioning/) —— **BLE Mesh 配网协议逆向笔记**（7 篇）：完整时序、通道与指令、错误码、实测边界（20s 链路寿命）、安全设计分析、中继工程实践
- [tools/mesh/](tools/mesh/) —— mesh 工具：多帧重组 + 熵值分析、btsnoop 解析、私钥派生假设验证
- [tools/build_all.sh](tools/build_all.sh) —— **改完 SDK 先跑这个**：一轮编译 `esp32/` 下全部 25 个工程，末尾打 `PASS (25)` / `FAIL` 汇总，专治「改了共享函数签名、某个工程静默编译不过」
- [.github/ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE/) —— 实测记录模板（PSK / 标准认证），串口日志必填
- [test/test_spec.c](test/test_spec.c) —— 黄金样本即实测帧
- [test/test_psk.c](test/test_psk.c) —— PSK 状态机单测（帧序列 / 模式分流 / KDF 标签）

## License

非商业许可 —— 仅供学习研究，禁止商用，详见 [LICENSE](LICENSE)。
