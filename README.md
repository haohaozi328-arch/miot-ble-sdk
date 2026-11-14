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
│  miot_profiles 已验证设备档案（12 台设备，见下方设备清单）     │
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
docs/               protocol.md（协议）/ testing.md（逐设备实机验收 + 踩坑）
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

**只想要一台设备的最小工程？** 不用动 multi_dev，直接用下面这 6 个（各自独立、互不依赖，
每个都是 `app_main.c` + `gatt_miot.c` 两个文件、700~950 行，不含 Mesh 栈，
`idf.py erase-flash flash monitor` 就能烧）：

| 工程 | 模拟设备 | 代码量 | 实测状态 |
|---|---|---|---|
| [`esp32/t3pro_adv`](esp32/t3pro_adv) | 温湿度计 3 Pro（0x9AB9） | 866 行 | ✅ 2026-11-13 绑定后温/湿/CO₂ 三项同时出数，小爱可查，云端有历史曲线 |
| [`esp32/btn1_adv`](esp32/btn1_adv) | 无线开关 单键（0x2387） | 799 行 | ✅ 绑定通过 + 单击/双击/长按事件加密广播上云；README 含与云端产品定义的逐层对比 |
| [`esp32/btn2_adv`](esp32/btn2_adv) | 无线开关 双键（0x1949） | 790 行 | ✅ 2026-11-14 实机：米家添加一次过，左键单击 `0x4E0C` / 双击 `0x4E0D`（`key=1`）、右键单击 `0x4E0C`（`key=2`）均上云；长按待测。**烧前必看**：派生 MAC，直接烧物理 MAC 会被米家去重扫不到 |
| [`esp32/thermo_gatt`](esp32/thermo_gatt) | 温湿度计 2（0x055B） | 745 行 | ✅ 2026-11-07 绑定 + 设备页实时读数；链路注释最全，适合当模板改别的型号 |
| [`esp32/scale_adv`](esp32/scale_adv) | 体脂秤 S400（0x30D9） | 870 行 | ✅ 2026-11-14 实机：绑定一次过 → `13.1 sync` 应答 5 个 out → 点「测量」`ACTION 10.1` → 7 帧称重全部 `rc>0`、连称三条各自入库 → **离线称重**（断手机 + 短按 BOOT）后下次连手机整段 7 帧重放、App 直接出报告；记录时间由 `13.1` 的手机时间校准。**烧后必调**：`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192`，默认 4096 会在 spec 解析时栈溢出复位 |
| [`esp32/wx08zm_gatt`](esp32/wx08zm_gatt) | 驱蚊器 智能版（0x040A） | 944 行 | ⚠️ 编译通过、能回待绑定态；legacy PSK 握手是否被云端接受**尚未真机验证** |

六者骨架同构（`thermo_gatt` 是最干净的那份），换型号只改三处：**PID、spec 属性表、广播对象号**。

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

## 设备清单（12 台，米家 Android App 实测状态）

> **先看这一条**：各档案在**官方原版 App（无任何模块、未 root）**下能否添加，
> 见 [docs/clean-app-matrix.md](docs/clean-app-matrix.md)。
>
> ⚠️ **两个档案当前有问题，不要当「开箱即用」演示**：
> **夜灯2（0x03DD）** 与 **驱蚊器智能版（0x040A）**。具体现象与规避方式见
> [docs/known-issues.md](docs/known-issues.md)；夜灯2 的完整打通记录（NUS 文本协议 +
> legacy RC4 登录 + 去 hook 验证）见
> [docs/nightl2-ag2-handoff.md](docs/nightl2-ag2-handoff.md)。
> 其余档案的 legacy PSK 分支已在 `miot_auth` 实现（默认 `AUTO` 自动判定），
> **但尚未实机验证**，帧格式与已实现状态机见 [docs/psk-auth.md](docs/psk-auth.md)。
> 推荐入门档案：**蓝牙温湿度计2**（标准认证、免网关、属性可验证）。
>
> 「详情」= 该设备代码的存放位置：`examples/<设备>/main.c`（可运行演示）+
> `src/miot_profiles.c`（spec 属性表）/ `include/miot_ble/miot_beacon.h`（广播对象表）。

### A. spec 连接型（建 GATT 连接，走标准认证 + spec v2 通道）

