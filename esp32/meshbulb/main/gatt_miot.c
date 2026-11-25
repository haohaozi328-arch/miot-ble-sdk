/**
 * @file gatt_miot.c
 * @brief 0xFE95 服务与四个特征值的 NimBLE 实现
 */
#include "gatt_miot.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake_cert.h"
#include "blob84.h"
#include "pkg4.h"
#include "ident_self.h"
#include "ident_mix.h"
#include "pkg4_sign.h"
#include "relay.h"
#include "nvs.h"
#include "nvs_flash.h"

/* ★身份选择开关（2026-11-20 三组对照实验）
 *   0 = 真机原版：真证书 + 真 84B            → 已知能过 auth（对照组）
 *   1 = 全自签  ：自签证书 + 自签 84B        → 实测被服务器拒（-39）
 *   2 = 混搭    ：真证书 + **我们自己的 mesh 公钥**
 *       用途：拆开「证书」和「mesh 公钥」两个变量。若这组能过 auth，
 *       说明服务器不把 dev_mesh_pub 绑到证书上，纯 ESP32 还有戏。 */
#define USE_SELF_ID 2
#if USE_SELF_ID == 1
#define ID_CERT      g_self_cert
#define ID_CERT_LEN  g_self_cert_len
#define ID_BLOB      g_self_blob
#define ID_BLOB_LEN  g_self_blob_len
#elif USE_SELF_ID == 2
#define ID_CERT      g_fake_cert      /* 真机原版 451B 证书 */
#define ID_CERT_LEN  g_fake_cert_len
#define ID_BLOB      g_mix_blob       /* 真机头 + 我们的 P-256 公钥 */
#define ID_BLOB_LEN  g_mix_blob_len
#else
#define ID_CERT      g_fake_cert
#define ID_CERT_LEN  g_fake_cert_len
#define ID_BLOB      g_auth_blob
#define ID_BLOB_LEN  g_auth_blob_len
#endif
#include "eph_key.h"

static const char *TAG = "MIOT-GATT";

/* 服务与特征值 UUID（SIG 分配给小米的 MiBeacon 服务 0xFE95）
 * 米家 App 侧的完整形式是 0000FE95-0000-1000-8000-00805F9B34FB */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFE95);
/* ★2026-11-19 按真机 12 条对齐（依据 esp32/multi_dev，已验证在米家跑通）：
 *   真机 FE95 服务下有 12 条特征，少一条 App 服务发现后就走错分支。
 *   我们之前只有 8 条（缺 0001/0002/0014/0016），现场症状正是
 *   「登录成功 → 读版本 → 1.2s 自己断开 → 一条 GET 都不发」。 */
static const ble_uuid16_t s_c01_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0001);
static const ble_uuid16_t s_c02_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t s_c04_uuid = BLE_UUID16_INIT(0x0004);
static const ble_uuid16_t s_c10_uuid = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t s_c19_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t s_c1a_uuid = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t s_c1b_uuid = BLE_UUID16_INIT(0x001B);
static const ble_uuid16_t s_c17_uuid = BLE_UUID16_INIT(0x0017);
static const ble_uuid16_t s_c18_uuid = BLE_UUID16_INIT(0x0018);
static const ble_uuid16_t s_c13_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0013);
static const ble_uuid16_t s_c14_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0014);
static const ble_uuid16_t s_c16_uuid __attribute__((unused)) = BLE_UUID16_INIT(0x0016);
/* ★2026-11-19 真模组 MHCB12G 配网 logcat 实证：App 在 FE95 下额外订阅 0x001C。
 *   这是 Mesh 设备独有的配网通道（非 SIG PB-GATT 的 0x1827），缺它 App 服务
 *   发现阶段就会判定「不是 Mesh 设备」而放弃配网。 */
static const ble_uuid16_t s_c1c_uuid = BLE_UUID16_INIT(0x001C);
/* ★2026-11-19 真机 GATT 表实测（14:D8:81:E6:95:3C，PC 直连 dump）：
 *   真机 FE95 下只有 9 条，且**没有 0x0019**、没有 0001/0002/0013/0014，
 *   但有一条我们从没有过的 0x0005（READ|NOTIFY，实读回 Read Not Permitted）。
 *   特征表的差异直接决定 App 走哪条配网路线：
 *     有 0019 → 走普通 BLE 认证（A4 → 40 …），Mesh 设备不会走这条
 *     无 0019 → 走 001C 的 Mesh 私有配网（首包 66B）
 *   这就是我们这块一直卡在「A4 之后 0x40 无应答」的结构性原因。 */
static const ble_uuid16_t s_c05_uuid = BLE_UUID16_INIT(0x0005);

/* ★2026-11-18 补齐官方规范里的 0x0004（Mi Service Version，READ）。
 *   官方 mijia_profiles/mi_service_server.c 里服务是 5 条特征值，第一条就是它：
 *     CHAR_VERSION_IDX = 0, char_property = MIBLE_READ, char_value_len = sizeof(version)
 *     static const uint8_t version[20] = MIBLE_LIB_AND_DEVELOPER_VERSION;
 *   内容是 "主.次.修订_开发者版本" 的 ASCII，标准认证库（HAVE_MSC=0）展开为
 *   1.0.1_xxxx；MSC/证书库才是 2.2.6_xxxx，Mesh 库 1.3.0_xxxx。
 *   真机（MHCB12G）读到的是 "3.0.2_0004"（Mesh 固件）。
 *
 *   我们之前**完全没有这条特征值**，而插件在 connect 之后是先读版本再决定
 *   走哪套 spec 的。缺它的表现就是：连上 → discoverServices(status=0) →
 *   立刻 cancelOpen/close，一个字节的读写都没有（2026-11-16 logcat 实录）。
 *
 *   ★版本号取值来自同仓库 multi_dev 的反编译结论（参考工程 v32）：
 *     标准认证链路里 App 读到 0x0004 后不解密，直接取到第一个 0x00 当字符串，
 *     然后 mb1(mac,ver) 存本地 + ir9(mac,ver) 上报云端，**上报成功才算注册完成**；
 *     失败就 onFailure「蓝牙连接失败」，并在读 0x0004 之后 1.17s 主动断开
 *     （reason=0x213）——和 meshbulb 现场每轮的收尾完全一致。
 *     版本要往**高**报，否则 App 认为固件过旧、转去走 OTA。故用 9.9.9_9999。
 *     必须是明文，不能加密。 */
/* ★★2026-11-18 根因（最终）：0x0004 必须返回【定长 20 字节】，尾部补 0x00。
 *
 *   官方 mi_service_server.c：
 *       static const uint8_t version[20] = MIBLE_LIB_AND_DEVELOPER_VERSION;
 *       char_value_len = sizeof(version);        // 20，不是 strlen
 *   真机 nRF 读出来是 "3.0.2_0004" + 10 个 0x00，共 20B。
 *
 *   App 侧解析（v32 反编译）：读到 0x0004 后不解密，直接「取到第一个 0x00
 *   作为字符串结束」，然后 mb1(mac,ver) 存本地 + ir9(mac,ver) 上报云端。
 *   我们之前只返回 strlen 那么长（10B），**没有终止符**，App 取不到 '\0'：
 *   版本串无效 → 上报失败 → onFailure「蓝牙连接失败」→ 读 0x0004 之后
 *   1.17s 主动断开（reason=0x213）。这就是每轮必断的真正原因。
 *
 *   所以内容（3.0.x）不是关键，长度 20 + 补 0 才是。 */
static uint8_t s_version[20] = "9.9.9_9999";

