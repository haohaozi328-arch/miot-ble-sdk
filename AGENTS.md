# AGENTS.md —— 给 AI 编码助手的执行手册

本文件是**操作指令**，不是介绍。目标：在一台干净的电脑上，由 AI 独立完成
「装环境 → 建工程 → 编译 → 烧录 → 验收」，最终得到一块能被**官方原版米家 App**
发现并绑定的设备。

读者是 AI。人类请直接看 [QUICKSTART.md](QUICKSTART.md)。

---

## 0. 目标与默认选择

| 项 | 值 |
|---|---|
| 芯片 | ESP32-C3（4MB Flash 即可） |
| 固件框架 | ESP-IDF **v5.5 或 v6.0**，NimBLE + mbedTLS |
| 默认设备档案 | **蓝牙温湿度计2**（`cgllc-cgdk3`，PID **0x055B**） |
| 为什么是它 | 走标准认证（非 PSK）、免网关、属性读写可验证、PID 国区有效 |
| 成功标志 | 米家 App「+」能扫到「蓝牙温湿度计2」→ 绑定成功 → 设备页显示温湿度 |

**默认只做这一台。** 其余档案见 [README 设备清单](README.md) §设备清单，
换档案只需换 `*_init()` 与 PID，不要一次做多台。

---

## 1. 五条禁止（违反任何一条，产物一定不可用）

```
1. 禁止让用户填写、修改、生成、伪造 did —— did 只能由云端在 REGISTER 阶段下发
2. 禁止使用海外 PID（如温湿度计3 mini 的 0x5BEA）；国区是 0x55B5
3. 禁止建议 root / Xposed / Magisk / LSPatch / 安装任何模块 / 修改或注入米家 App
   —— 用户路径上只有「官方原版 App」，本仓库也不提供任何 hook 工具
4. 禁止承诺「一定能添加成功」—— 云端档案与账号区域不在本项目控制范围内
5. 遇到「手机网络异常 / 104」不要建议换网络、重启路由器、清 App 缓存
```

第 3 条是硬红线。若你（AI）在任何步骤里想到「hook 一下 App 就能过」，说明方向错了：
正确做法是让设备端协议正确，让官方 App 自愿接受它。

---

## 2. 必须先理解的一件事：did 从哪来

```
云端 REGISTER 下发 92B 加密帧
  → AES-CCM(dev_key) 解密 → 明文 = did(20) + cloud_sign(64) + utc(4)
  → 设备存进 NVS，之后 info 回传时去掉前后导 0x00
```

**设备端不生成 did，也没有地方需要填。** 所以只要 `model`/PID 是真实存在的产品、
账号是国区，云端就会分配一个 `blt.3.*` 形态的 did，绑定自然成功。

反过来，任何人把 did 写死在固件里，都会得到：

```
{"code":-8,"message":"cant find did <did>","result":null}
→ App 显示「设备绑定账号失败 (104,did=...)」
```

---

## 3. 环境准备（选一条，不要混用）

### 3.1 原生安装（推荐，Linux/macOS）

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3
. ./export.sh          # 每开新终端都要执行
idf.py --version
```

Windows：用 ESP-IDF Installer（官方离线包，选 v5.5.x，目标 esp32c3），
之后从开始菜单启动 "ESP-IDF PowerShell"。

### 3.2 Docker（不想装工具链时）

```bash
docker run --rm -it -v "$PWD":/project -w /project \
  espressif/idf:v6.0 bash