| # | 设备名 | 型号 / model | 描述 | 验证状态 | 详情（代码） |
|---|--------|--------------|------|----------|--------------|
| 1 | 米家体脂秤 S400 | MJTZC01YM / `yunmai.scales.ms103`（PID 0x30D9） | 绑定/登录/用户资料推送/测量事件上报（8.1 过程流 + 8.2 结果 + 11.1022 + 属性变更）全链路，含云端入库与完整身体成分报告 | 全流程 ✓（App 实时页 + 报告页 + 云端历史实测通过） | [examples/scale_s400/](examples/scale_s400/main.c) · `src/miot_profiles.c §1` |
| 2 | 蓝牙温湿度计2 | LYWSD03MMC(t9) / `cgllc-cgdk3`（PID 0x055B） | 双 spec siid 兼容（同一属性在两个 spec 里 siid 不同，两份都注册），温度 float / 湿度 / 电量，周期上报 | 绑定 + spec 读写 ✓ | [examples/thermometer/](examples/thermometer/main.c) · `src/miot_profiles.c §2` |
| 3 | 夜灯2 | MUE4094RT（PID 0x03DD） | 亮度%(1-100)/延时ms/开关 + 4 个固定场景；`esp32/multi_dev` 走**北欧 NUS 文本协议**（`6E400001/2/3`）+ legacy RC4 登录（4B 恒等 `E0C7FAFE`） | ⚠️ **已打通但有问题**：原版 App 可绑定，详情页可读可写（实测 `set_bright 35` → 回读 35）；遗留 ① 偶发「连接超时」（页面打开瞬间未抢到连接，重试自恢复）② 删除设备后重扫前**必须先 `factory`**（设备侧带 token 时广播 BondAbility=0x29，扫描列表直接过滤）③ 无「如果」触发条件（spec 只有属性、无 event） | [examples/night_light/](examples/night_light/main.c)（spec 层）· `esp32/multi_dev`（NUS 全链路） |
| 4 | 驱蚊器智能版 | WX08ZM（PID 0x040A） | 开关下行 + set 后立刻回报（页面开关停在正确位置）+ 电量/驱蚊片余量 | ⚠️ **有问题**（现象待登记，见 [docs/known-issues.md](docs/known-issues.md)） | [examples/repeller/](examples/repeller/main.c) · `src/miot_profiles.c §6` |
| 5 | 米家无线开关（双键版） | XMWXKG01YL / `yeelink.remote.contrl`（PID 0x1949） | 绑定 + 按键事件广播（0x4E0C 单击 / 0x4E0D 双击 / 0x4E0E 长按，载荷 1B = 键位 1 左 / 2 右）；按键值不进 GATT，事件只走广播 | 绑定 + 事件上报 ✓（连接握手偏慢，见 testing.md #5） | [examples/button_event/](examples/button_event/main.c) · `miot_beacon.h` |

### B. MiBeacon 广播型（不建连接，加密广播帧）

| # | 设备名 | 型号 / PID | 描述 | 验证状态 | 详情（代码） |
|---|--------|-----------|------|----------|--------------|
| 6 | 甲醛检测仪 | JQJCY01YM（PID 0x02DF） | 0x1010 甲醛(0.01mg/m³) / 0x1004 温度 / 0x1006 湿度 / 0x100A 电量，三路轮换分帧 | ✓ | [examples/sensor_hcho/](examples/sensor_hcho/main.c) · `miot_beacon.h` |
| 7 | 门窗传感器2 | MCCGQ02HL（PID 0x098B） | 0x1019 开合(开/关/超时未关/复位) / 0x1018 光照 / 0x100A 电量 | ✓ | [examples/sensor_door/](examples/sensor_door/main.c) · `miot_beacon.h` |
| 8 | 人体传感器2 | RTCGQ02LM（PID 0x0A8D） | 0x000F 移动+光照 / 0x1017 无人时长 / 0x1001 按键 / 0x100A 电量 | ✓ | [examples/sensor_motion/](examples/sensor_motion/main.c) · `miot_beacon.h` |
| 9 | 水浸传感器 | SJWS01LM（PID 0x0863） | 0x1014 漏水状态(0=正常 >0=漏水) / 0x100A 电量 | ✓ | [examples/sensor_leak/](examples/sensor_leak/main.c) · `miot_beacon.h` |
| 10 | 温湿度计3 mini | MJWSD06MMC（国区 pdid 21941 = **0x55B5**；0x5BEA 是海外版 23530，国区扫不到） | 0x4C01 温度 float32 / 0x4C02 湿度 uint8 / 0x4803 电量 uint8（对象 ID 依 XiaomiGateway3 `mi`=19457/19458/18435） | ✓ | [examples/thermo_t3m/](examples/thermo_t3m/main.c) · `miot_beacon.h` |
| 11 | 温湿度计3 Pro | MJWSD07MMC（PID **0x9AB9**） | 0x5C01 温度 float32 / 0x5C02 湿度 uint8 / 0x601D CO₂ uint16 / 0x4803 电量 uint8，四对象三帧轮换（一帧装不下）。**对象号是 3 Pro 专属的新系列，沿用 3 mini 的 0x4C01/0x4C02 会导致「设备在线但数值全空」**。真机为按键连接模式（平时不广播可连接态，按键后才 `solicited=1`） | ✓（广播 → 网关解密 → 云端 → 小爱语音查温湿度，全链路实测；2026-11-13 23:12 独立工程实机三项同时出数：温度 / 湿度 / CO₂） | [esp32/t3pro_adv/](esp32/t3pro_adv)（单设备完整工程，`idf.py build` 即烧）· `multi_dev` 档案 `T3PRO` |