/* ★2026-11-19 版本号候选表（串口 `ver` 可切，默认每次连接轮换一条）。
 *
 * 已用 PC 端 BLE 直读证实：0x0004 现在返回的是干净的 20B
 * （"9.9.9_9999" + 10 个 0x00，MTU 247 下 read 一次拿到），格式不再是问题。
 * 但 App 每轮仍是「读版本 → 1.05s 后主动 unsubscribe 两条并断开」——说明它
 * 拿到字符串后在**本地判了版本不合法**，连数据通道都不去开。
 *
 * 官方 mi_service_server.c 这一条是 MIBLE_LIB_AND_DEVELOPER_VERSION，
 * 前半段是 mible 库版本、后半段是厂商构建号。App 很可能拿库版本跟自己的
 * mible 比（高于自己 → 认为不兼容）。9.9.9 显然越界，3.0.9 也可能偏高。
 * 真机 MHCB12G 读出来是 3.0.2_0004，那是被官方认可的取值。
 *
 * 所以做成表：每连一次换一条，看哪一条能让 App 继续往下走（出现 001A 订阅
 * 或 wr 001A 就是命中了）。命中后串口 `ver N` 钉住即可。 */
static const char *const s_ver_list[] = {
    "2.1.3_0039",   /* 0 ★云端「最新固件版本」原值（2026-11-19 App 弹窗实测）——
                     *   说明云端认识这台设备且档案里记着这个版本，报一致的值
                     *   最可能让「版本比对」分支通过 */
    "3.0.2_0004",   /* 1 真机 MHCB12G 实测值 */
    "2.1.2_0001",   /* 2 2.1.3 的前一版（验证是否精确匹配） */
    "2.4.1_0001",   /* 3 老一代 mible 2.4 线 */
    "3.0.2_0004",   /* 4 占位 */
    "9.9.9_9999",   /* 5 对照组（已知会被拒） */
};
#define VER_N ((int)(sizeof(s_ver_list) / sizeof(s_ver_list[0])))
static int s_ver_pin = 1;    /* ★2026-11-19 默认钉住 #1 = "3.0.2_0004"（真机实测值）。
                              *   米家侧的判据实测为字符串比较：米家日志
                              *     "device has version too low"  → 直接报 -17 退出
                              *   我们报 2.1.3_0039 时过不了这条，改报 3.0.2_0004 才通过。
                              *   别再改回 #0（那是云端「最新固件」的展示值，不是协议版本）。 */
static int s_ver_next;       /* 轮换游标 */

static void ver_apply(int idx)
{
    const char *s = s_ver_list[idx];
    memset(s_version, 0, sizeof(s_version));
    size_t n = strlen(s);
    if (n > sizeof(s_version)) n = sizeof(s_version);
    memcpy(s_version, s, n);
}

void gatt_miot_set_ver(int idx)
{
    if (idx < 0 || idx >= VER_N) {
        s_ver_pin = -1;
        ESP_LOGW(TAG, "ver: 轮换模式（每条连接换一个候选）");
        return;
    }
    s_ver_pin = idx;
    ver_apply(idx);
    ESP_LOGW(TAG, "ver: 钉住 #%d '%s'", idx, (const char *)s_version);
}

int gatt_miot_ver_count(void) { return VER_N; }
const char *gatt_miot_ver_name(int i) { return (i >= 0 && i < VER_N) ? s_ver_list[i] : "?"; }

static miot_auth_t   *s_auth;
static miot_device_t *s_dev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;
static uint16_t s_ntf_log;      /* 本次连接已打印的 notify 条数（诊断用） */

static uint16_t s_h01, s_h02, s_h14, s_h16;
static uint16_t s_h04, s_h05, s_h10, s_h19, s_h1a, s_h1b, s_h17, s_h18, s_h13, s_h1c;

/* 前向声明：写入分支（001C 应答）在 notify_chr 定义之前就要用到 */
static int notify_chr(uint16_t handle, const uint8_t *d, size_t len);

/* ─────────────────────────────────────────────────────────────────────────
 * ★2026-11-19 23:00 证书下发实验（Q23 的反证）
 *
 * 真机对 0010 的 `40 00 00 00`(LOGIN) 不是回 E1，而是先在 0016 上发一个
 * START 帧 `00 00 00 01 1A 00`（我要发 26 帧），等客户端回 `00 00 01 01`
 * 之后把 451B 的设备证书分 26 帧吐出来。
 *
 * 这里换成我们自己的自签证书（DN/序列号照抄真机，公钥是 dev.key 的）。
 * 目的只有一个：验证 App 到底有没有对设备证书做**链式验签**。
 *   - 若 App 只查 DN 或不查 → 它会继续往下走，并用手上这张证书的公钥加密
 *     下发 Mesh 网络密钥 → 我们能用 dev.key 解开 → 纯 ESP32 走通云端下发；
 *   - 若 App 验签 → 立刻 -17，那就只剩「hook 掉验签」一条路。
 * ───────────────────────────────────────────────────────────────────────── */
static volatile int s_cert_armed;   /* 40 已到，START 已发，等 App ACK */
static volatile int s_cert_go;      /* App 已 ACK，可以吐帧了 */
static volatile int s_blob_armed;   /* 第二段 START 已发，等 App ACK */
static volatile int s_blob_go;      /* 可以吐第二段 84B 了 */
static volatile int s_start2_pending; /* 收到「传输完成」后，等 1.6s 再发第二段 START */
static volatile int s_blob_done;    /* ★第二段已发过：绝不能再发（否则死循环刷 App） */

/* ★2026-11-20 pkg4（设备签名）推送状态机。
 * 反编译 OooO0o.java：App 收齐 pkg7/3/8 后等 20s 收 pkg4，收到的内容
 * 原样进 /v2/blemesh/bind 的 sign 字段（App 不本地验签）。
 * 真机收到 pkg8 后若服务器证书/签名校验失败，在 0010 回 42 00 00 00 中止
 * （replay2.log 14.288s 实证）——之前我们抄了这条 42，等于主动宣告失败，
 * App 就一直卡着等 pkg4 直到超时。正确行为是：收齐 pkg8 → 发 pkg4。 */
static volatile int s_pkg4_armed;   /* pkg4 START 已发，等 App ACK */
static volatile int s_pkg4_go;      /* App 已 ACK，可以吐 pkg4 帧 */

/* ★2026-11-20 真签名：把 pkg4 从「64B 全零」换成用我们自己的 ECDH 私钥
 * 对候选明文做 ECDSA-SHA256。服务器到底签什么没人知道，所以按会话轮换候选，
 * 每次米家重试换下一个，哪个让 bind 放行就钉死（存 NVS）。 */
static uint8_t s_ecdh[32];          /* ECDH(我们的 mesh 私钥, pkg3) = 共享密钥 X */
static volatile int s_ecdh_ok;      /* 共享密钥已算出 */
static uint8_t s_spub[64];          /* pkg3：App/服务器公钥 */
static uint8_t s_chal[64];          /* pkg8：64B 挑战（或是服务器签名，未知） */
static uint8_t s_pkg4buf[64];       /* 本轮要发的 pkg4 */
static int s_pkg4_variant = -1;     /* -1 = 尚未从 NVS 读出 */
static int s_pkg4_used = -1;        /* 本轮实际用的候选号（bind 成功时回写 NVS） */
#define PKG4_VARIANTS 14