# 容器内：idf.py --version
```

串口在 Linux 容器内映射：`--device=/dev/ttyUSB0`；Windows 用 `-p COM6` 时
需在宿主机烧录（容器内看不到 COM 口），建议只把 Docker 用于编译。

### 3.3 判据

`idf.py --version` 输出版本号，且 `python -c "import serial"` 不报错。
两步任一失败就先修环境，**不要继续后面的步骤**。

---

## 4. 步骤

### S1 — 先用 host 测试确认 SDK 本身是好的

```bash
cd miot-ble-sdk
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure     # 必须全绿
./build/example-thermometer                    # 打印一帧 properties_changed
```

`ctest` 失败说明 SDK 或编译器有问题，与硬件无关，先解决再往下走。

### S2 — 建一个空的 IDF 工程（不要指望仓库里有现成工程）

本仓库是 SDK，**不含可烧录工程**，工程由你（AI）现建：

```bash
cd ..                                   # 与 miot-ble-sdk 同级，避免污染仓库
idf.py create-project miot-thermo
cd miot-thermo
idf.py set-target esp32c3
```

把 SDK 源码直接编进 `main` 组件（不走组件注册，最少踩坑）。编辑
`main/CMakeLists.txt`：

```cmake
set(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../miot-ble-sdk")

idf_component_register(
    SRCS "main.c" "ble_link.c" "store.c"
         "${SDK_ROOT}/src/miot_spec.c"
         "${SDK_ROOT}/src/miot_props.c"
         "${SDK_ROOT}/src/miot_crypto.c"
         "${SDK_ROOT}/src/miot_ag2.c"
         "${SDK_ROOT}/src/miot_device.c"
         "${SDK_ROOT}/src/miot_auth.c"
         "${SDK_ROOT}/src/miot_profiles.c"
         "${SDK_ROOT}/src/miot_beacon.c"
         "${SDK_ROOT}/port/esp32/miot_port_esp32.c"
    INCLUDE_DIRS "." "${SDK_ROOT}/include"
    REQUIRES bt nvs_flash mbedtls esp_timer
)
```

`sdkconfig.defaults` 追加（否则编译能过、运行必挂）：

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256
CONFIG_BT_NIMBLE_EXT_ADV=n
CONFIG_MBEDTLS_CCM_C=y
CONFIG_MBEDTLS_ECDH_C=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
```

### S3 — 写三样东西（这是唯一需要你从零实现的部分）

SDK 已经提供了认证状态机、spec 编解码、属性表、广播帧，**你只需要提供 BLE 与存储**。

#### S3.1 `store.c` —— NVS 持久化绑定身份（否则每次重启都要重新绑定）

```c
#include "nvs.h"
#include "miot_ble/miot_auth.h"

static const char *NS = "miot", *KEY = "id";

static void store_save(void *user, const miot_auth_identity_t *id) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY, id, sizeof(*id));
    nvs_commit(h); nvs_close(h);
}

static bool store_load(void *user, miot_auth_identity_t *id) {
    nvs_handle_t h; size_t len = sizeof(*id);
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_blob(h, KEY, id, &len) == ESP_OK && len == sizeof(*id));
    nvs_close(h);
    return ok;
}
```

#### S3.2 `ble_link.c` —— GATT 服务 + 广播

**服务与特征（必须完全一致，少一个 App 就找不到设备）**

| 角色 | UUID | 属性 | 用途 |
|---|---|---|---|
| Service | `0000FE95-0000-1000-8000-00805F9B34FB` | primary | 小米 BLE 服务 |
| Char | `00000010-…`（16-bit `0x0010`） | Write + Notify | 认证结果码通道 |
| Char | `00000019-…`（`0x0019`） | Write + Notify | 认证数据通道 |
| Char | `0000001A-…`（`0x001A`） | Write + Notify | spec 数据通道 |
| Char | `0000001B-…`（`0x001B`） | Write | 第二写通道 |

NimBLE 用 16-bit UUID 注册即可（会自动展开成上面的 128-bit）：

```c
static const ble_uuid16_t u_svc  = BLE_UUID16_INIT(0xFE95);
static const ble_uuid16_t u_0010 = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t u_0019 = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t u_001a = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t u_001b = BLE_UUID16_INIT(0x001B);
```

写回调必须按来源分流（**发错通道 App 永远不判成功**）：

| 写入通道 | 转给 |
|---|---|
| `0x0010` | `miot_auth_on_0010(&auth, d, n)` |
| `0x0019` | `miot_auth_on_0019(&auth, d, n)` |
| `0x001A` / `0x001B` | `miot_device_on_rx(&dev, d, n)` |

notify 同理：`0x0010`/`0x0019` 由 auth 层触发，`0x001A` 是 `miot_port_t.send_notify` 的出口。
断开连接时调用 `miot_auth_on_disconnect(&auth)` 并重启广播。

**可复制的 NimBLE 骨架**（按此写，不要另设计；只保留必要部分）：

```c
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static uint16_t s_conn, s_h10, s_h19, s_h1a;
static ble_uuid16_t u_svc = BLE_UUID16_INIT(0xFE95);
static uint8_t s_wbuf[512];

static uint16_t take_flat(struct ble_gatt_access_ctxt *x, uint8_t *buf, size_t cap)
{
    uint16_t len = OS_MBUF_PKTLEN(x->om);
    if (len > cap) return 0;
    if (ble_hs_mbuf_to_flat(x->om, buf, cap, &len) != 0) return 0;
    return len;
}

static int cb_10(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) miot_auth_on_0010(&s_auth, s_wbuf, n);
    return 0;
}

static int cb_19(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) miot_auth_on_0019(&s_auth, s_wbuf, n);
    return 0;
}

static int cb_1a(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) miot_device_on_rx(&s_dev, s_wbuf, n);      /* 001A 与 001B 共用 */
    return 0;
}

static const struct ble_gatt_chr_def chars[] = {
    { .uuid = BLE_UUID16_DECLARE(0x0010), .access_cb = cb_10, .val_handle = &s_h10,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x0019), .access_cb = cb_19, .val_handle = &s_h19,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x001A), .access_cb = cb_1a, .val_handle = &s_h1a,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x001B), .access_cb = cb_1a,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { 0 }
};

static const struct ble_gatt_svc_def svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &u_svc.u, .characteristics = chars },
    { 0 }
};

/* notify：0010 / 0019 由 auth 层的回调调用，001A 是 port.send_notify 的出口 */
static int notify_chr(uint16_t handle, const uint8_t *d, size_t n)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
    return om ? ble_gatts_notify_custom(s_conn, handle, om) : -1;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.svc_data_uuid16 = g_adv_frame;            /* 只放 service data，不放 uuids16（省 4B） */
    f.svc_data_uuid16_len = g_adv_len;
    if (ble_gap_adv_set_fields(&f) != 0) { /* rc=4 → 帧超 31B，拆帧重试，别 stop 后 return */ }
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, on_gap, NULL);
}
```

注意两点：

- `s_h10/s_h19/s_h1a` 是 `.val_handle` 的**输出**（注册完成后才被填值），只用于
  notify，不要拿它判断"这次写入来自哪个特征"——那是 `attr_handle` 的活，且时机不可靠。
  用上面的"一特征一个 cb"写法最省事。
- `g_adv_frame` / `g_adv_len` 由 S3.2 的两种帧形态生成，广播内容每 5s 更新一次。

**广播有两种形态，用错就扫不到设备**

绑定前（App 靠它发现设备，**未加密，必须带 Capability**）：

```
AD 结构：Flags(3) + Service Data
Service Data = [len][0x16][95 FE] + FC(2) + PID(2 LE) + frame_cnt(1) + MAC(6 空中序) + capability(1)

FC        = 30 58
          0x30 = MAC(0x10) | Capability(0x20)，未加密(bit3=0)、不含 Object
          0x58 = version 5(0x50) | authMode 2 标准认证(0x08) | registered 0(bit8)
capability= 0x10   (BondAbility=2 后绑定，左移 3 位)
PID       = 0x055B → 字节 5B 05
```

绑定后（给网关上报，用 SDK 现成函数）：

```c
miot_beacon_cfg_t cfg = { .pid = 0x055B, .mac_air = {...}, .beacon_key = {...} };
// beacon_key 用 identity.bind_key（12~28B 那 16 字节）
int n = miot_beacon_frame_v5(&cfg, frame_cnt++, objs, objs_len, out, sizeof(out));
// 输出 FC = 58 59（已加密 + registered=1），直接塞进 Service Data
```

温湿度计2 的广播对象（三路轮换，别塞一帧）：

| 对象 | 编码 | 长度 |
|---|---|---|
| `0x1004` 温度 | `miot_beacon_obj_i16(p, 0x1004, (int16_t)(t*10))` | 5B |
| `0x1006` 湿度 | `miot_beacon_obj_u16(p, 0x1006, (uint16_t)(h*10))` | 5B |
| `0x100A` 电量 | `miot_beacon_obj_u8(p, 0x100A, 92)` | 4B |

**31 字节硬上限**：`flags(3) + [len][0x16][95 FE](4) + 帧 ≤ 24B`。
温度帧 = 2+2+1+6+5+3+4 = 23B ✓；一次塞两个对象 = 28B ✗。
`ble_gap_adv_set_fields()` 返回 `4`（EMSGSIZE）就是这个原因，且**失败后不要停在
stop 状态**，否则表现为「App 一直正在连接 → 超时」。

#### S3.3 `main.c` —— 接线与主循环

```c
void app_main(void) {
    nvs_flash_init();
    miot_port_esp32_crypto_init();          // 必须最先：注册 mbedTLS 后端

    static miot_auth_t auth;
    static miot_auth_port_t ap = {
        .notify_0010 = notify_0010, .notify_0019 = notify_0019,
        .save = store_save, .load = store_load, .random = rand_fill,
    };
    miot_auth_init(&auth, &ap);             // 内部会 load 已绑定身份
    miot_auth_prepare(&auth);               // 预生成 ECDH（C3 上约 390ms，别放进回调）

    static miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    thermometer_init(&dev, &port, &tbl, &cnt);   // port.send_notify → notify_001A

    ble_link_init(&auth, &dev);
    ble_link_start();
    xTaskCreate(poll_task, "poll", 4096, NULL, 5, NULL);
}

static void poll_task(void *arg) {
    bool keys_set = false;
    for (;;) {
        miot_auth_poll(&auth, esp_timer_get_time() / 1000);

        bool in = miot_auth_is_logged_in(&auth);
        if (in && !keys_set) {                 // LOGIN 成功 → 接通 spec 会话密钥
            miot_device_set_session_keys(&dev, miot_auth_session_keys(&auth));
            keys_set = true;
        } else if (!in && keys_set) {          // 断线 → 清会话，等下次 LOGIN
            keys_set = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

数据源：默认用缓慢变化的演示值即可（本 SDK 是协议实现，不含传感器驱动）。
要接真实传感器，只改一处：

```c
static void sensor_read(float *t, float *h) { *t = 25.0f; *h = 55.0f; }  // ← 换成 SHT30/AHT20 读取
```

周期上报（温湿度计2 走 GATT spec 通道，不是广播）：

```c
if (miot_auth_is_logged_in(&auth)) thermometer_report(&dev, t, h, 0);
```

### S4 — 编译烧录

```bash
idf.py build
idf.py -p <PORT> flash monitor     # Windows: -p COM6   Linux: -p /dev/ttyUSB0
```

首次编译 5~15 分钟。**不要**因为慢就中断或改架构。

### S5 — 验收（缺一不可）

串口（115200）按顺序出现：

```
ECDH keypair ready
device info sent (did=0B)              ← 绑定前 did 长度为 0，正常
REGISTER COMPLETE (result 0x11 on 0x0010)
REGISTER: did stored (utc=...)         ← 云端分配了 did，这一步过了绑定就成功
*** STD REGISTER: bind result OK (bound=1) ***
*** STD LOGIN COMPLETE (0x21 on 0x0010) ***
spec req seq=... pt=...B               ← 手机在正常读写属性
```

App 侧：`+` → 扫到「蓝牙温湿度计2」→ 绑定 → 设备页出现温湿度数值 → 数值随上报变化。

**两条判据同时满足才算成功。** 只满足一半时按 [docs/troubleshooting.md](docs/troubleshooting.md) 定位。

---

## 5. 失败分支（先查表，不要猜）

| 现象 | 首个检查点 | 结论 |
|---|---|---|
| App 扫不到设备 | 广播是否含 PID、FC 是否 `30 58`、`adv_set_fields` 是否返回 4 | 广播格式/长度问题 |
| 扫到但连不上 | GATT 是否注册了 FE95 与四个特征 | 服务缺失 |
| 「设备绑定账号失败 (104,did=…)」 | **不是网络问题** | 见 troubleshooting Q1 |
| `REGISTER: encrypted did decrypt FAIL` | App 版本 | 92B 帧偏移差异，报告版本号 |
| `LOGIN: app confirm MISMATCH` | devConfirm 是否重发 | 时间/重发逻辑 |
| 绑定成功但页面空白 | 属性 siid/piid 是否与档案一致 | 用 `thermometer_init` 不要自造 |
| 「未连接蓝牙网关」 | 网关缓存 | 插拔网关 + 重启 App，非固件问题 |

完整决策树：[docs/troubleshooting.md](docs/troubleshooting.md)。
纯净 App 下各档案能否绑定：[docs/clean-app-matrix.md](docs/clean-app-matrix.md)。

---

## 6. AI 最容易犯的六个错

1. **自己编 did 或让用户填 did** → 必然 104，见 §2
2. **把 notify 发到 `0x0019`** → App 只认 `0x0010` 上的结果码，永远不判成功
3. **广播里塞两个以上对象** → 超 31B，`rc=4`，广播停摆，表现为"一直正在连接"
4. **忘记 `miot_crypto_set_ops()`** → 一切加密返回错误，日志只有 `decrypt FAIL`
5. **忘记 `miot_auth_prepare()` 或放进回调里** → device info 迟到，App 以 `-51` 断开
6. **建议装模块/改 App 来绕过问题** → 触碰红线，且对最终用户完全不可用

---

## 7. 交付给用户时必须说明的三句话

1. 这是**协议研究/互操作性验证**用途，不是可售产品（见 [NOTICE.md](NOTICE.md)）；
2. 设备能被米家接受的前提是**真实 PID + 国区账号 + 官方原版 App**，云端策略由小米控制；
3. 换其他设备档案前先看 [clean-app-matrix.md](docs/clean-app-matrix.md)，
   有些型号走 legacy PSK，本 SDK 不支持，纯净 App 下无法绑定。