### C. 事件型按键（可连接用于绑定，事件本身走广播）

| # | 设备名 | 型号 / PID | 描述 | 验证状态 | 详情（代码） |
|---|--------|-----------|------|----------|--------------|
| 12 | 小米无线开关（蓝牙版） | XMWXKG01CM / pdid 9095（**PID 0x2387**） | 单键：0x4E0C 单击 / 0x4E0D 双击 / 0x4E0E 长按，载荷恒 1；绑定走 legacy 移位应答，事件由网关收广播后触发自动化 | ✓（绑定 + 事件；自动化条件需重连网关后出现，上报有网关轮询延迟） | [examples/button_event/](examples/button_event/main.c) · `miot_beacon.h` |

> 各设备的验证均针对米家 Android App 实测（扫描添加 → 绑定 → 数据互通），
> 逐设备回归步骤见 [docs/testing.md](docs/testing.md)。

## 已知边界（明确未适配）

| 项 | 结论 | 原因 |
|----|------|------|
| S400 抱婴称重 | ❌ 未适配 | 固件未实现婴儿称重语义（低量程/去皮与对应事件字段），本轮不做 |
| S400 平衡测试 | ❌ 未适配 | 未实现站姿/重心偏移数据 |
| 驱蚊器定时任务 | ❌ 未适配 | 固件对 `0x0003 TIMER_ID` / `0x0004 TIMER_CONTENT` 只记录不落表，`0x0006 TIMER_INDEXS` 恒返回 11×`0xFF`（空槽）；且设备无 RTC，`0x0005 LINUX_STAMP` 未用于走时 |
| **legacy PSK 绑定**（驱蚊器 / 夜灯重绑 / 部分老型号） | ⚠️ 已实现·未实测 | 与标准认证**同构**（同一 ECDH P-256，换 `smartcfg-*` HKDF 标签，多一帧 `0x04 dev_sign`）。代码在 `miot_auth`（`MIOT_AUTH_MODE_PSK`，默认 `AUTO` 由启动帧自动判定），`test/test_psk.c` 覆盖状态流转。**缺的是真机验证**，共三个未知：① App 扫描阶段认哪种广播形态（已做候选表轮播探测，[psk-auth.md](docs/psk-auth.md) §6.1）② `dev_sign` 签什么 ③ 云端 `ble_psk_*` 是否仍开放 → 实测结果请用 [PSK 实测 issue 模板](.github/ISSUE_TEMPLATE/psk-field-test.yml)，串口日志必填 |
| 「手机网络异常」/ `设备绑定账号失败 (104,did=…)` | ⚠️ 见 FAQ | 云端查不到该 did 的产品档案（自造 did、PID 不符、或 92B 帧未解出）。**与网络无关**，见 [docs/troubleshooting.md](docs/troubleshooting.md) Q1 |
| 门锁 / security-chip 类 | ❌ 硬墙 | 云端强制 X.509 芯片证书链（`securityChip=1`），无法借壳 |
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
| `miot_profiles` | 设备档案 | 5 个 spec 型 + 5 个广播型设备的 init/report 函数（见设备清单） |

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
- [docs/testing.md](docs/testing.md) —— 逐设备实机验证清单
- [.github/ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE/) —— 实测记录模板（PSK / 标准认证），串口日志必填
- [test/test_spec.c](test/test_spec.c) —— 黄金样本即实测帧
- [test/test_psk.c](test/test_psk.c) —— PSK 状态机单测（帧序列 / 模式分流 / KDF 标签）

## License

非商业许可 —— 仅供学习研究，禁止商用，详见 [LICENSE](LICENSE)。