/* ── 接收 App 发来的多帧包（2026-11-19 23:5x 真机 oracle 实测）─────────────
 * 设备把证书 + ECDH 公钥发完之后，App 会反过来给我们发三个包：
 *   pkg07 379B  App 证书链（Mijia Root CA → Mijia Cloud），
 *   pkg03  64B  App 的 ECDH 公钥（X‖Y，无 04 前缀），
 *   pkg08  64B  会话令牌。
 * 三段全部收全后，真机在 0010 上回 `42 00 00 00` —— 认证通过。
 * 收包协议与发方向对称：
 *     00 00 00 <pkg> <帧数> 00   →  回 00 00 01 01  (可以发)
 *     [seq][00][≤18B 载荷] × N   →  回 00 00 01 00  (收全)
 * 真机另有 NAK（00 00 01 05 + u16 帧号列表）要求补帧，我们照做。 */
static uint8_t s_rx_pkg;
static uint8_t s_rx_need;
static uint8_t s_rx_got;
static uint16_t s_rx_len;
static uint8_t s_rx_buf[512];

/* ── ECDH 临时密钥对（2026-11-19 23:20 分析结论）───────────────────────
 * 真机那 84B 不是「签名」，实测拆解为 [20B 头] + [64B P-256 公钥]：
 * 随手挑 64 字节命中曲线点的概率是 2^-256，所以那 64B 必然是公钥。
 * 也就是说 40 之后走的是 ECDH 密钥交换（App 回的 pkg3 同样是一个合法
 * 曲线点）。我们把公钥换成**自己的那对**（私钥见 eph_key.h），这样
 * App 用它算出的会话密钥我们也能算出来 —— 后续加密下发的 Mesh 网络
 * 密钥才有机会解开。20B 头原样照抄真机（它决定 App 认不认这个包）。 */
static void eph_log(void)
{
    char b[140]; int p = 0;
    for (int i = 0; i < 64; i++) p += snprintf(b + p, sizeof(b) - p, "%02X", g_eph_pub[i]);
    ESP_LOGW(TAG, "eph pub  = %s", b);
}

/* ── pkg4 候选明文（2026-11-20 在线试签名）──────────────────────────────
 * 依据：PSK 流程里云端用 dev_pub 验签（docs/psk-auth.md §5），本工程实测
 * 服务器也不绑 mesh 公钥与证书 —— 那么 bind 的 sign 就应该是「设备用自己的
 * mesh 私钥签一段双方都知道的串」。真实串未知，这里按可能性排序轮换。
 * 每轮 = 一次米家添加；bind 成功会收到 0016 的 43 00 00 00，自动钉死。 */
static const char *pkg4_variant_name(int v)
{
    /* ★2026-11-20 第2轮：反编译 o000oOoO(BleSecurityDoubleFactorSharedLogin)
     * 揭示同族流程里设备签的是 **ECDH 共享密钥**（不是随机挑战）：
     *     i3g.o000oOoO(sharedSecret, devPub, sig)
     * mesh 场景下服务器自己生成临时密钥对（auth 返回 pub=pkg3），所以云端
     * 也能算出同一个 S → 用 dev_mesh_pub 验签完全成立。0~3 就是 S 家族。 */
    static const char *n[PKG4_VARIANTS] = {
        "S(ecdh)", "S-as-digest", "sha256(S)", "S+spub",
        "spub+S", "chal(pkg8)", "dpub+spub", "spub+dpub",
        "blob84", "dpub", "spub", "chal+dpub", "chal+spub", "dpub+chal"
    };
    return (v >= 0 && v < PKG4_VARIANTS) ? n[v] : "?";
}

static int build_pkg4_msg(int v, uint8_t *out, size_t cap)
{
    const uint8_t *dpub = ID_BLOB + 20;      /* 84B 里那 64B P-256 公钥 */
    size_t n = 0;
    if (v <= 4 && !s_ecdh_ok) return -1;     /* S 家族：没算出共享密钥就不签 */
    switch (v) {
    case 0: n = 32; memcpy(out, s_ecdh, 32); break;
    case 1: n = 32; memcpy(out, s_ecdh, 32); break;
    case 2: mesh_sha256(s_ecdh, 32, out); n = 32; break;
    case 3: memcpy(out, s_ecdh, 32); memcpy(out + 32, s_spub, 64); n = 96; break;
    case 4: memcpy(out, s_spub, 64); memcpy(out + 64, s_ecdh, 32); n = 96; break;
    case 5: n = 64; memcpy(out, s_chal, 64); break;
    case 6: memcpy(out, dpub, 64); memcpy(out + 64, s_spub, 64); n = 128; break;
    case 7: memcpy(out, s_spub, 64); memcpy(out + 64, dpub, 64); n = 128; break;
    case 8: n = ID_BLOB_LEN; memcpy(out, ID_BLOB, ID_BLOB_LEN); break;
    case 9: n = 64; memcpy(out, dpub, 64); break;
    case 10: n = 64; memcpy(out, s_spub, 64); break;
    case 11: memcpy(out, s_chal, 64); memcpy(out + 64, dpub, 64); n = 128; break;
    case 12: memcpy(out, s_chal, 64); memcpy(out + 64, s_spub, 64); n = 128; break;
    case 13: memcpy(out, dpub, 64); memcpy(out + 64, s_chal, 64); n = 128; break;
    default: return -1;
    }
    return (int)(n <= cap ? n : -1);
}

#define PKG4_SCHEMA 3      /* 候选表版本：变了就作废旧轮次，从 0 重新试 */

static int nvs_get_variant(void)
{
    nvs_handle_t h;
    uint8_t v = 0, sv = 0;
    if (nvs_open("meshpkg4", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "v", &v) != ESP_OK) v = 0;
        if (nvs_get_u8(h, "sv", &sv) != ESP_OK) sv = 0;
        nvs_close(h);
    }
    if (sv != PKG4_SCHEMA) {
        ESP_LOGW(TAG, "候选表已更新(schema %u->%u)，从 variant=0 重新开始", sv, PKG4_SCHEMA);
        return 0;
    }
    return v % PKG4_VARIANTS;
}

static void nvs_put_variant(int v)
{
    nvs_handle_t h;
    if (nvs_open("meshpkg4", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "v", (uint8_t)(((v % PKG4_VARIANTS) + PKG4_VARIANTS) % PKG4_VARIANTS));
        nvs_set_u8(h, "sv", (uint8_t)PKG4_SCHEMA);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 生成 pkg4：按当前候选号签名，并把候选号 +1 存回（下一轮自动换下一个） */
static void make_pkg4(void)
{
    if (s_pkg4_variant < 0) s_pkg4_variant = nvs_get_variant();
    uint8_t msg[256];
    int n = build_pkg4_msg(s_pkg4_variant, msg, sizeof(msg));
    int sret;
    if (n == 32 && s_pkg4_variant == 1) {
        sret = mesh_sign_digest(msg, s_pkg4buf);      /* 把 S 当哈希值签 */
    } else {
        sret = mesh_sign_sha256(msg, (size_t)n, s_pkg4buf);
    }
    if (n <= 0 || sret != 0) {
        ESP_LOGE(TAG, "pkg4 签名失败(variant=%d, n=%d) -> 发全零", s_pkg4_variant, n);
        memset(s_pkg4buf, 0, sizeof(s_pkg4buf));
    } else {
        ESP_LOGW(TAG, "pkg4 已签名：variant=%d(%s) 明文 %dB",
                 s_pkg4_variant, pkg4_variant_name(s_pkg4_variant), n);
    }
    {
        char b[140]; int p = 0;
        for (int i = 0; i < 64; i++) p += snprintf(b + p, sizeof(b) - p, "%02X", s_pkg4buf[i]);
        ESP_LOGW(TAG, "pkg4 = %s", b);
    }
    s_pkg4_used = s_pkg4_variant;
    s_pkg4_variant = (s_pkg4_variant + 1) % PKG4_VARIANTS;
    nvs_put_variant(s_pkg4_variant);   /* 预置下一轮；bind 成功时会写回 used */
}

/* 通用多帧推送：START 已由调用方发过，这里只负责 [seq][00][≤18B] 逐帧吐 */
static void push_frames(const uint8_t *d, size_t len, const char *what)
{
    const size_t chunk = 18;
    size_t off = 0;
    uint8_t seq = 1;
    while (off < len) {
        uint8_t f[20];
        size_t n = len - off;
        if (n > chunk) n = chunk;
        f[0] = seq++;
        f[1] = 0x00;
        memcpy(f + 2, d + off, n);
        notify_chr(s_h16, f, n + 2);
        off += n;
        vTaskDelay(pdMS_TO_TICKS(25));   /* 真机 ~30ms，别把 App 灌爆 */
    }
    ESP_LOGW(TAG, "%s push done: %u frames / %u bytes", what, seq - 1, (unsigned)len);
}

static void cert_push_task(void *arg)
{
    (void)arg;
    eph_log();
    {
        int rc = mesh_sign_selftest();
        ESP_LOGW(TAG, "pkg4 签名自检: %s (rc=%d)", rc == 0 ? "OK" : "失败", rc);
    }
    for (;;) {
        if (s_cert_go) {
            s_cert_go = 0;
            push_frames(ID_CERT, ID_CERT_LEN, "cert");
            /* 真机的时序：App 回「传输完成」`00 00 01 00` 之后约 1.6s，
             * 设备才主动发起第二段 START。这里用标志位等那句 ACK。 */
        }
        if (s_start2_pending) {
            s_start2_pending = 0;
            vTaskDelay(pdMS_TO_TICKS(1600));
            const uint8_t start2[6] = { 0x00, 0x00, 0x00, 0x03,
                                        (uint8_t)((ID_BLOB_LEN + 17) / 18), 0x00 };
            ESP_LOGW(TAG, "-> 第二段 START(%u frames) 等 ACK",
                     (unsigned)((ID_BLOB_LEN + 17) / 18));
            if (s_h16) notify_chr(s_h16, start2, sizeof(start2));
            s_blob_armed = 1;
        }
        if (s_blob_go) {
            s_blob_go = 0;
            /* ★2026-11-19 23:44 回退：84B 原样照抄真机。
             * 上一轮把 20B 之后的 64B 换成我们自己的 ECDH 公钥，App 虽然收下、
             * 也回了它自己的三个包，但收到 `42` 之后彻底静默 —— 最可能的原因是
             * 它把「证书里的公钥」和「84B 里的公钥」做了比对，不一致就静默中止。
             * 先按真机原样发，确认能过 42 这一关再谈换钥。 */
            push_frames(ID_BLOB, ID_BLOB_LEN, "auth-blob(自签身份)");
            s_blob_done = 1;    /* 只发一次，绝不再发 */
        }
        if (s_pkg4_go) {
            s_pkg4_go = 0;
            push_frames(s_pkg4buf, sizeof(s_pkg4buf), "pkg4(设备签名)");
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void cert_push_init(void)
{
    static int inited;
    if (inited) return;
    inited = 1;
    xTaskCreate(cert_push_task, "certpush", 6144, NULL, 5, NULL);
}

/* 0x0002 Product ID（真机只读，小端 2 字节）。串口 `pid` 命令改 PID 时同步。 */
static uint8_t s_pid_le[2] = { 0x66, 0x62 };
void gatt_miot_set_pid(uint16_t pid) { s_pid_le[0] = (uint8_t)(pid & 0xFF); s_pid_le[1] = (uint8_t)(pid >> 8); }

/* 0x0013 Device ID：App 绑定确认后会把 20B did 写进来，之后再读回校验。
 * ★必须转存到 auth 层（miot_auth_set_did）——device info 帧靠它声明身份，
 *   只存在 GATT 层的话 auth->id.did 永远是 00 00 00 00。 */
static uint8_t s_devid[20];
static uint8_t s_devid_len;

/* 各级通道的收发计数，出问题时先看这里：写得进来但没有回包，通常是状态机
 * 停在别的分支；一条写都没有，则是 App 压根没认这个服务。 */
static uint32_t s_wr10, s_wr19, s_wr1a, s_wr1b, s_wr1c;

/* 0x0010 是同一条通道上的多路复用：认证命令（A4/A2/24，长度 1 或 4）
 * 和认证之后的数据帧（spec，ag2 封装，首字节 0x00）都从这里进来。
 * 按已知认证命令白名单分流，剩下的都交给 spec 层。 */
static int is_auth_cmd(const uint8_t *d, uint16_t len)
{
    if (len == 1 && d[0] == 0xA4) return 1;                    /* 建链 */
    if (len != 4) return 0;
    if (d[1] || d[2] || d[3]) return 0;
    /* 0x15 = App 索要设备公钥；0x13 = 绑定结果。
     * ★这两个必须放行到 auth 层：漏掉 0x15 会被当成 spec 命令丢进
     *   miot_device_on_rx，表现为 App 连发 4 次都无应答、16s 后 -55
     *   （2026-11-16 meshbulb 实测：串口只见 rx#N，没有任何 [miot-auth] 行）。 */
    if (d[0] == 0x15 || d[0] == 0x13) return 1;
    return d[0] == 0x24 || (d[0] & 0x7F) == 0x22;              /* LOGIN / REGISTER */
}

static void log_hex(const char *tag, const uint8_t *b, size_t n)
{
    char hx[3 * 64 + 1];
    int p = 0;
    for (size_t i = 0; i < n && i < 64; i++)
        p += snprintf(hx + p, sizeof(hx) - p, "%02X ", b[i]);
    if (p) hx[p - 1] = 0;
    ESP_LOGW(TAG, "  %s %s", tag, hx);
}

/* relay 模式：真灯 notify → 手机 */
int gatt_miot_relay_notify(uint16_t uuid16, const uint8_t *d, size_t len)
{
    uint16_t h = 0;
    switch (uuid16) {
    case 0x0010: h = s_h10; break;
    case 0x0016: h = s_h16; break;
    case 0x001A: h = s_h1a; break;
    case 0x001B: h = s_h1b; break;
    case 0x001C: h = s_h1c; break;
    default: return -1;
    }
    if (!h) return -1;
    ESP_LOGW(TAG, "RELAY <- lamp %04X %uB %02X %02X %02X %02X",
             uuid16, (unsigned)len,
             len > 0 ? d[0] : 0, len > 1 ? d[1] : 0,
             len > 2 ? d[2] : 0, len > 3 ? d[3] : 0);
    int rc = notify_chr(h, d, len);
    if (rc) ESP_LOGW(TAG, "  <- 转发给手机失败 rc=%d h=%u len=%u", rc, h, (unsigned)len);
    return rc;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* ★2026-11-16 绑定成功后重连：手机连上 → 1.6s 内一个字节都不写就断开
         *   （reason=0x213 远端主动断），lyra 侧 mtu=20 / service 空 / devId unknow。
         *   此前这些特征只有 WRITE|NOTIFY、没有 READ，ATT 层直接回 0x02
         *   「Read Not Permitted」，任何读探针都会立刻失败。这里放行并打日志，
         *   用来确认 App 到底有没有读、读的是哪一条。 */
        uint16_t ru = ble_uuid_u16(ctxt->chr->uuid);
        uint8_t rbuf[4] = { 0x02, 0x00, 0x00, 0x00 };   /* version=2, caps=0 */
        /* offset 必须打出来：App 若走 READ_BLOB（长读），会先 offset=0 再 offset=20，
         * 我们若无条件从头返回 20B，App 就收到 40B 重复数据 → 解析失败。 */
        ESP_LOGI(TAG, "rd %04X op=%d off=%u mtu=%u", ru, ctxt->op,
                 (unsigned)ctxt->offset, (unsigned)s_mtu);
        /* ★ 注意：这里的 om 进来时已含 1 字节 ATT_OP_READ_RSP(0x0B)，
         *   所以 append 20B 后 PKTLEN=21 是**正确**的 PDU 长度，不要去清。
         *   （2026-11-19 曾以为是脏数据清过一次，结果 App 直接停止认证 ——
         *    等于把 opcode 吃掉了。） */
        if (ru == 0x0005) {
            /* 真机行为实测：属性里写了 READ，实际读回 Read Not Permitted。
             * 照抄，别自作聪明给数据 —— App 对这条的期望就是「读不到」。 */
            ESP_LOGI(TAG, "rd 0005 -> Read Not Permitted (照真机)");
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        if (ru == 0x0002) {
            return os_mbuf_append(ctxt->om, s_pid_le, sizeof(s_pid_le)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ru == 0x0014) {
            /* Beacon Key：App 连上后读它解 MiBeacon 广播。没有 token 时给全 0
             * （multi_dev 同款兜底），不影响连接与页面数据。 */
            static const uint8_t zero[16] = { 0 };
            return os_mbuf_append(ctxt->om, zero, sizeof(zero)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ru == 0x001B) {
            /* spec 版本字节：真机 0x02。缺这条读会让 App 在读回调里拿到 null。 */
            static const uint8_t spec_ver = 0x02;
            return os_mbuf_append(ctxt->om, &spec_ver, 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ru == 0x0004) {
            /* 官方语义：定长 20B ASCII + 尾部 0x00 填充（见 s_version 注释）。 */
            const uint16_t total = (uint16_t)sizeof(s_version);
            if (ctxt->offset >= total) {
                ESP_LOGW(TAG, "  -> ver off=%u >= %u, reply empty", (unsigned)ctxt->offset, total);
                return 0;
            }
            const uint16_t n = (uint16_t)(total - ctxt->offset);
            int rc = os_mbuf_append(ctxt->om, s_version + ctxt->offset, n);
            ESP_LOGI(TAG, "  -> ver '%s' off=%u send=%uB rc=%d om=%uB",
                     (const char *)s_version, (unsigned)ctxt->offset, n, rc,
                     (unsigned)OS_MBUF_PKTLEN(ctxt->om));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ru == 0x0013) {
            /* did 读回：先看 App 写进来的那份，没有就用认证层持久化的，
             * 都没有才回空 —— 回错会让 App 认为这是另一台设备。 */
            if (s_devid_len) {
                return os_mbuf_append(ctxt->om, s_devid, s_devid_len) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            const miot_auth_identity_t *id = s_auth ? miot_auth_identity(s_auth) : NULL;
            if (id) {
                size_t n = sizeof(id->did);
                while (n && id->did[n - 1] == 0x00) n--;
                if (n) {
                    return os_mbuf_append(ctxt->om, id->did, n) == 0
                           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            }
            return 0;
        }
        if (ru == 0x0010 || ru == 0x0019) {
            return os_mbuf_append(ctxt->om, rbuf, sizeof(rbuf)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;   /* 其余读返回空值 */
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* relay 模式：0010/0016/001A/001B/001C 的写全部转给真灯，本机状态机不参与 */
    if (relay_active() && relay_forward(u, buf, got)) {
        ESP_LOGW(TAG, "RELAY -> lamp %04X %uB", u, (unsigned)got);
        return 0;
    }

    switch (u) {
    case 0x0010:
        s_wr10++;
        /* 命令很短（1B 或 4B），打印首字节足够定位是哪一步 */
        ESP_LOGI(TAG, "wr 0010 #%lu len=%u cmd=%02X", (unsigned long)s_wr10, got, got ? buf[0] : 0);
        if (got && got <= 64) {
            char hx[3 * 64 + 1];
            int p = 0;
            for (uint16_t i = 0; i < got; i++) p += snprintf(hx + p, sizeof(hx) - p, "%02X ", buf[i]);
            if (p) hx[p - 1] = 0;
            ESP_LOGI(TAG, "  hex10 %s", hx);
        }
        /* ★2026-11-19 真机实测（PC 扮 App 直连 14:D8:81:E6:95:3C）：
         *   0010 <- 40 00 00 00  ⇒  0010 回 E1 00 00 00
         *   E1 = 0xE0|0x01，小米协议里的 RESULT 帧（成功）。
         *   我们此前没有这条应答，App 发完等 16 秒无响应直接断链，
         *   米家报「配网失败 -7」。这是整个 -7 的最后一环。 */
        if (got == 4 && buf[0] == 0x40 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0) {
            /* ★2026-11-19 23:00 真机实测（PC 扮 App 直连真机）：
             *   0010 <- 40 00 00 00  ⇒  0016 回 START 帧 00 00 00 01 1A 00
             *   客户端回 00 00 01 01 之后，设备分 26 帧吐 451B 证书。
             *   之前我们回 E1 00 00 00（RESULT ok）是错的 —— 真机根本不发那个，
             *   App 等的是证书。这里按真机行为改成 START + 25 帧自签证书。 */
            const uint8_t frames = (uint8_t)((ID_CERT_LEN + 17) / 18);
            uint8_t start[6] = { 0x00, 0x00, 0x00, 0x01, frames, 0x00 };
            cert_push_init();
            s_cert_armed = 1;
            s_cert_go = 0;
            s_blob_armed = 0;
            s_blob_go = 0;
            s_start2_pending = 0;
            s_blob_done = 0;            /* 新一轮会话，允许再走一次第二段 */
            s_pkg4_armed = 0;
            s_pkg4_go = 0;              /* pkg4 每次会话只推一次 */
            s_ecdh_ok = 0;              /* 共享密钥每轮重算（pkg3 会变） */
            memset(s_ecdh, 0, sizeof(s_ecdh));
            ESP_LOGW(TAG, "wr 0010 cmd=40 -> START(%u frames) on 0016, 等 ACK", frames);
            if (s_h16) notify_chr(s_h16, start, sizeof(start));
            break;
        }
        if (is_auth_cmd(buf, got)) {
            if (s_auth) miot_auth_on_0010(s_auth, buf, got);
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x0019:
        s_wr19++;
        ESP_LOGI(TAG, "wr 0019 #%lu len=%u cmd=%02X", (unsigned long)s_wr19, got, got ? buf[0] : 0);
        if (got && got <= 64) {
            char hx[3 * 64 + 1];
            int p = 0;
            for (uint16_t i = 0; i < got; i++) p += snprintf(hx + p, sizeof(hx) - p, "%02X ", buf[i]);
            if (p) hx[p - 1] = 0;
            ESP_LOGI(TAG, "  hex19 %s", hx);
        }
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x001A:
        s_wr1a++;
        ESP_LOGI(TAG, "wr 001A #%lu len=%u cmd=%02X", (unsigned long)s_wr1a, got, got ? buf[0] : 0);
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    case 0x001B:
        s_wr1b++;
        ESP_LOGI(TAG, "wr 001B #%lu len=%u cmd=%02X", (unsigned long)s_wr1b, got, got ? buf[0] : 0);
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    case 0x001C:
        /* Mesh 配网通道：真模组上 App 在这里下发私有 Provision 报文。
         * 内容**原样完整打印**（最多 128B）——复刻 Mesh 配网协议的一手证据。 */
        s_wr1c++;
        ESP_LOGW(TAG, "wr 001C #%lu len=%u", (unsigned long)s_wr1c, got);
        if (got) {
            char hx[3 * 128 + 1];
            int p = 0;
            for (uint16_t i = 0; i < got && i < 128; i++)
                p += snprintf(hx + p, sizeof(hx) - p, "%02X ", buf[i]);
            if (p) hx[p - 1] = 0;
            ESP_LOGW(TAG, "  hex1C %s", hx);
        }
        /* ★2026-11-19 真机实测（PC 扮 App 直连真机）：
         *   001C <- 00  ⇒  001C 回 00 02 03 08（4B）
         *   真机回了这一包，App 才继续下发 3B → 6B → 66B 的 Mesh 私有配网报文。
         *   我们一直不回，App 就判定「这台不是 Mesh 可配网的」，回落到
         *   普通 BLE 认证（A4 → 40 → E1），然后在最后一步按产品类型拒绝，
         *   报 -17。所以这一包是进入 Mesh 配网路径的门票。 */
        if (got == 1 && buf[0] == 0x00) {
            static const uint8_t c1c_rsp[4] = { 0x00, 0x02, 0x03, 0x08 };
            ESP_LOGW(TAG, "001C <- 00 -> 回 00 02 03 08（照真机，进 Mesh 配网）");
            notify_chr(s_h1c, c1c_rsp, sizeof(c1c_rsp));
            break;
        }
        /* ★2026-11-19 22:05 真机 oracle 实证（PC 直连 14:D8:81:E6:95:3C，
         * 配网窗口内，连接后 1s 内问出，真机 26ms 就答）：
         *   001C <- 08 01 00   ⇒   001C 回 08 06 00 68 65 6C 6C 6F
         *                            = [08][len=06][00]["hello"]
         * 也就是说这条通道是**明文命令通道**，不是加密配网：App 用 08 01 00
         * 问「你在吗」，设备必须回一句 hello 才算通过；我们不回，App 等 2.5s
         * 就判定「这个设备不可 Mesh 配网」，回落到 A4/40 降级路径，最后 -17。
         * 帧格式：[op=08][payload 长度][序号/参数…][数据…] */
        if (got >= 2 && buf[0] == 0x08 && buf[1] == 0x01) {
            static const uint8_t hello[8] = { 0x08, 0x06, 0x00, 'h', 'e', 'l', 'l', 'o' };
            ESP_LOGW(TAG, "001C <- 08 01 %02X -> 回 hello（照真机）", got > 2 ? buf[2] : 0);
            notify_chr(s_h1c, hello, sizeof(hello));
            break;
        }
        /* ★2026-11-19 22:18 真机 oracle 全表实测（一次连接扫完 00~0F）：
         *   写 03          ⇒ 回 03 07 4D 48 43 42 31 32 47 = [03][len=07]"MHCB12G"
         *                     —— App 在问「你是什么模组」，答得上型号才继续
         *   写 01/02/04..0F ⇒ 回 FF 02 <命令字> 02（未支持 NAK）
         *   写 08（长度错） ⇒ 回 FF 02 08 08（参数错）
         * 帧格式统一为 [op][payload len][payload…]；FF 开头是错误帧。 */
        if (got == 1 && buf[0] == 0x03) {
            static const uint8_t model[9] = { 0x03, 0x07, 'M', 'H', 'C', 'B', '1', '2', 'G' };
            ESP_LOGW(TAG, "001C <- 03 -> 回 MHCB12G（照真机）");
            notify_chr(s_h1c, model, sizeof(model));
            break;
        }
        if (got == 1) {
            const uint8_t nak[4] = { 0xFF, 0x02, buf[0], 0x02 };
            ESP_LOGW(TAG, "001C <- %02X -> 回 NAK（照真机）", buf[0]);
            notify_chr(s_h1c, nak, sizeof(nak));
            break;
        }
        break;
    case 0x0001:
        /* 真机有这条（write+notify，无 READ）：老型号「前绑定」下发 12B token。
         * 我们走标准认证，不在它上面跑业务，但必须存在且不报错。 */
        ESP_LOGW(TAG, "wr 0001 len=%u (token channel, ignored)", got);
        break;
    case 0x0016:
        /* ★2026-11-19 22:27 实测：这条不只是「观测点」，它就是**认证数据通道**。
         * App 收到 mng 后 0ms 就回写 00 00 05 00 06 12（mng_ack），
         * 之前我们只打日志把它丢了 → App 在 0016 上干等 5 秒，然后盲发 40 → -17。
         * 现在把 0016 的写入交给认证层：mng_ack 会触发 20B 建链测试包，
         * App 回执 code=1 后设备再发 device info —— 这正是标准认证的建链三步。 */
        ESP_LOGI(TAG, "wr 0016 len=%u (auth channel)", got);
        if (got) log_hex("hex16", buf, got);
        /* ★2026-11-20 反编译 t41（default 分支，bind 响应）：bind 成功后
         * App 在 0016 写 43 00 00 00（sendRegVerifySuccessToDevice）。
         * 真机收到后做什么需要抓包确认，先记日志。 */
        if (got == 4 && buf[0] == 0x43 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0) {
            ESP_LOGW(TAG, "0016 <- 43 00 00 00 = bind 成功！(RegVerifySuccess)");
            if (s_pkg4_used >= 0) {
                ESP_LOGW(TAG, "★★ pkg4 候选命中：variant=%d(%s) —— 已写入 NVS",
                         s_pkg4_used, pkg4_variant_name(s_pkg4_used));
                nvs_put_variant(s_pkg4_used);
                s_pkg4_variant = s_pkg4_used;   /* 之后每轮都用命中项 */
            }
            break;
        }
        /* ★2026-11-19 23:5x 从真机取回的完整收包协议（详见 s_rx_* 注释）。
         * 先处理「App 发给我们」的 START / 数据帧，再处理 ACK。 */
        if (got == 6 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[5] == 0x00) {
            static const uint8_t ready[4] = { 0x00, 0x00, 0x01, 0x01 };
            s_rx_pkg = buf[3];
            s_rx_need = buf[4];
            s_rx_got = 0;
            s_rx_len = 0;
            ESP_LOGW(TAG, "0016 <- START pkg=%u 帧数=%u -> 回 00 00 01 01",
                     s_rx_pkg, s_rx_need);
            notify_chr(s_h16, ready, sizeof(ready));
            break;
        }
        if (s_rx_need && got >= 3 && buf[0] >= 1 && buf[1] == 0x00) {
            static const uint8_t done[4] = { 0x00, 0x00, 0x01, 0x00 };
            size_t pl = (size_t)got - 2;
            if (s_rx_len + pl <= sizeof(s_rx_buf)) {
                memcpy(s_rx_buf + s_rx_len, buf + 2, pl);
                s_rx_len += (uint16_t)pl;
            }
            s_rx_got++;
            if (s_rx_got >= s_rx_need) {
                ESP_LOGW(TAG, "0016 <- pkg=%u 收全 %u 帧 / %u 字节",
                         s_rx_pkg, s_rx_got, (unsigned)s_rx_len);
                notify_chr(s_h16, done, sizeof(done));
                if (s_rx_pkg == 0x03 && s_rx_len >= 64) {
                    /* ★服务器临时公钥（auth 返回的 pub）。用它和我们自己的 mesh
                     * 私钥做 ECDH，得到与云端一致的共享密钥 S —— 反编译
                     * o000oOoO 证明同族流程里设备签的就是这个 S。 */
                    memcpy(s_spub, s_rx_buf, 64);
                    s_ecdh_ok = (mesh_ecdh_shared(s_spub, s_ecdh) == 0);
                    if (s_ecdh_ok) {
                        char b[80]; int p = 0;
                        for (int i = 0; i < 32; i++) p += snprintf(b + p, sizeof(b) - p, "%02X", s_ecdh[i]);
                        ESP_LOGW(TAG, "pkg3(spub) 已存；ECDH 共享密钥 S = %s", b);
                    } else {
                        ESP_LOGE(TAG, "pkg3(spub) 已存，但 ECDH 计算失败");
                    }
                }
                if (s_rx_pkg == 0x08) {
                    /* ★2026-11-20 修正：真机在 0010 回 42 00 00 00 是「服务器
                     * 证书/签名校验失败」的中止帧（replay2.log：真机收到与
                     * 会话不符的 pkg7/3/8 才回 42），不是「认证通过」。
                     * App 此时在等 pkg4（等 20s，OooO0o.java startAction(29)），
                     * 收到 pkg4 后原样上送 /v2/blemesh/bind。
                     * 正确行为：发 pkg4 START，等 App 回 00 00 01 01 再吐帧。 */
                    if (s_rx_len >= 64) {
                        memcpy(s_chal, s_rx_buf, 64);
                    }
                    make_pkg4();       /* 用候选明文做 ECDSA-SHA256 真签名 */
                    const uint8_t frames = (uint8_t)((sizeof(s_pkg4buf) + 17) / 18);
                    uint8_t start4[6] = { 0x00, 0x00, 0x00, 0x04, frames, 0x00 };
                    ESP_LOGW(TAG, "pkg8 收齐 -> 发 pkg4 START(%u frames) 等 ACK", frames);
                    s_pkg4_armed = 1;
                    s_pkg4_go = 0;
                    notify_chr(s_h16, start4, sizeof(start4));
                }
                if (s_rx_pkg == 0x09) {
                    /* ★pkg9 = Mesh 配网数据（App 在 bind 成功后下发）。
                     * 反编译 OooOOOO：设备在 0016 回 41 00 00 00 即成功，
                     * App 随后走 /v2/blemesh/provision_done 收尾。 */
                    static const uint8_t ok41[4] = { 0x41, 0x00, 0x00, 0x00 };
                    ESP_LOGW(TAG, "pkg9(配网数据) 收齐 -> 0016 回 41 00 00 00（配网成功）");
                    notify_chr(s_h16, ok41, sizeof(ok41));
                }
                s_rx_need = 0;
            }
            break;
        }
        /* ★证书/签名的多帧 ACK：`00 00 01 01` = 请开始发；`00 00 01 00` = 传输完成。
         * 真机两段传输都是这个套路，用 buf[3] 区分「开始」和「完成」。 */
        if (got >= 4 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) {
            if (buf[3] == 0x01 && s_cert_armed) {
                ESP_LOGW(TAG, "0016 <- ACK(cert START) -> 开始推证书");
                s_cert_armed = 0;
                s_cert_go = 1;
                break;
            }
            if (buf[3] == 0x01 && s_blob_armed) {
                ESP_LOGW(TAG, "0016 <- ACK(2nd START) -> 开始推签名");
                s_blob_armed = 0;
                s_blob_go = 1;
                break;
            }
            if (buf[3] == 0x01 && s_pkg4_armed) {
                ESP_LOGW(TAG, "0016 <- ACK(pkg4 START) -> 开始推 pkg4");
                s_pkg4_armed = 0;
                s_pkg4_go = 1;
                break;
            }
            if (buf[3] == 0x00) {
                /* ★2026-11-19 23:20 修复死循环：第二段发完后 App 还会回一次
                 * 「传输完成」，之前无条件再排一次 START → 每 1.6s 重发一遍
                 * 84B，把 App 的收包节奏彻底打乱（log 里 97 轮全是这个循环）。 */
                if (s_blob_done) {
                    ESP_LOGW(TAG, "0016 <- 传输完成 ACK（第二段已发完，静默等待 App 下一步）");
                } else {
                    ESP_LOGW(TAG, "0016 <- 传输完成 ACK -> 1.6s 后发第二段 START");
                    s_start2_pending = 1;
                }
                break;
            }
        }
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x0017:
    case 0x0018:
        /* OTA 控制点 / 数据。官方服务表里就有这两条（0x0017 WRITE|NOTIFY、
         * 0x0018 WRITE_NR|NOTIFY），真机也都在。我们不做 OTA，但不能回 ATT 错误
         * ——插件探测到写失败会当成「设备不健康」。收下、记一笔、静默。 */
        ESP_LOGI(TAG, "wr %04X len=%u (OTA ignored)", u, got);
        break;
    case 0x0013:
        /* App 在绑定确认后写 20B did；转存给 auth 层，device info 才有身份 */
        if (got <= sizeof(s_devid)) {
            memcpy(s_devid, buf, got);
            s_devid_len = (uint8_t)got;
            if (s_auth) miot_auth_set_did(s_auth, buf, got);
        }
        ESP_LOGW(TAG, "wr 0013 did len=%u: %02X %02X %02X %02X ...",
                 got, got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0,
                 got > 2 ? buf[2] : 0, got > 3 ? buf[3] : 0);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

/* ★2026-11-19 真机 12 条特征表（顺序与属性 1:1 对齐，依据 esp32/multi_dev
 * 的现场实测记录：真机表如下，少一条/属性不一致，App 服务发现后就走错分支）：
 *
 *   0001 W|WNR|N   0002 R        0004 R       0010 W|WNR|N
 *   0013 R|W|WNR   0014 R        0016 R|W|WNR|N  0017 R|W|WNR|N
 *   0018 R|W|WNR|N 0019 W|WNR|N  001A W|WNR|N    001B W|WNR|N
 *
 * 注意 0x0010/0x0019 真机**没有 READ**（之前我们给了 READ，属性表就不一致），
 * 0x001B 真机**有 NOTIFY**（我们之前漏了，App 订阅 001B 会直接失败）。 */
static const struct ble_gatt_chr_def s_chrs[] = {
    /* ★2026-11-19 整表按真机（PID 0x6ABC / MHCB12G 出厂固件）逐条对齐，
     *   顺序、属性位都照抄 PC 直连 dump 的结果。Mesh 设备与普通 BLE 设备的
     *   特征表本来就不同，照普通 BLE 那套摆（多带 0019/0001/0002/0013/0014）
     *   会让 App 走错配网分支。 */
    {   /* 0004 Version：定长 20B，真机是 "3.0.2_0004" + 0 填充 */
        .uuid = &s_c04_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h04,
    },
    {   /* 0010 控制点：真机只有 notify + write-without-response */
        .uuid = &s_c10_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h10,
    },
    {   /* 0016 安全级通道：真机 notify + write-without-response */
        .uuid = &s_c16_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h16,
    },
    {   /* 0005 真机独有（READ|NOTIFY），实读返回 Read Not Permitted */
        .uuid = &s_c05_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h05,
    },
    {   /* 0017 OTA 控制点：真机 notify + write（无 write-no-rsp） */
        .uuid = &s_c17_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h17,
    },
    {   /* 0018 OTA 数据：真机 notify + write-without-response */
        .uuid = &s_c18_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h18,
    },
    {   /* 001A：真机 notify + write-without-response */
        .uuid = &s_c1a_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1a,
    },
    {   /* 001B：真机 notify + write-without-response */
        .uuid = &s_c1b_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1b,
    },
    {   /* 001C Mesh 私有配网通道：真机 notify + write-without-response */
        .uuid = &s_c1c_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1c,
    },
    { 0 },
};


/* ================= Nordic UART (NUS) =================
 * ★2026-11-18：型号 `careco.nlight.o654` 带 `nlight`，和飞利浦系夜灯同源。
 *   实测夜灯2 的插件（philips.light.nlight）根本不走 FE95 spec，而是
 *   CALLBLE cmd=6/15 找 NUS 服务 6E400001，char 6E400003 收发**纯文本**命令：
 *     get_prop power bright lux snm dur / set_bright 58 / notify_ol ...
 *   我们只有 FE95 时，插件在本地服务发现就返回 -1（50ms，设备端零字节），
 *   表现正是「认证成功 → 读完 0x0004 → 1.2s 优雅断开」。
 *   先只把通道建起来 + 把收到的文本打出来，确认插件到底说不说话。 */
static const ble_uuid128_t s_nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0xe5, 0x0e, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0xe5, 0x0e, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0xe5, 0x0e, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static uint16_t s_nus_tx;

static int nus_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t buf[160];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &got) != 0) return 0;
    buf[got] = 0;
    ESP_LOGW(TAG, "NUS rx %uB: [%s]", got, (const char *)buf);
    return 0;
}

static const struct ble_gatt_chr_def s_nus_chrs[] = {
    {
        .uuid = &s_nus_rx_uuid.u,
        .access_cb = nus_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_nus_tx_uuid.u,
        .access_cb = nus_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_nus_tx,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = s_nus_chrs,
    },
    { 0 },
};

void gatt_miot_register_svcs(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return;
    }
    if (s_ver_pin >= 0) ver_apply(s_ver_pin);   /* 启动即生效，别等首次连接 */
    ESP_LOGI(TAG, "0xFE95 service registered (12 chars: 0001/0002/0004/0010/0013/0014/0016/0017/0018/0019/001A/001B), ver=%s",
             (const char *)s_version);
    ESP_LOGI(TAG, "NUS service registered (6E400001 rx=6E400002 tx=6E400003)");
}

void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev)
{
    s_auth = auth;
    s_dev = dev;
}

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu)
{    s_conn = conn_handle;
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    s_wr10 = s_wr19 = s_wr1a = s_wr1b = s_wr1c = 0;
    s_ntf_log = 0;
    /* 版本轮换：每条新连接换一个候选，App 每 3s 重连一次，几轮就能扫完。
     * 命中判据见文件上方 s_ver_list 注释。 */
    if (s_ver_pin >= 0) {
        ver_apply(s_ver_pin);
    } else {
        ver_apply(s_ver_next % VER_N);
        s_ver_next++;
    }
    ESP_LOGI(TAG, "connected conn=%u mtu=%u ver='%s'",
             conn_handle, s_mtu, (const char *)s_version);
}

void gatt_miot_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected (wr 0010=%lu 0019=%lu 001A=%lu 001B=%lu 001C=%lu)",
             (unsigned long)s_wr10, (unsigned long)s_wr19,
             (unsigned long)s_wr1a, (unsigned long)s_wr1b, (unsigned long)s_wr1c);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = BLE_ATT_MTU_DFLT;
}

void gatt_miot_dump_handles(void)
{
    /* value handle；CCCD 一般紧跟其后（+1/+2 视是否多个描述符而定）。
     * App 的 SUBSCRIBE 事件里给的是 CCCD handle，对照这里就能知道它订的是哪条通道。 */
    ESP_LOGI(TAG, "handles: 0004=%u 0010=%u 0013=%u 0019=%u 001A=%u 001B=%u 001C=%u 0017=%u 0018=%u",
             s_h04, s_h10, s_h13, s_h19, s_h1a, s_h1b, s_h1c, s_h17, s_h18);
}

const char *gatt_miot_handle_tag(uint16_t h)
{
    /* subscribe 事件给的是 CCCD handle，单描述符时 = value handle + 1 */
    if (h == s_h10 + 1) return "0010-cccd";
    if (h == s_h19 + 1) return "0019-cccd";
    if (h == s_h1a + 1) return "001A-cccd";
    if (h == s_h1b + 1) return "001B-cccd(no notify)";
    if (h == s_h1c + 1) return "001C-cccd";
    return "other";
}

void gatt_miot_on_mtu(uint16_t mtu)
{
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    ESP_LOGI(TAG, "mtu -> %u", s_mtu);
}

int gatt_miot_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

static int notify_chr(uint16_t handle, const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || handle == 0) return -1;

    uint16_t max = s_mtu > 3 ? (uint16_t)(s_mtu - 3) : 20;
    if (len > max) {
        /* 协议层没按 MTU 分片，说明对端协商出来的 MTU 比预期小（或没协商）。
         * 直接截断会送出半帧，宁可不发。 */
        ESP_LOGW(TAG, "notify h=%u len=%u > mtu-3=%u, dropped", handle, (unsigned)len, max);
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)len);
    if (!om) return -1;

    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    /* ★不再只记失败：成功/失败都打（每次连接前 40 条），
     * 否则「notify 其实没发出去」会被误判成「App 收到了却不理」。 */
    if (s_ntf_log < 40) {
        s_ntf_log++;
        ESP_LOGI(TAG, "ntf h=%u len=%u rc=%d head=%02X %02X %02X",
                 handle, (unsigned)len, rc,
                 len > 0 ? d[0] : 0, len > 1 ? d[1] : 0, len > 2 ? d[2] : 0);
    }
    return (rc == 0 || rc == BLE_HS_EDONE) ? 0 : -1;
}

int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* ★2026-11-19 按 multi_dev（已跑通）的双通道同发：
     *   spec 应答/事件 → 0x0010 与 0x001B 各发一份，App 侧按 tid 幂等去重；
     *   ag2 链路层应答（4 字节 00 00 01 xx）另外补一路到 0x001A ——
     *   App 把 spec 帧写在 001A 上，就在 001A 等这条 ACK，收不到就一个分片
     *   都不发（页面停在「设备初始化」）。
     *   只发一条通道时，另一边的消费方永远等不到。 */
    notify_chr(s_h10, d, len);
    if (len == 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x01) {
        notify_chr(s_h1a, d, len);
    }
    return notify_chr(s_h1b, d, len);
}

int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h10, d, len);
}

int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* ★2026-11-19 真机特征表里**没有 0x0019**。这块板子的认证通道是 **0x0016**：
     * 实测 App 连上后第一件事就是 subscribe 0016，然后就在 0016 上等设备回包
     * （A4 的 mng 也是从 0016 发出去才被它接住）。所以 0019 不存在时回落到
     * 0016，而不是 0010 —— 发到 0010 那一刻 App 还没订阅，等于白发。 */
    if (s_h19 == 0) {
        if (s_h16) return notify_chr(s_h16, d, len);
        return notify_chr(s_h10, d, len);
    }
    return notify_chr(s_h19, d, len);
}

int gatt_miot_notify_0016(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* 真机把 A4 的 mng 应答从 0016 发出（PC 直连探针实测），照抄。
     * 0016 在表里没注册时退回 0010，避免句柄 0。 */
    if (s_h16 == 0) return notify_chr(s_h10, d, len);
    return notify_chr(s_h16, d, len);
}
