/**
 * @file gatt_miot.c
 * @brief 0xFE95 服务与四个特征值的 NimBLE 实现
 */
#include "gatt_miot.h"

#include <string.h>
#include <stdlib.h>     /* atoi / atol：nlight 文本协议解析 */

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "miot_ble/miot_crypto.h"   /* miot_hkdf_sha256：st=9 的 HKDF 登录密钥候选 */

static const char *TAG = "MIOT-GATT";

/* 服务与特征值 UUID（SIG 分配给小米的 MiBeacon 服务 0xFE95）
 * 米家 App 侧的完整形式是 0000FE95-0000-1000-8000-00805F9B34FB */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFE95);
static const ble_uuid16_t s_c01_uuid = BLE_UUID16_INIT(0x0001);
static const ble_uuid16_t s_c02_uuid = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t s_c04_uuid = BLE_UUID16_INIT(0x0004);
static const ble_uuid16_t s_c14_uuid = BLE_UUID16_INIT(0x0014);
static const ble_uuid16_t s_c10_uuid = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t s_c13_uuid = BLE_UUID16_INIT(0x0013);
static const ble_uuid16_t s_c16_uuid = BLE_UUID16_INIT(0x0016);
static const ble_uuid16_t s_c17_uuid = BLE_UUID16_INIT(0x0017);
static const ble_uuid16_t s_c18_uuid = BLE_UUID16_INIT(0x0018);
static const ble_uuid16_t s_c19_uuid = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t s_c1a_uuid = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t s_c1b_uuid = BLE_UUID16_INIT(0x001B);

/* ===== Nordic UART（6E400001）=====
 * ★2026-11-11 17:25 实机证据：夜灯2（0x03DD）的插件是 philips.light.nlight，
 * 页面初始化时只做两次 BLE 调用，参数都是
 *   service=6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   char   =6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 * 而我们的 GATT 表只有 0xFE95，两次调用都在 App 本地直接返回 -1（设备端零字节
 * 交互），页面因此一直转圈到超时。真机必然暴露了这套 NUS 通道，这里补齐。
 * 注意：UUID 常量按小端字节序书写（最后两字节是 0x01/0x02/0x03 与 0x40,0x6e）。 */
static const ble_uuid128_t s_nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static miot_auth_t   *s_auth;
static miot_device_t *s_dev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

/* 上层可以覆盖（定义强符号）来接管「App 发请求」这个事件。
 * 默认空实现，保证单独编译这个文件也能过。 */
__attribute__((weak)) void miot_demo_app_req_hook(void) { }

/* legacy BOND 完成（App 下发过 token）——由 app_main 实现：把 token 当本机身份
 * 落盘，并让广播切到「已绑定」。弱符号保证单独编译本文件也能过。 */
__attribute__((weak)) void miot_legacy_bound_hook(const uint8_t token[12]) { (void)token; }
/** 收到 12B token 就立刻落盘（不等 BOND）。
 *  BOND 那一帧识别不稳（MAC 序/候选 token 都可能选错），一旦漏掉，token 就
 *  只剩内存里的副本 —— 设备一复位（USB-Serial-JTAG 每次 open 都会复位）就丢，
 *  重连的 GETRAND 只能用 NVS 里的旧 token 应答，App 静默丢弃、1s 后断开。
 *  这个 hook 只写 token + id_save，不动 bound、不切广播，避免干扰绑定判定。 */
__attribute__((weak)) void miot_legacy_token_hook(const uint8_t token[12]) { (void)token; }
__attribute__((weak)) void miot_token_subscribed_hook(void) { }
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;

static uint16_t s_h10, s_h19, s_h1a, s_h1b, s_h02, s_h04, s_h13, s_h01, s_h14;
static uint16_t s_h16, s_h17, s_h18;
static uint16_t s_hn_rx, s_hn_tx;   /* NUS 6E400002 / 6E400003 */

/* 最后一次收到手机写入的时刻（esp_timer ms）。app_main 用它做「手机长时间
 * 没动静才收尾」的看门狗 —— 之前按登录时刻 +60s 硬断，会让 App 每 60s
 * 重连一次，页面永远在「设备初始化中」。 */
static volatile uint32_t s_rx_ms;

/* 真机 GATT 里就有的只读信息特征（mijia-protocol/docs/00-index.md GATT map）。
 * ★App 优化连接模型在 LOGIN 后会 read 0x0004 拿固件版本 —— 上一轮
 * "read version failed" 的元凶就是我们的表里没有 0x0004，GATT read 直接
 * ATT error。0x0002=PID(LE u16)、0x0013=序列号，一并补齐防后续卡点。 */
/* PID 由 app_main 在启动时按 Kconfig 选的型号注入（gatt_miot_set_pid），
 * 不再硬编码 —— 这个工程要模拟 6 种型号。 */
static uint8_t s_pid_le[2] = { 0x00, 0x00 };
static uint16_t s_pid;                              /* mix 密钥要按 PID 算 */

/** 注入当前型号的 PID（0x0002 特征值要按型号回真实值） */
void gatt_miot_set_pid(uint16_t pid)
{
    s_pid = pid;
    s_pid_le[0] = (uint8_t)(pid & 0xFF);
    s_pid_le[1] = (uint8_t)(pid >> 8);
}
/* ★0x0004 版本号必须是**明文** "x.x.x_xxxx"，并且要抬得足够高。
 * 参考工程 v32 的反编译结论：标准认证注册链路里 App 读到 0x0004 后不解密，
 * 直接取第一个 0x00 当字符串，然后 mb1.Oooo0o0(mac,ver) 存本地、
 * ir9.OoooOO0(mac,ver) 上报云端，**上报成功才算注册完成**；
 * 失败就走进 onFailure，界面报「蓝牙连接失败」，并在读 0x0004 后 1.17s
 * 主动断开（reason=0x213）——和我们现场每一轮的收尾完全一致。
 * 版本号抬高是为了避免 App 认为固件过旧、转去走 OTA 流程。 */
static const char     s_fw_ver[] = "9.9.9_9999";     /* 明文 10B，勿加密 */
static const char     s_serial[] = "0815/000001";

/* 绑定过程中 App 写进来的 20B Device ID（0x0013 是可写特征，旧工程日志里
 * 绑定确认之后就有一条 WRITE 0x0013 len=20；这里只读会让框架层拿不到句柄） */
static uint8_t s_devid[20];
static uint8_t s_devid_len;

/* 各级通道的收发计数，出问题时先看这里：写得进来但没有回包，通常是状态机
 * 停在别的分支；一条写都没有，则是 App 压根没认这个服务。 */
static uint32_t s_wr10, s_wr19, s_wr1a, s_wr1b, s_wr01;

/* 0x0010 是同一条通道上的多路复用：认证命令（A4/A2/24，长度 1 或 4）
 * 和认证之后的数据帧（spec，ag2 封装，首字节 0x00）都从这里进来。
 * 按已知认证命令白名单分流，剩下的都交给 spec 层。 */
static int notify_chr(uint16_t handle, const uint8_t *d, size_t len);

/* ================= legacy RC4 认证（夜灯2 / 老 MiBeacon 型号） =================
 * 依据旧工程 mi_beacon/main/main.c（v54~v77b，PID 0x03DD 实机跑通）：
 *   1) 写 90 CA 85 DE             → 认证初始化，**不应答**
 *   2) 写 cipher(mixA, T) 12B     → 采纳新 token，回 12B ACK（0x0001）
 *   3) 写 00 BC 43 CD             → 回 RC4(token, rand4) 4B（0x0001）
 *   4) 写 cipher(kl, 0x93BFAC09)  → kl=(rand4^tok[0:4])||tok[4:12]，
 *                                    校验过后回 RC4(kl, 0x369A58C9)
 *   5) 写 cipher(token, 0xFA54AB92) → 绑定确认
 * 走标准 ECDH 的型号永远不会发这四个魔数，所以这段可以无条件挂上。 */
#define AUTH_INIT_MAGIC    0xDE85CA90u
#define AUTH_GETRAND_MAGIC 0xCD43BC00u
#define LOGIN_MAGIC        0x369A58C9u
#define LOGIN_MAGIC_V2     0x93BFAC09u
#define BOND_MAGIC         0xFA54AB92u

static uint8_t  s_mac_air[6];
static uint8_t  s_ltok[12];         /* legacy 流程采纳的 token */
static uint8_t  s_rand4[4];         /* 最近一次 GETRAND 的挑战值 */
static uint8_t  s_legacy_tok_ok;
static uint8_t  s_legacy_bonded;    /* 已完成 legacy BOND（App 下发过 token） */
static uint8_t  s_tok_this_conn;    /* 本次连接 App 写过 12B token（= 注册/绑定那一轮） */

/* ★2026-11-10 18:50 定论：mixA/mixB 用的是「空中序」MAC。
 * 证据是旧日志里唯一一轮**认证全通过**的记录（uptime 65825~74415）：
 *   wr 0001 len=12 -> 采纳 0E3BB84A... -> 12B ACK -> App 90ms 后写 BOND（校验通过）
 *   -> 写 0x0013 devid -> GETRAND -> 4B LOGIN 校验通过 -> 回 login-ok
 * 那一版固件用的就是 s_mac_air。人读序是参考工程那台设备（PID/MAC 都不同）
 * 实验出来的，搬到这台上是错的。所以默认空中序，未锁定时下一次重试换人读序兜底。 */
static uint8_t  s_mac_human[6];
static uint8_t  s_mac_human_ok;
static uint8_t  s_ord = 1;          /* 下一次用哪一序：0=人读序 1=空中序 */
static uint8_t  s_ord_locked;       /* BOND 已确认 → 锁定 s_ord */
static uint8_t  s_ord_used = 1;     /* 本次采纳实际用的序 */

void gatt_miot_set_mac(const uint8_t mac_air[6])
{
    memcpy(s_mac_air, mac_air, 6);
}

void gatt_miot_set_mac_human(const uint8_t mac_human[6])
{
    memcpy(s_mac_human, mac_human, 6);
    s_mac_human_ok = 1;
}

static const uint8_t *legacy_mix_mac(int air)
{
    if (air) return s_mac_air;
    return s_mac_human_ok ? s_mac_human : s_mac_air;
}

/* ---------------- 事件环形缓冲（事后取证用） ----------------
 * 现场测试时串口监听经常在两次对话之间就没了，人一操作完日志就断档。
 * 这里把关键事件按时间戳记在 RAM 里（掉电才丢，USB 一直插着就不会丢），
 * 事后串口敲 `log` 一次性全部倒出来，不再依赖监听进程是否活着。 */
#define EVT_MAX 128
typedef struct {
    uint32_t t;      /* 开机毫秒 */
    uint16_t code;
    uint8_t  a, b;
    uint32_t d;
} evt_rec_t;

static evt_rec_t s_evt[EVT_MAX];
static uint16_t  s_evt_n;

static void evt(uint16_t code, uint8_t a, uint8_t b, uint32_t d)
{
    evt_rec_t *e = &s_evt[s_evt_n % EVT_MAX];
    e->t = (uint32_t)(esp_timer_get_time() / 1000);
    e->code = code;
    e->a = a;
    e->b = b;
    e->d = d;
    s_evt_n++;
}

void gatt_miot_dump_evt(void)
{
    uint16_t n = s_evt_n < EVT_MAX ? s_evt_n : EVT_MAX;
    ESP_LOGW(TAG, "=== event log %u/%u ===", (unsigned)n, (unsigned)s_evt_n);
    for (uint16_t i = 0; i < n; i++) {
        const evt_rec_t *e = &s_evt[(s_evt_n - n + i) % EVT_MAX];
        ESP_LOGW(TAG, "evt %lu c=%u a=%u b=%u d=%08lX",
                 (unsigned long)e->t, (unsigned)e->code,
                 (unsigned)e->a, (unsigned)e->b, (unsigned long)e->d);
    }
}

/** 选本次采纳用的 MAC 序；未锁定时每次绑定尝试换一种（App 每轮都会重发 token） */
static int legacy_next_order(void)
{
    int air;
    if (s_ord_locked) {
        air = s_ord & 1;
    } else {
        air = s_ord & 1;
        s_ord ^= 1;
    }
    s_ord_used = (uint8_t)air;
    return air;
}

/* RC4（KSA + PRGA，每次重建） */
static void rc4(const uint8_t *key, size_t klen, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t S[256];
    int i, j;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    j = 0;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % klen]) & 0xFF;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    i = j = 0;
    for (size_t t = 0; t < len; t++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
        out[t] = in[t] ^ S[(uint8_t)(S[i] + S[j])];
    }
}

static void mix_key1(const uint8_t *m, uint16_t pid, uint8_t k[8])
{
    k[0] = m[0]; k[1] = m[2]; k[2] = m[5];
    k[3] = (uint8_t)(pid & 0xFF); k[4] = (uint8_t)(pid & 0xFF);
    k[5] = m[4]; k[6] = m[5]; k[7] = m[1];
}
static void mix_key2(const uint8_t *m, uint16_t pid, uint8_t k[8])
{
    k[0] = m[0]; k[1] = m[2]; k[2] = m[5];
    k[3] = (uint8_t)(pid >> 8);
    k[4] = m[4]; k[5] = m[0]; k[6] = m[5]; k[7] = (uint8_t)(pid & 0xFF);
}

/** 当前该用哪个 token：legacy 采纳过就用它，否则用 ECDH 绑定的身份 */
static const uint8_t *cur_token(void)
{
    if (s_legacy_tok_ok) return s_ltok;
    if (s_auth && s_auth->bound) return miot_auth_identity(s_auth)->token;
    return NULL;
}

/* ===== GETRAND 应答：多候选探测 =====
 *
 * 旧工程（v54，PID 0x03DD 实机）记过两种坑：单帧被丢、以及 token 来源不唯一。
 * 本次实测更极端：4 帧同一内容全部 rc=0（确实发出去了），App 仍在一秒后主动断开，
 * 且再也没写过 0x0010 —— 说明「发出去」不等于「被接受」。
 *
 * 于是把两个不确定的轴合并到一次连接里轮着试，每帧只带一个假设：
 *   轴1 通道：0x0001 / 0x001A / 0x001B / 0x0010（GETRAND 就是从 0x0010 来的）
 *   轴2 密钥：ECDH token（旧工程结论）/ FF*12（插件未拿到 token 时的默认值）/ bind_key
 * 谁被接受，App 就会立刻回写 LOGIN，日志里能直接看出是第几帧。 */
#define GR_MAX 18
#define GR_STEP_MS 200
static esp_timer_handle_t s_gr_timer;
static uint8_t  s_gr_cand[4][12];
static const char *s_gr_cname[4];
static uint8_t  s_gr_ncand;
static uint8_t  s_grf_data[GR_MAX][4];
static uint16_t s_grf_h[GR_MAX];
static const char *s_grf_tag[GR_MAX];
static uint8_t  s_grf_n, s_grf_i;

/* 回包帧数（运行时可变，串口 `gr N` 设置）：
 *   0  = 完全静默，用来判定「App 到底在不在等这个回包」
 *   1  = 单帧（旧工程唯一被证明有效过的形态）
 *   18 = 6 种假设循环 3.6 秒 */
static int s_gr_frames = GR_MAX;

/* 回包延时（默认 GR_STEP_MS）。旧工程结论：必须等 ATT 写响应先发出去，
 * 但 App 在登录路径上等应答的窗口非常短，所以这里做成可调。 */
static int s_gr_delay_ms = GR_STEP_MS;

/* 登录路径（绑定之后的每次重连）策略轮换：
 * 每次新建连接 +1，用 seq % 6 选一种「通道 + 延时」组合，串口日志里打 [strat=N]。
 * 目的：App 在这条路径上 1.0s 就主动断开，只有一轮实验覆盖多个假设才能看出
 * 它到底在哪个通道、哪个时间窗上收包。 */
static uint8_t s_conn_seq;
/* ★默认 10 = 「GETRAND 回明文 rand」。
 * 这是 2026-11-11 反解出来的协议语义：设备把 challenge 明文交给 App，
 * App 回 LOGIN = f(token, rand)，设备验算。四组真值 4/4 验证通过。
 * 之前默认 -1（探测轮换）会把 6 种假设轮着试，命中率只有 1/6，
 * 表现为「有时能过、多数超时」。烧录后即生效，不必再手工 strat 10。 */
static int     s_strat = 10;

static int strat_pick(void)
{
    return (s_strat >= 0) ? s_strat : (int)(s_conn_seq % 6);
}

void gatt_miot_set_gr_reply(int n)
{
    if (n < 0) n = 0;
    if (n > GR_MAX) n = GR_MAX;
    s_gr_frames = n;
    if (n == 0 && s_gr_timer) esp_timer_stop(s_gr_timer);
}

int gatt_miot_get_gr_reply(void)
{
    return s_gr_frames;
}

void gatt_miot_set_strat(int n)
{
    s_strat = (n < 0 || n > 10) ? -1 : n;
}

/* ★GETRAND 应答的首帧延时（串口 `grd <ms>`）。
 *
 * 这是「不装 hook 也要能用」的核心参数：实测 App 写完 GETRAND 后，
 * 在**它自己的判定窗口**内就会调 disconnect（失败轮 37ms），
 * 旧的 150ms 回包只有靠 hook 拦住断连才送得到 App。
 * 压到 40ms 后，回包落在断连之前，原版 App 也能接住。
 * 注意 gr_send_next 里还有「等 CCCD 订阅最多 1.2s」的保护，不会被这个值影响。 */
static int s_gr_step_ms = 40;

void gatt_miot_set_gr_delay(int ms)
{
    s_gr_step_ms = (ms < 0) ? 0 : (ms > 3000 ? 3000 : ms);
}

int gatt_miot_get_gr_delay(void)
{
    return s_gr_step_ms;
}

/* 「设备先说话」探针开关（串口 `spec 1`）。见 gatt_miot_on_subscribe。 */
static int s_spec_probe;

void gatt_miot_set_spec_probe(int on) { s_spec_probe = on; }
int  gatt_miot_get_spec_probe(void)   { return s_spec_probe; }

/* 每条特征值的订阅状态（1 = App 写过 CCCD 开启 notify/indicate）。
 * ★为什么必须有这张表：夜灯2 详情页只订 0x001B / 0x001A / 0x0001，
 * **0x0019 和 0x0010 从来不订**。而标准认证的应答面默认在 0x0019 上，
 * 不看订阅状态直接发就等于往没人收的地址写信 —— 门锁（v107）踩过同一个坑。
 * 这里记录订阅，认证应答改投 App 真正订阅的那条通道。 */
static uint8_t s_sub[64];

/* ★ag2/channel 控制帧的「来路面」（2026-11-10 22:1x 定论）。
 * mi_beacon 的同型实测写得很明确：控制帧（mng/mng_ack）的应答必须回到
 * **App 把那一帧写进来的那条特征**上（门锁=0x0016、体脂秤=0x0019）。
 * 夜灯2 的 App 把 mng_ack 写在 0x001B 上，就在 0x001B 等我们的测试包；
 * 我们之前按 auth_chan() 固定投到 0x001A，等于把校验包投进了没人读的信箱，
 * App 干等 1s 后 cancelOpen（页面「连接超时」）。 */
static uint16_t s_ag_h;
/** legacy 请求的「来路面」（0x0001 或 0x0010）。
 *  绑定时 App 把 GETRAND 写在 0x0010，却在 0x0001 上等回包 —— 两种都有；
 *  实测被接住的那次是**单帧**回到 App 真正订阅的那条面。 */
static uint16_t s_leg_h;

/** ★2026-11-12 16:4x：**当前这一帧是从哪条特征写进来的**。
 *  s_ag_h 是「本次连接第一次写入的那条面」，粘住不变；登录阶段 App 先写
 *  0x0019，于是 s_ag_h 被钉成 0x0019。登录成功后 App 改在 0x001A 发 ag2
 *  控制帧（START `00 00 00 00 01 00`），我们来 ACK 却仍投 0x0019 ——
 *  App 在 0x001A 上等不到 ACK，每 6 秒重发一次 START（实测正是如此），
 *  属性读写永远推进不下去。应答必须**逐帧**回到该帧的来源面。 */
static uint16_t s_rx_chan;

/** 认证应答面：优先「控制帧来路面」，其次 0x0019 > 0x001A > 0x001B。
 *  夜灯2 命中的是 0x001A；门锁是 0x0016（那套在 mi_beacon 里另有分支）。 */
static uint16_t auth_chan(void)
{
    if (s_ag_h && s_ag_h < 64 && s_sub[s_ag_h]) return s_ag_h;
    if (s_h19 && s_h19 < 64 && s_sub[s_h19]) return s_h19;
    if (s_h1a && s_h1a < 64 && s_sub[s_h1a]) return s_h1a;
    if (s_h1b && s_h1b < 64 && s_sub[s_h1b]) return s_h1b;
    return s_h19;
}

/* START 声明了 count=1，就必须在 120ms 内把那一帧数据发出去，否则 App
 * 一直等（实测：只发 START 时 App 回完 `00 00 01 01` 就静默，1s 后断开）。
 * 数据帧 = [seq16 LE][载荷]，载荷按 docs/protocol.md §4 的 spec v2 明文帧：
 *   0D 20        len16 = 0x200D（协议版本 2 | 总长 13）
 *   01 00        tid
 *   04           op = properties_changed
 *   01           count
 *   03  01 00    siid=3 piid=1（电量）
 *   01 10        type16 = (uint8=1 <<12) | len=1
 *   01  50       长度 1，值 80%
 * 明文直接塞进 ag2 数据帧 —— 这一版只为验证「App 会不会收」，
 * 若它按 k61 解（[seq16][ciphertext][tag4]）会判格式错，那就说明必须
 * 先完成标准认证拿会话密钥，是另一个方向。 */
static esp_timer_handle_t s_spec_timer;
static uint16_t s_spec_h;

static void spec_probe_data(void *arg)
{
    (void)arg;
    uint16_t h = s_spec_h;
    static const uint8_t k_frame[] = {
        0x01, 0x00,                                     /* seq = 1 */
        0x0D, 0x20, 0x01, 0x00, 0x04, 0x01,
        0x03, 0x01, 0x00, 0x01, 0x10, 0x01, 0x50
    };
    int rc = notify_chr(h, k_frame, sizeof(k_frame));
    ESP_LOGW(TAG, "spec probe: DATA %uB -> h=%u rc=%d",
             (unsigned)sizeof(k_frame), h, rc);
}

static void spec_probe_start(uint16_t h)
{
    static const uint8_t k_start[6] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    int rc = notify_chr(h, k_start, sizeof(k_start));
    ESP_LOGW(TAG, "spec probe: START -> h=%u rc=%d", h, rc);
    s_spec_h = h;

    if (!s_spec_timer) {
        const esp_timer_create_args_t a = { .callback = spec_probe_data, .name = "spec" };
        if (esp_timer_create(&a, &s_spec_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_spec_timer);
    esp_timer_start_once(s_spec_timer, 120 * 1000);
}

/* ★建链探针（`spec 2`，默认模式）：设备主动发 ag2 的 mng 建链帧。
 * 依据 mi_beacon v107/v108（同型真机实测）：
 *   App 收到 mng 才回 `00 00 05 00 06 12`（mng_ack），再回一条 16B 测试包，
 *   通道才算 READY；之后它才会写 0xA2/0x24 到 0x0010 启动 ECDH 认证。
 *   而 App 自己从不主动写建链请求 —— 它只在自己发起的会话里写单字节 0xA4，
 *   我们这台从头到尾没收到过 0xA4，所以这条链一次都没被触发。
 * 帧 = channel 包 [00 00][cmd=04][code=00][num=06][dmtu=12]。
 * dmtu 必须是 18：填 0 会让 App 侧 min(local,0) 算出 0，测试包长度变 -2，
 * 整条建链当场死掉（门锁那台的原话是「设备初始化卡住的起点」）。 */
static void spec_probe_link(uint16_t h)
{
    static const uint8_t k_mng[6] = {0x00, 0x00, 0x04, 0x00, 0x06, 0x12};
    int rc = notify_chr(h, k_mng, sizeof(k_mng));
    ESP_LOGW(TAG, "link probe: mng(code=0,num=6,dmtu=18) -> h=%u rc=%d", h, rc);
}

static esp_timer_handle_t s_link_timer;

static void spec_probe_link_retry(void *arg)
{
    (void)arg;
    if (s_auth && s_auth->chan_ready) return;         /* 已经 READY，不重复打扰 */
    uint16_t h = auth_chan();
    if (h && h < 64 && s_sub[h]) spec_probe_link(h);
}

static void spec_probe_link_once(uint16_t h)
{
    spec_probe_link(h);
    if (!s_link_timer) {
        const esp_timer_create_args_t a = { .callback = spec_probe_link_retry,
                                           .name = "link" };
        if (esp_timer_create(&a, &s_link_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_link_timer);
    esp_timer_start_once(s_link_timer, 400 * 1000);   /* 400ms 没反应就补一次 */
}


int gatt_miot_get_strat(void)
{
    return s_strat;
}

static void gr_put(uint8_t i, uint16_t h, const uint8_t *p4, const char *tag)
{
    if (i >= GR_MAX) return;
    memcpy(s_grf_data[i], p4, 4);
    s_grf_h[i] = h;
    s_grf_tag[i] = tag;
    if (i + 1 > s_grf_n) s_grf_n = (uint8_t)(i + 1);
}

static void gr_send_next(void *arg);

static void gr_ensure_timer(void)
{
    if (s_gr_timer) return;
    const esp_timer_create_args_t a = { .callback = gr_send_next, .name = "gr" };
    if (esp_timer_create(&a, &s_gr_timer) != ESP_OK) s_gr_timer = NULL;
}

static uint32_t s_gr_t0;      /* 等待通道订阅的起点（ms） */

static void gr_send_next(void *arg)
{
    (void)arg;
    if (!s_gr_timer || s_grf_i >= s_grf_n) return;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) { s_grf_n = 0; return; }
    uint16_t h = s_grf_h[s_grf_i] ? s_grf_h[s_grf_i] : s_h10;
    /* ★夜灯2 的时序是「先写 GETRAND，随后才订阅 0x0010」，150ms 就投等于
     * 投进还没人订阅的信箱（CCCD 未开 → notify 直接丢）。这里最多等 1.2s
     * 让 App 把订阅打开；超时仍按原面发，避免彻底卡死。 */
    if (h < 64 && !s_sub[h]) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_gr_t0 == 0) s_gr_t0 = now;
        if (now - s_gr_t0 < 1200) {
            esp_timer_start_once(s_gr_timer, 50 * 1000);
            return;
        }
        ESP_LOGW(TAG, "等订阅超时(1.2s) h=%u，仍投递", h);
    }
    s_gr_t0 = 0;
    int rc = notify_chr(h, s_grf_data[s_grf_i], 4);
    evt(4, (uint8_t)s_grf_i, (uint8_t)h, (uint32_t)rc);
    ESP_LOGW(TAG, "auth 回包 #%u h=%u %s rc=%d",
             (unsigned)(s_grf_i + 1), (unsigned)h, s_grf_tag[s_grf_i], rc);
    s_grf_i++;
    if (s_grf_i < s_grf_n) {
        esp_timer_start_once(s_gr_timer, (uint64_t)s_gr_delay_ms * 1000);
    }
}

static void gr_start(void)
{
    gr_ensure_timer();
    if (!s_gr_timer) return;
    if (s_gr_frames <= 0) {
        ESP_LOGW(TAG, "auth 回包：静默模式（不回应答，用于判定 App 是否在等它）");
        return;
    }
    if (s_grf_n > (uint8_t)s_gr_frames) s_grf_n = (uint8_t)s_gr_frames;
    s_grf_i = 0;
    s_gr_t0 = 0;
    esp_timer_stop(s_gr_timer);
    esp_timer_start_once(s_gr_timer, (uint64_t)s_gr_delay_ms * 1000);
}

/** 候选密钥：0=App 下发的 legacy token（若有）、1=ECDH token、2=FF*12、3=bind_key */
static void gr_build_cands(void)
{
    const miot_auth_identity_t *id =
        (s_auth && s_auth->bound) ? miot_auth_identity(s_auth) : NULL;
    s_gr_ncand = 0;
    if (s_legacy_tok_ok) {
        memcpy(s_gr_cand[s_gr_ncand], s_ltok, 12);
        s_gr_cname[s_gr_ncand++] = "legacy-token";
    }
    if (id) {
        memcpy(s_gr_cand[s_gr_ncand], id->token, 12);
        s_gr_cname[s_gr_ncand++] = "ecdh-token";
    }
    memset(s_gr_cand[s_gr_ncand], 0xFF, 12);
    s_gr_cname[s_gr_ncand++] = "ff12";
    if (id) {
        memcpy(s_gr_cand[s_gr_ncand], id->bind_key, 12);
        s_gr_cname[s_gr_ncand++] = "bindkey";
    }
}

/** GETRAND：6 个假设循环 18 帧、共 3.6s。
 *
 * 目的不只是"重发"，而是判定 App 到底在等什么：
 *   若断开时刻随帧一起后移 → 它在等我们的包（只是不认账）；
 *   若断开仍固定在 GETRAND 后约 1s → 它根本没等这个回包，得另找原因。 */
static void reply_getrand(const uint8_t *rand4)
{
    /* 旧版绑定流程（App 下发 12B token）里密钥是确定的，不需要轮播猜测：
     * 旧工程唯一验证过的形态就是「同一份 RC4(token,rand) 连发多帧」
     * （150/450/750ms，单发 180ms 曾 4/4 被丢）。这里照做，发 4 帧。 */
    if (s_legacy_tok_ok) {
        uint8_t p[4];
        /* ★2026-11-10 12:35 定论：注册轮（本次连接写过 12B token）用
         *   「RC4(token,rand) 单内容 ×4 帧、间隔 200ms」是**唯一被实机接住**的形态 ——
         *   10:?? 那轮日志：GETRAND 73775 → 回包 73975(200ms) → 74015 App 就写
         *   LOGIN 密文并校验通过。之后的重连轮却改成 25ms×2 帧，全部失败。
         *   25ms 那两帧会与 ATT 写响应抢跑（App 回调 60ms 才就绪），等于白发。
         *   所以登录轮**直接复用注册轮形态**，不再换时延、不再多假设并发
         *   （错的帧会先把 App 顶进失败分支，多假设并发只会互相拆台）。 */
        /* ★20:35 登录轮（页面重连）用的密钥假设要能自动轮换：
         * 绑定轮（s_tok_this_conn=1）固定 0（App 刚下发的 token，必然对）；
         * 登录轮以前写死 0，`strat -1` 根本不生效，导致永远只试同一种密钥。
         * 现在交给 strat_pick()：strat=-1 时按连接序号 %6 轮换 6 种假设，
         * 页面每 2s 重连一次，15 秒内能把所有假设试完，日志里看哪一轮
         * 出现「LOGIN 校验通过」就知道 App 页面用的是哪把密钥。 */
        /* ★2026-11-10 20:55 定论（本轮硬证据，见 _nightl.log 159xx 与 7786-7820）：
         * 绑定轮那次成功用的是 **strat=0 + 同一份 resp 连发 4 帧 @200ms，只发 0x0001**：
         *     69285 GETRAND strat=0 resp=C2317D10（同帧 x4 @200ms）
         *     69495 回包 #1 h=16  →  69545 App 就写 LOGIN 并通过
         * 之后为「多假设并发」改成「单帧 180ms、同发 0x0001+0x0010」，结果登录轮
         * 每一轮的 strat 都被 connection_seq%6 轮换着挑，挑到 1..5 就必然是错密钥。
         * 页面每秒重连一次，等于把 6 把钥匙里 5 把错的轮流喂给 App。
         * 现在回到唯一被实机接住的形态：密钥固定 token 的 RC4、4 帧、只走 0x0001。 */
        const int st = (s_strat > 0) ? strat_pick() : 0;   /* 默认 0；串口 strat N 才切 */
        const miot_auth_identity_t *bid =
            (s_auth && s_auth->bound) ? miot_auth_identity(s_auth) : NULL;
        uint8_t key[16];
        size_t klen = 12;
        const char *tag = "legacy-token";

        switch (st) {
        case 1:
            if (bid) { memcpy(key, bid->bind_key, 12); klen = 12; tag = "bkey12f"; }
            else     { memcpy(key, s_ltok, 12);        klen = 12; tag = "legacy-token"; }
            break;
        case 2:
            if (bid) { memcpy(key, bid->bind_key + 4, 12); klen = 12; tag = "bkey12b"; }
            else     { memcpy(key, s_ltok, 12);            klen = 12; tag = "legacy-token"; }
            break;
        case 3:
            if (bid) { memcpy(key, bid->bind_key, 16); klen = 16; tag = "bkey16"; }
            else     { memcpy(key, s_ltok, 12);        klen = 12; tag = "legacy-token"; }
            break;
        default:
            memcpy(key, s_ltok, 12); klen = 12; tag = "legacy-token";
            break;
        }

        rc4(key, klen, rand4, p, 4);
        if (st == 4) {                     /* 字节序假设：输出反转 */
            uint8_t t0 = p[0], t1 = p[1];
            p[0] = p[3]; p[1] = p[2]; p[2] = t1; p[3] = t0;
            tag = "legacy-token-rev";
        } else if (st == 5) {              /* 明文假设：直接回 token 前 4B */
            memcpy(p, s_ltok, 4);
            tag = "legacy-token4raw";
        } else if (st == 6 || st == 7) {   /* 挑战应答假设：设备直接回明文随机数
                                            * （p21 标准挑战-应答：设备给 nonce，
                                            * App 回 RC4(K,nonce)，设备验算）。
                                            * st=7 额外把同一份同时投到 0x0010。 */
            memcpy(p, rand4, 4);
            tag = (st == 7) ? "raw-rand+both" : "raw-rand";
        }

        /* ★2026-11-10 19:45：改成「单帧 + 180ms」。
         * mi_beacon v121 的终局结论写得很明白：单帧只发一次、写响应之后
         * 180ms 才 notify，是唯一被实机接住过的形态；多帧会让后续帧落在
         * App 已经进入下一步（写 LOGIN）的窗口里，被当脏帧丢弃。 */
        /* 回到注册轮被接住的那个形态：4 帧同内容、间隔 200ms、只发 0x0001 */
        s_gr_delay_ms = 150;
        s_grf_n = 0;
        if (st == 7) {
            /* 双通道各一帧：0x0001 → 0x0010 */
            gr_put(0, s_h01, p, tag);
            gr_put(1, s_h10, p, tag);
        } else if (st == 10) {
            /* ★根因修复（2026-11-10 22:5x）：GETRAND 的语义是「把挑战随机数
             * 交给 App」，不是「把密文交给 App」。本端 esp_fill_random 生成
             * s_rand4 后一直只回 RC4(token,rand)，App 根本无从得知 rand ——
             * 而它必须拿 rand 才能算出 LOGIN = RC4(kl, magic)。拿不到就永远
             * 不写 LOGIN，1s 后断开（0x213）。这与「绑定轮却写得出 LOGIN」
             * 并不矛盾：那一轮 App 手里已经有别的来源。
             * 明文 4B 下发后，本端仍用 s_rand4 校验，两端自洽。 */
            s_gr_delay_ms = 150;
            gr_put(0, s_h01, rand4, "raw-rand");
            gr_put(1, s_h01, rand4, "raw-rand");
        } else if (st == 8 || st == 9) {
            /* ★多算法探测：一次连接内定位「App 登录用哪把钥匙」。
             * 页面登录轮每次都要重连一轮，一轮只试一把钥匙太慢；而 App 收到
             * 回包后有约 1s 判定窗口，足够按 200ms 间隔塞 4 个候选。它一旦认可
             * 就会立刻写 LOGIN —— 所以日志里「第几帧被接住」=「哪把钥匙」。
             *   st=8: A token-RC4 / B bkey16 / C bkey[4..16] / D 明文 rand
             *   st=9: E HKDF(login) / F token-RC4 反序 / G rand 反序 / H 双重 RC4 */
            uint8_t q[4], e[4];
            const miot_auth_identity_t *bi = bid;
            s_gr_delay_ms = 200;
            if (st == 8) {
                gr_put(0, s_h01, p, "A:token-rc4");
                if (bi) {
                    rc4(bi->bind_key, 16, rand4, q, 4);
                    gr_put(1, s_h01, q, "B:bkey16");
                    rc4(bi->bind_key + 4, 12, rand4, q, 4);
                    gr_put(2, s_h01, q, "C:bkey12b");
                } else {
                    gr_put(1, s_h01, p, "B:token-dup");
                    gr_put(2, s_h01, p, "C:token-dup");
                }
                gr_put(3, s_h01, rand4, "D:raw-rand");
            } else {
                uint8_t salt[32] = {0};
                uint8_t sess[64];
                uint8_t rr[4];
                miot_hkdf_sha256(s_ltok, 12, salt, 32, "mible-login-info", 16, sess, 64);
                rc4(sess, 16, rand4, q, 4);
                gr_put(0, s_h01, q, "E:hkdf-login");
                q[0] = p[3]; q[1] = p[2]; q[2] = p[1]; q[3] = p[0];
                gr_put(1, s_h01, q, "F:token-rev");
                rr[0] = rand4[3]; rr[1] = rand4[2]; rr[2] = rand4[1]; rr[3] = rand4[0];
                rc4(key, klen, rr, q, 4);
                gr_put(2, s_h01, q, "G:rand-rev");
                memcpy(e, p, 4);
                rc4(key, klen, e, q, 4);
                gr_put(3, s_h01, q, "H:double-rc4");
            }
        } else {
            /* ★2026-11-10 23:2x：默认策略改为「单帧 + 回到来路面」。
             * 之前是 4 帧同内容连发到 0x0001，实测绑定轮被接住的那次是
             * **单帧、且落在 App 正在读的那条面**（写在 0010 → 读 0010）。
             * 多帧会把后续帧塞进 App 已进入下一步的窗口，被当脏帧丢弃。 */
            /* 夜灯2 实测：App 把 GETRAND 写在 0x0010，却订阅 0x0001 等回包。
             * 所以「来路面」不能盲信，必须在**已订阅**的面里选：
             *   来路面已订阅 → 用它；否则 0x0001 → 0x0010。 */
            uint16_t h = 0;
            if (s_leg_h && s_leg_h < 64 && s_sub[s_leg_h]) h = s_leg_h;
            else if (s_h01 && s_h01 < 64 && s_sub[s_h01]) h = s_h01;
            else if (s_h10 && s_h10 < 64 && s_sub[s_h10]) h = s_h10;
            else h = s_h01;
            s_gr_delay_ms = 150;
            gr_put(0, h, p, tag);
            ESP_LOGW(TAG, "legacy GETRAND 应答面 h=%u (leg_h=%u h01=%u/%u h10=%u/%u)",
                     h, s_leg_h, s_h01, s_h01 < 64 ? s_sub[s_h01] : 0,
                     s_h10, s_h10 < 64 ? s_sub[s_h10] : 0);
        }
        ESP_LOGW(TAG, "legacy GETRAND(%s) conn#%u strat=%d rand=%02X%02X%02X%02X key=%02X%02X%02X%02X(%s) -> resp=%02X%02X%02X%02X（%u 帧 @%dms）",
                 s_tok_this_conn ? "注册" : "登录", (unsigned)s_conn_seq, st,
                 rand4[0], rand4[1], rand4[2], rand4[3],
                 key[0], key[1], key[2], key[3], tag,
                 p[0], p[1], p[2], p[3],
                 (unsigned)s_grf_n, s_gr_delay_ms);
        /* ★多候探测（st=8/9）依赖 200ms 的间隔，其它策略统一用可调首帧延时。 */
        if (st != 8 && st != 9) s_gr_delay_ms = s_gr_step_ms;
        gr_start();
        return;
    }

    gr_build_cands();
    uint8_t p[4], q[4], t[4];
    uint16_t hs[6];
    const char *tags[6];

    if (s_gr_ncand < 2) return;

    rc4(s_gr_cand[0], 12, rand4, p, 4);
    rc4(s_gr_cand[1], 12, rand4, q, 4);
    if (s_gr_ncand >= 3) rc4(s_gr_cand[2], 12, rand4, t, 4);
    else memcpy(t, p, 4);

    hs[0] = s_h01; tags[0] = s_gr_cname[0];
    hs[1] = s_h01; tags[1] = s_gr_cname[1];
    hs[2] = s_h1a; tags[2] = "ecdh@001A";
    hs[3] = s_h1b; tags[3] = "ecdh@001B";
    hs[4] = s_h10; tags[4] = "ecdh@0010";
    hs[5] = s_h01; tags[5] = (s_gr_ncand >= 3) ? s_gr_cname[2] : "bindkey";

    s_grf_n = 0;
    for (uint8_t i = 0; i < GR_MAX; i++) {
        uint8_t k = (uint8_t)(i % 6);
        const uint8_t *pl = (k == 1) ? q : (k == 5 ? t : p);
        gr_put(i, hs[k], pl, tags[k]);
    }
    gr_start();
}

/** 在指定特征上连发两帧 4B（同一份内容） */
/** legacy 回包该落在哪条面。
 *
 *  ★不能盲信「来路面」：无线开关（0x1949）实测 App 把 4B 写在 0x0010，
 *  但 0x0010 是只写特征、App 根本订阅不了通知；回包投到 h=23 时
 *  `notify rc=0` 却没人收，App 只能干等自己的超时（本轮实测：连上 44.5s，
 *  回包 49.5s 送完，之后**静默 35s** 才退订断开 reason=0x213）。
 *  夜灯2 成功那一轮之所以能过，正是因为它落在了 App 已订阅的 h=16（0x0001）。
 *  规则：已订阅的来路面 → 0x0001 → 0x0010 → 最后才退回来源句柄。 */
static uint16_t legacy_reply_handle(uint16_t src_h)
{
    if (s_leg_h && s_leg_h < 64 && s_sub[s_leg_h]) return s_leg_h;
    if (s_h01   && s_h01   < 64 && s_sub[s_h01])   return s_h01;
    if (s_h10   && s_h10   < 64 && s_sub[s_h10])   return s_h10;
    return src_h;
}

static void reply_4b_on(uint16_t h, const uint8_t *p4, const char *tag)
{
    s_grf_n = 0;
    gr_put(0, h ? h : s_h01, p4, tag);
    gr_put(1, h ? h : s_h01, p4, tag);
    gr_start();
}

/** LOGIN 应答：两帧足够（同一份 RC4(kl, magic)） */
static void reply_login(const uint8_t *p4)
{
    reply_4b_on(s_h01, p4, "login-ok");
}

/** 未识别 4B 的兜底应答（mi_beacon v121 的反编译+实测结论）
 *
 *  BOND 之后 App 会**立刻再发一次 LOGIN**，密文每次不同、且不经过 GETRAND：
 *    手机发 = RC4(kl, 09 AC BF 93)，设备应答 = RC4(kl, C9 58 9A 36)
 *  流密码下密文之差 = 明文之差，所以无需知道 kl：
 *    resp = req ^ (09 AC BF 93 ^ C9 58 9A 36) = req ^ {C0,F4,25,A5}
 *  旧工程实测：**不回这一帧**，App 空等 6.5~7.5s 后放弃，退回 GETRAND 重来，
 *  最终 0x213 断开 —— 与本机日志里 wr 0010 #2(5E B0 FA 11) 后 7.5s 静默完全吻合。 */
static const uint8_t LEGACY_4B_DELTA[4] = {0xC0, 0xF4, 0x25, 0xA5};

/** 连接器模式字 / 标准认证帧：绝对不能走兜底（脏帧会插队打断注册流程） */
static int is_connector_word(const uint8_t *d)
{
    if (d[1] || d[2] || d[3]) return 0;
    switch (d[0]) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x20: case 0x21: case 0x22: case 0xE1:
        return 1;
    default:
        return 0;
    }
}

/** 采纳 App 下发的 12B token（cipher(mixA, token)），并立刻回 12B ACK。
 *
 *  ★这一步是夜灯2 的「卡死点」：App 写 12B token 之后**不再发任何东西**，
 *  而是订阅 0x0001 的 notify 等 ACK；收不到就干等 15s（p21 timer#1）后
 *  以 reason=0x213 主动断开，界面报「连接超时」。
 *  旧工程 mi_beacon v121（PID 0x03DD 实机跑通）日志里明确有：
 *    GATT WRITE 0x0001 len=12 -> NOTIFY handle=16 len=12 -> GATT WRITE 0x0001 len=4(BOND)
 *
 *  ACK 推导（cy0 Step4 校验式）：App 验 RC4(mixA, RC4(mixB, ack)) == token。
 *  RC4 是流密码 RC4(K,x) = x ^ KS(K)，故 ack = token ^ KS_A ^ KS_B。
 *
 *  @return ACK 是否已发出（1=发出，0=没有订阅者，调用方可以重试） */
static int legacy_adopt_token(const uint8_t *d)
{
    uint8_t k[8], tok[12], z[12] = {0}, ks[12], ack[12];

    int air = legacy_next_order();
    evt(6, (uint8_t)air, 0, (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]);
    const uint8_t *m = legacy_mix_mac(air);

    mix_key1(m, s_pid, k);
    rc4(k, 8, d, tok, 12);
    memcpy(s_ltok, tok, 12);
    s_legacy_tok_ok = 1;
    ESP_LOGW(TAG, "legacy TOKEN 采纳(%s): %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             air ? "air" : "human",
             tok[0], tok[1], tok[2], tok[3], tok[4], tok[5],
             tok[6], tok[7], tok[8], tok[9], tok[10], tok[11]);

    mix_key1(m, s_pid, k);
    rc4(k, 8, z, ks, 12);
    for (int i = 0; i < 12; i++) ack[i] = tok[i] ^ ks[i];
    mix_key2(m, s_pid, k);
    rc4(k, 8, z, ks, 12);
    for (int i = 0; i < 12; i++) ack[i] ^= ks[i];

    uint16_t h = s_h01 ? s_h01 : s_h10;
    int rc = notify_chr(h, ack, 12);
    evt(7, (uint8_t)air, (uint8_t)h, (uint32_t)rc);
    ESP_LOGW(TAG, "legacy TOKEN ACK(%s) -> h=%u %02X%02X%02X%02X... rc=%d",
             air ? "air" : "human", h, ack[0], ack[1], ack[2], ack[3], rc);

    /* ACK 先发（App 在等它），再落盘：NVS commit 有几十 ms，别挡在 ACK 前面 */
    miot_legacy_token_hook(tok);
    return rc == 0;
}

/** 步骤 4/5：4B 登录确认 / 绑定确认。
 *  0x0010 和 0x0001 两条通道都会收到这两种帧（旧工程日志里 LOGIN 确认就写在
 *  0x0001 上），所以抽出来共用。
 *  kl 有两种推导（xor rand / 不 xor），密钥有多个候选，全都试一遍：
 *  谁解出 LOGIN_MAGIC，谁就是 App 手里那个 token。
 *  ★时序差异（mi_beacon v121 实机日志）：
 *    BOND  → App 不等任何 notify，紧接着 read 0x0004，所以只记状态不回包；
 *    LOGIN → 必须回 RC4(kl, magic)，不回 App 就干等到 15s 超时（0x213）。 */
static int legacy_confirm_4b(const uint8_t *d, uint16_t src_h)
{
    gr_build_cands();
    uint32_t magic = LOGIN_MAGIC;
    uint8_t m[4], out[4];
    memcpy(m, &magic, 4);
    int bonded = 0;
    uint8_t dec0[4] = {0}, kl0[12];

    for (uint8_t ci = 0; ci < s_gr_ncand; ci++) {
        for (uint8_t variant = 0; variant < 2; variant++) {
            uint8_t kl[12], dec[4];
            memcpy(kl, s_gr_cand[ci], 12);
            if (variant == 0) {
                kl[0] ^= s_rand4[0]; kl[1] ^= s_rand4[1];
                kl[2] ^= s_rand4[2]; kl[3] ^= s_rand4[3];
            }
            rc4(kl, 12, d, dec, 4);
            uint32_t dv = 0;
            memcpy(&dv, dec, 4);
            if (ci == 0 && variant == 0) { memcpy(dec0, dec, 4); memcpy(kl0, kl, 12); }

            if (dv == LOGIN_MAGIC_V2 || dv == LOGIN_MAGIC) {
                rc4(kl, 12, m, out, 4);
                ESP_LOGW(TAG, "legacy LOGIN 校验通过：token=%s kl=%s -> 回 RC4(kl, magic)",
                         s_gr_cname[ci], variant == 0 ? "xorrand" : "raw");
                evt(9, s_ord_used, ci, 0);
                reply_login(out);
                return 1;
            }
            if (dv == BOND_MAGIC) {
                ESP_LOGW(TAG, "legacy BOND 确认：token=%s kl=%s（App 不等应答）",
                         s_gr_cname[ci], variant == 0 ? "xorrand" : "raw");
                /* 这一份就是 App 指定的 token，用它当身份 */
                memcpy(s_ltok, s_gr_cand[ci], 12);
                s_legacy_tok_ok = 1;
                s_legacy_bonded = 1;
                evt(8, s_ord_used, ci, 0);
                s_ord_locked = 1;              /* 命中的 MAC 序锁死，后续重连复用 */
                s_ord = s_ord_used;
                ESP_LOGW(TAG, "legacy MAC 序锁定 = %s", s_ord_used ? "air" : "human");
                miot_legacy_bound_hook(s_ltok);
                bonded = 1;
            }
        }
    }
    if (bonded) return 1;

    /* 连接器模式字（10/11/12/13/14/15/20/21/22/e1 + 三个 0）属于标准认证的
     * 控制字，不是 legacy 密文：一律静默，交给 spec 状态机处理。 */
    if (is_connector_word(d)) {
        ESP_LOGW(TAG, "legacy 4B 连接器模式字 %02X 00 00 00 -> 静默", d[0]);
        return 0;
    }

    /* 兜底：BOND 之后的那次 LOGIN（密文每次不同、不经 GETRAND）。
     * 流密码下 resp = req ^ {C0,F4,25,A5}，不必知道 kl。
     * ★回包必须落在 App 订阅了的面，否则它收不到、只能等超时（见 legacy_reply_handle）。 */
    uint8_t fb[4];
    for (int i = 0; i < 4; i++) fb[i] = d[i] ^ LEGACY_4B_DELTA[i];
    uint16_t rh = legacy_reply_handle(src_h);
    ESP_LOGW(TAG, "legacy 4B 兜底应答 %02X %02X %02X %02X -> %02X %02X %02X %02X (src=%u 回=%u)",
             d[0], d[1], d[2], d[3], fb[0], fb[1], fb[2], fb[3], src_h, rh);
    reply_4b_on(rh, fb, "4b-fallback");
    (void)dec0;
    (void)kl0;
    return 1;
}

/** 处理 legacy 认证帧；返回 1 表示已消费，不再交给 ECDH/spec 状态机 */
static int legacy_on_0010(const uint8_t *d, uint16_t len)
{
    uint32_t v = 0;
    if (len == 4) memcpy(&v, d, 4);

    /* 步骤 1：认证初始化，按反编译结论**不能应答** */
    if (len == 4 && v == AUTH_INIT_MAGIC) {
        ESP_LOGW(TAG, "legacy AUTH_INIT -> 静默（不应答）");
        return 1;
    }

    /* 步骤 3：GETRAND —— 回 RC4(K, rand4)，K 不确定，六个假设轮着试 */
    if (len == 4 && v == AUTH_GETRAND_MAGIC) {
        esp_fill_random(s_rand4, 4);
        evt(3, s_tok_this_conn, (uint8_t)s_ord, (uint32_t)s_rand4[0] << 24 | s_rand4[1] << 16 | s_rand4[2] << 8 | s_rand4[3]);
        ESP_LOGW(TAG, "legacy GETRAND rand=%02X%02X%02X%02X -> 多候选探测",
                 s_rand4[0], s_rand4[1], s_rand4[2], s_rand4[3]);
        reply_getrand(s_rand4);
        return 1;
    }

    /* 步骤 2：12B token 下发（cipher(mixA, token)） */
    if (len == 12) {
        legacy_adopt_token(d);
        return 1;
    }

    /* 步骤 4/5：登录确认 / 绑定确认 */
    if (len == 4) {
        return legacy_confirm_4b(d, s_h10);
    }
    return 0;
}

/* 0x0010 是多路复用：认证命令（A4/A2/24，长度 1 或 4）和认证后的 spec 帧
 * 都从这条通道进来，按白名单分流，剩下的交给 spec 层。 */
static int is_auth_cmd(const uint8_t *d, uint16_t len)
{
    if (len == 1 && d[0] == 0xA4) return 1;                    /* 建链 */
    if (len != 4) return 0;
    if (d[1] || d[2] || d[3]) return 0;
    /* 0x13 = 注册最后一步的 bind result（App 把它发在 0x0010 上，
     * 只认 0x0019 会让这一步被静默丢弃，bound 永远不置位） */
    return d[0] == 0x24 || (d[0] & 0x7F) == 0x22 || d[0] == 0x13;
}

/* 这一帧是不是 ag2 协议帧（控制包 / channel 包 / 分片数据帧）。
 *
 * 为什么不能只认 [00 00] 前缀：认证层的分片数据帧长这样
 *   [帧号 1B][00][数据 ≤18B]      ← 0x0B appRand / 0x0A appConfirm 都走这里
 * 首字节是 1..N 的帧号，不带 [00 00] 头。只按前缀判断会把最关键的两包
 * 漏给 miot_device_on_rx，认证状态机永远等不到 appRand → 卡在那一跳。 */
static int looks_ag2(const uint8_t *b, uint16_t n)
{
    if (n < 2) return 0;
    if (b[0] == 0x00 && b[1] == 0x00) return 1;
    if (b[1] == 0x00 && b[0] >= 1 && b[0] <= 64 &&
        s_auth && s_auth->ag_expect > 0) return 1;
    return 0;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "rd %04X from phone", u);
        switch (u) {
        case 0x0001: {
            /* Token 特征（12B，write+notify）。
             * ★夜灯2（0x03DD）这类老型号的页面登录走的是 token 通道：
             *   反编译里 login 路径 x51 订阅的是 0x0001，不是 0x0010。
             * 之前表里没有这个特征，App subscribe 直接失败，连接 1.2s 就断，
             * 界面报「连接超时」。这里先把绑定得到的 token 回给它。 */
            static const uint8_t ff[12] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                           0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            const miot_auth_identity_t *id = s_auth ? miot_auth_identity(s_auth) : NULL;
            if (id && s_auth->bound) return os_mbuf_append(ctxt->om, id->token, 12);
            return os_mbuf_append(ctxt->om, ff, sizeof(ff));
        }
        case 0x0002:
            return os_mbuf_append(ctxt->om, s_pid_le, sizeof(s_pid_le));
        case 0x0004:
            evt(10, 0, 0, 0);
            return os_mbuf_append(ctxt->om, s_fw_ver, sizeof(s_fw_ver) - 1);
        case 0x0013:
            /* 绑定时 App 会把 20B Device ID 写进来，之后再读回校验。
             * 内存没有时回退到认证层持久化的 did（NVS 恢复），
             * 都不行才回序列号——回错会让 App 认为这是新设备。 */
            if (s_devid_len) return os_mbuf_append(ctxt->om, s_devid, s_devid_len);
            {
                const miot_auth_identity_t *id =
                    (s_auth && s_auth->bound) ? miot_auth_identity(s_auth) : NULL;
                if (id) {
                    size_t n = sizeof(id->did);
                    while (n && id->did[n - 1] == 0x00) n--;
                    if (n) return os_mbuf_append(ctxt->om, id->did, n);
                }
            }
            return os_mbuf_append(ctxt->om, s_serial, sizeof(s_serial) - 1);
        case 0x0014: {
            /* Beacon Key（16B）。真机 App 连上后读它解广播；老型号约定返回
             * RC4(token, bkey)，App 用同一 token 解出 bkey 再解 0xFE95 帧。 */
            static const uint8_t zero[16] = {0};
            const miot_auth_identity_t *id = s_auth ? miot_auth_identity(s_auth) : NULL;
            const uint8_t *tok = cur_token();
            if (tok && id) {
                uint8_t out[16];
                rc4(tok, 12, id->bind_key, out, 16);
                return os_mbuf_append(ctxt->om, out, sizeof(out));
            }
            return os_mbuf_append(ctxt->om, zero, sizeof(zero));
        }
        case 0x001B: {
            /* spec 版本字节（优化连接模型 LOGIN 后也会读它） */
            static const uint8_t spec_ver = 0x02;
            return os_mbuf_append(ctxt->om, &spec_ver, 1);
        }
        default:
            return 0;
        }
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

    s_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);

    switch (u) {
    case 0x0001: {
        s_leg_h = s_h01;
        /* 老型号「前绑定」走的就是这条通道：App 把新 token 写在 0x0001 上，
         * 然后在同一句柄等 12B ACK。旧工程跑通的时序是
         *   WRITE 0x0001 len=12 → NOTIFY h=16 len=12 → WRITE 0x0001 len=4(BOND)
         * 之前这里只打印不回应，App 干等 15s 后 reason=0x213 断开。 */
        ESP_LOGW(TAG, "wr 0001 len=%u: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 got, got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0,
                 got > 2 ? buf[2] : 0, got > 3 ? buf[3] : 0,
                 got > 4 ? buf[4] : 0, got > 5 ? buf[5] : 0,
                 got > 6 ? buf[6] : 0, got > 7 ? buf[7] : 0,
                 got > 8 ? buf[8] : 0, got > 9 ? buf[9] : 0,
                 got > 10 ? buf[10] : 0, got > 11 ? buf[11] : 0);
        s_wr01++;
        evt(5, (uint8_t)got, 0, (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3]);
        if (got == 12) {
            s_tok_this_conn = 1;
            legacy_adopt_token(buf);
        } else if (got == 4) {
            /* BOND（cipher(token, 0x0B9D1403)）或 LOGIN 确认都可能写在这个句柄上 */
            legacy_confirm_4b(buf, s_h01);
        }
        break;
    }
    case 0x0013:
        /* 20B Device ID（绑定确认后 App 写入）。
         * ★必须同时转存到认证层：device info 帧靠它声明「我是哪台设备」，
         *   只存在 GATT 层的话 auth->id.did 永远是 00 00 00 00。 */
        if (got <= sizeof(s_devid)) {
            memcpy(s_devid, buf, got);
            s_devid_len = (uint8_t)got;
            if (s_auth) miot_auth_set_did(s_auth, buf, got);
        }
        evt(11, (uint8_t)got, 0, 0);
        ESP_LOGW(TAG, "wr 0013 devid len=%u: %02X %02X %02X %02X ...",
                 got, got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0,
                 got > 2 ? buf[2] : 0, got > 3 ? buf[3] : 0);
        break;
    case 0x0016:
    case 0x0017:
    case 0x0018:
        /* 这三个通道主要是「存在性」用途（插件做能力判断），写入接受但不处理，
         * 返回 ATT 错误会让框架层直接判死 */
        ESP_LOGI(TAG, "wr %04X len=%u: %02X %02X %02X %02X",
                 u, got, got > 0 ? buf[0] : 0, got > 1 ? buf[1] : 0,
                 got > 2 ? buf[2] : 0, got > 3 ? buf[3] : 0);
        break;
    case 0x0010:
        s_wr10++;
        s_leg_h = s_h10;
        /* 命令很短（1B 或 4B），打印首字节足够定位是哪一步 */
        ESP_LOGI(TAG, "wr 0010 #%lu len=%u cmd=%02X", (unsigned long)s_wr10, got, got ? buf[0] : 0);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, got > 32 ? 32 : got, ESP_LOG_INFO);
        /* ★顺序至关重要：legacy_on_0010 的兜底是 `return legacy_confirm_4b(...)`，
         * 也就是说 0x0010 上**任何 4 字节写入都会被它当登录/绑定确认吞掉**；
         * 而标准认证的 CMD_REGISTER(0xA2) / CMD_LOGIN(0x24) / bind-result(0x13)
         * 恰好也是 4 字节。这段判断原来写在 legacy 之后 —— 永远走不到，
         * ECDH 认证一次都没被触发过。标准命令必须先分派。 */
        if (is_auth_cmd(buf, got)) {
            ESP_LOGW(TAG, "std auth cmd len=%u first=%02X -> 认证层", got, got ? buf[0] : 0);
            if (s_auth) miot_auth_on_0010(s_auth, buf, got);
        } else if (legacy_on_0010(buf, got)) {
            /* 老型号 RC4 流程已处理 */
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x0019:
        s_wr19++;
        if (!s_ag_h) s_ag_h = s_h19;
        s_rx_chan = s_h19;      /* 本帧来路面：后续应答回这里 */
        ESP_LOGD(TAG, "wr 0019 #%lu len=%u", (unsigned long)s_wr19, got);
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x001A:
        s_wr1a++;
        if (!s_ag_h) s_ag_h = s_h1a;
        s_rx_chan = s_h1a;      /* ★登录后 App 的 ag2 全在这里，ACK 必须回来 */
        ESP_LOGI(TAG, "wr 001A #%lu len=%u cmd=%02X", (unsigned long)s_wr1a, got, got ? buf[0] : 0);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, got > 32 ? 32 : got, ESP_LOG_INFO);
        /* ★ag2 / channel 帧统一交给认证层：
         * 它有 START→ACK、mng→测试包、分片重组、channel data/ack 的完整分支，
         * 我们在这里再补一个 ACK 只会造成重复应答、把 App 的状态机顶乱。 */
        /* ★2026-11-12 21:36 网关实测（central = 46:7D:0B:3F:E9:36）：
         * 认证一通过，网关立刻发 ag2 数据帧要数据，但 looks_ag2() 按
         * 「认证层还想收数据」(ag_expect>0) 把这些帧判给了认证层 —— 连 START
         * 一起吃掉。结果 spec 侧的 ag2 状态机永远收不到 START（rx_expect=0），
         * 后续数据帧全部按 stray 丢弃，日志表现就是
         *   `dispatch: no consumer (pkg=0 len=11)`
         * 网关等 4.7s 拿不到回应，退订并断开（reason=0x213）。
         * 修法：登录完成后 0x001A 一律交给 spec 层（START/ACK/DATA 全归它的
         * ag2 状态机），认证层只在未登录时接管。 */
        if (looks_ag2(buf, got) && !(s_auth && miot_auth_is_logged_in(s_auth))) {
            ESP_LOGW(TAG, "spec: 0x001A ag2 帧 len=%u -> 认证层（未登录）", got);
            if (s_auth) miot_auth_on_0019(s_auth, buf, got);
            break;
        }
        /* 通知上层的「App 主动发起了一次属性请求」。
         * 米家插件点「测量」时并不会发任何“开始称重”的指令，唯一的痕迹就是
         * 连发两次属性请求（实测间隔稳定 6s；平时心跳是 30~36s）。上层靠这个
         * 间隔判断“用户点了测量”，替它把称重数据推上去。 */
        miot_demo_app_req_hook();
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    case 0x001B:
        s_wr1b++;
        if (!s_ag_h) s_ag_h = s_h1b;
        s_rx_chan = s_h1b;
        ESP_LOGI(TAG, "wr 001B #%lu len=%u cmd=%02X", (unsigned long)s_wr1b, got, got ? buf[0] : 0);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, got > 32 ? 32 : got, ESP_LOG_INFO);
        if (looks_ag2(buf, got) && !(s_auth && miot_auth_is_logged_in(s_auth))) {
            ESP_LOGW(TAG, "spec: 0x001B ag2 帧 len=%u -> 认证层（未登录）", got);
            if (s_auth) miot_auth_on_0019(s_auth, buf, got);
            break;
        }
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        /* 0x0001 Token：App 前绑定时下发 12B（write+notify，真机无 READ） */
        .uuid = &s_c01_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h01,
    },
    {
        /* 0x0002 Product ID：真机只读（之前为定位问题加的 NOTIFY 已撤，
         * 属性/描述符数量必须与真机一致，否则 App 服务发现后走错分支） */
        .uuid = &s_c02_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h02,
    },
    {
        /* 0x0004 Version：真机只读 */
        .uuid = &s_c04_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h04,
    },
    {
        .uuid = &s_c10_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h10,
    },
    {
        .uuid = &s_c13_uuid.u,
        .access_cb = gatt_access,
        /* ★必须可写：绑定确认后 App 会 WRITE 0x0013 20B 写入 Device ID
         *   （mi_beacon v121 实测），只读会被 Android 框架层直接拒绝 */
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_h13,
    },
    {
        /* 0x0014 Beacon Key：App 连上后读它解广播（真机只读） */
        .uuid = &s_c14_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h14,
    },
    {
        /* 0x0016/0x0017/0x0018：真机存在，旧流程（挑战/加密 token）探测桩。
         * ★2026-11-11：之前为了压缩发现耗时把它们删了，结果是 App 连上后
         * 走错了分支（只订阅 001B/001A/0001、一条认证命令都不写）。参考工程
         * 有明确记录：这类特征缺失会让 App 拿到 ATT "attribute not found"
         * 后静默卡住。现已按真机属性恢复。 */
        .uuid = &s_c16_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h16,
    },
    {
        .uuid = &s_c17_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h17,
    },
    {
        .uuid = &s_c18_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h18,
    },
    {
        .uuid = &s_c19_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h19,
    },
    {
        .uuid = &s_c1a_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1a,
    },
    {
        .uuid = &s_c1b_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1b,
    },
    { 0 },
};

/* ===== philips.light.nlight 文本协议（跑在 Nordic UART 上） =====
 *
 * 依据：手机上 dump 的插件包 15_main.bundle（_receiveBluetoothData）反解，
 *       不依赖 hook、不依赖抓包，属于可长期维护的实现依据。
 *
 *   App → 设备：ASCII 命令，空格分隔，无帧头无校验
 *       notify_ol / get_prop power bright lux snm dur / get_s_prop scene
 *       set_bright <1..100> / set_duration <5000..60000>
 *       set_bridur <b> <dur> / apply_fixed_scene <1..4> / set_scene <id> <b> <dur>
 *   设备 → App：ASCII JSON，必须带 flag 字段，且长度 ≥ 15 字符
 *       {"flag":"get_prop","result":{"power":1,"bright":58,"lux":1,"snm":0,"dur":30000}}
 *       {"flag":"get_s_prop","result":{"scene":[{"bright":60,"dur":25000},…]}}
 *
 * ★插件对**每一条通知**单独 JSON.parse，所以一帧应答必须完整落在一个通知里，
 *   不能跨通知分片 —— MTU 必须 ≥ 应答长度 + 3（我们主动协商 247，够用）。
 */
#define NL_SCENE_N 4

typedef struct {
    uint8_t  bright;
    uint32_t dur;
} nl_scene_t;

static struct {
    uint8_t     power, bright, lux, snm;
    uint32_t    dur;
    nl_scene_t  sc[NL_SCENE_N];
    uint32_t    rx_cnt;
} s_nl = {
    .power = 1, .bright = 60, .lux = 1, .snm = 0, .dur = 25000,
    .sc = { { 60, 25000 }, { 20, 30000 }, { 100, 10000 }, { 40, 5000 } },
};

/* 插件会**连续三发**（notify_ol / get_prop / get_s_prop），所以应答必须排队，
 * 不能在 30ms 定时器里只留一个槽位——那会把前两条覆盖掉。
 * 队列每 20ms 取一帧，发空后停表。 */
#define NL_QN 6
typedef struct {
    uint8_t n;
    char    js[176];
} nl_msg_t;

static nl_msg_t s_nl_q[NL_QN];
static uint8_t  s_nl_qh, s_nl_qt;
static esp_timer_handle_t s_nl_timer;

/* 写回调里不能直接 notify（ATT 的写响应还没发出去），由定时器延后送。 */
static void nl_send_now(void *arg)
{
    (void)arg;
    if (s_nl_qh == s_nl_qt) {
        if (s_nl_timer) esp_timer_stop(s_nl_timer);
        return;
    }
    nl_msg_t *m = &s_nl_q[s_nl_qh];
    int rc = gatt_miot_nus_notify((const uint8_t *)m->js, m->n);
    ESP_LOGW(TAG, "NL tx len=%u rc=%d: %.*s", (unsigned)m->n, rc, (int)m->n, m->js);
    s_nl_qh = (uint8_t)((s_nl_qh + 1) % NL_QN);
}

static void nl_queue(const char *js, size_t n)
{
    if (!n) return;
    if (n > sizeof(s_nl_q[0].js)) n = sizeof(s_nl_q[0].js);
    uint8_t next = (uint8_t)((s_nl_qt + 1) % NL_QN);
    if (next == s_nl_qh) {              /* 满：丢最旧，保证最新状态能出去 */
        s_nl_qh = (uint8_t)((s_nl_qh + 1) % NL_QN);
        ESP_LOGW(TAG, "NL 队列满，丢最旧一帧");
    }
    s_nl_q[s_nl_qt].n = (uint8_t)n;
    memcpy(s_nl_q[s_nl_qt].js, js, n);
    s_nl_qt = next;

    if (!s_nl_timer) {
        const esp_timer_create_args_t a = { .callback = nl_send_now, .name = "nl" };
        if (esp_timer_create(&a, &s_nl_timer) != ESP_OK) s_nl_timer = NULL;
    }
    if (s_nl_timer && !esp_timer_is_active(s_nl_timer)) {
        esp_timer_start_periodic(s_nl_timer, 20 * 1000);
    }
}

static uint8_t nl_bright_clamp(long v) { return (uint8_t)(v < 1 ? 1 : (v > 100 ? 100 : v)); }
static uint32_t nl_dur_clamp(long v)   { return (uint32_t)(v < 5000 ? 5000 : (v > 60000 ? 60000 : v)); }

static void nl_reply_prop(void)
{
    char js[176];
    int n = snprintf(js, sizeof(js),
                     "{\"flag\":\"get_prop\",\"result\":{\"power\":%u,\"bright\":%u,"
                     "\"lux\":%u,\"snm\":%u,\"dur\":%lu}}",
                     (unsigned)s_nl.power, (unsigned)s_nl.bright,
                     (unsigned)s_nl.lux, (unsigned)s_nl.snm, (unsigned long)s_nl.dur);
    if (n > 0) nl_queue(js, (size_t)n);
}

static void nl_reply_scene(void)
{
    char js[224];
    int n = snprintf(js, sizeof(js),
                     "{\"flag\":\"get_s_prop\",\"result\":{\"scene\":["
                     "{\"bright\":%u,\"dur\":%lu},{\"bright\":%u,\"dur\":%lu},"
                     "{\"bright\":%u,\"dur\":%lu},{\"bright\":%u,\"dur\":%lu}]}}",
                     (unsigned)s_nl.sc[0].bright, (unsigned long)s_nl.sc[0].dur,
                     (unsigned)s_nl.sc[1].bright, (unsigned long)s_nl.sc[1].dur,
                     (unsigned)s_nl.sc[2].bright, (unsigned long)s_nl.sc[2].dur,
                     (unsigned)s_nl.sc[3].bright, (unsigned long)s_nl.sc[3].dur);
    if (n > 0) nl_queue(js, (size_t)n);
}

/* 场景变化后，若当前 bri/dur 正好等于某个固定场景，就把 snm 指过去，
 * 让 App 的场景高亮与真实状态自洽。 */
static void nl_match_scene(void)
{
    s_nl.snm = 0;
    for (int i = 0; i < NL_SCENE_N; i++) {
        if (s_nl.sc[i].bright == s_nl.bright && s_nl.sc[i].dur == s_nl.dur) {
            s_nl.snm = (uint8_t)(i + 1);
            break;
        }
    }
}

/* 解析并处理一条 App 命令（ASCII） */
void gatt_miot_nl_input(const char *cmd)
{
    char buf[128];
    size_t n = strlen(cmd);
    while (n && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n' || cmd[n - 1] == ' ')) n--;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, cmd, n);
    buf[n] = 0;

    /* 切词：空格可连续 */
    char *tok[8];
    int nt = 0;
    for (char *p = strtok(buf, " "); p && nt < 8; p = strtok(NULL, " ")) tok[nt++] = p;
    if (!nt) return;

    s_nl.rx_cnt++;

    if (!strcmp(tok[0], "notify_ol")) {
        char js[64];
        int k = snprintf(js, sizeof(js), "{\"flag\":\"notify_ol\",\"result\":{\"power\":%u}}",
                         (unsigned)s_nl.power);
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }
    if (!strcmp(tok[0], "get_prop")) { nl_reply_prop(); return; }
    if (!strcmp(tok[0], "get_s_prop")) { nl_reply_scene(); return; }

    if (!strcmp(tok[0], "set_bright") && nt >= 2) {
        s_nl.bright = nl_bright_clamp(atol(tok[1]));
        s_nl.power = s_nl.bright ? 1 : 0;
        nl_match_scene();
        char js[72];
        int k = snprintf(js, sizeof(js),
                         "{\"flag\":\"set_bright\",\"result\":{\"bright\":%u}}", (unsigned)s_nl.bright);
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }
    if (!strcmp(tok[0], "set_duration") && nt >= 2) {
        s_nl.dur = nl_dur_clamp(atol(tok[1]));
        nl_match_scene();
        char js[80];
        int k = snprintf(js, sizeof(js),
                         "{\"flag\":\"set_duration\",\"result\":{\"dur\":%lu}}", (unsigned long)s_nl.dur);
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }
    if (!strcmp(tok[0], "set_bridur") && nt >= 3) {
        s_nl.bright = nl_bright_clamp(atol(tok[1]));
        s_nl.dur = nl_dur_clamp(atol(tok[2]));
        s_nl.power = 1;
        nl_match_scene();
        char js[96];
        int k = snprintf(js, sizeof(js),
                         "{\"flag\":\"set_bridur\",\"result\":{\"bright\":%u,\"dur\":%lu}}",
                         (unsigned)s_nl.bright, (unsigned long)s_nl.dur);
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }
    if (!strcmp(tok[0], "apply_fixed_scene") && nt >= 2) {
        int id = atoi(tok[1]);
        if (id >= 1 && id <= NL_SCENE_N) {
            s_nl.snm = (uint8_t)id;
            s_nl.bright = s_nl.sc[id - 1].bright;
            s_nl.dur = s_nl.sc[id - 1].dur;
            s_nl.power = 1;
        }
        char js[112];
        int k = snprintf(js, sizeof(js),
                         "{\"flag\":\"apply_fixed_scene\",\"result\":{\"snm\":%u,\"bright\":%u,\"dur\":%lu}}",
                         (unsigned)s_nl.snm, (unsigned)s_nl.bright, (unsigned long)s_nl.dur);
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }
    if (!strcmp(tok[0], "set_scene") && nt >= 4) {
        int id = atoi(tok[1]);
        if (id >= 1 && id <= NL_SCENE_N) {
            s_nl.sc[id - 1].bright = nl_bright_clamp(atol(tok[2]));
            s_nl.sc[id - 1].dur = nl_dur_clamp(atol(tok[3]));
        }
        nl_match_scene();
        char js[112];
        int k = snprintf(js, sizeof(js),
                         "{\"flag\":\"set_scene\",\"result\":{\"id\":%d,\"bright\":%u,\"dur\":%lu}}",
                         id, (unsigned)((id >= 1 && id <= NL_SCENE_N) ? s_nl.sc[id - 1].bright : 0),
                         (unsigned long)((id >= 1 && id <= NL_SCENE_N) ? s_nl.sc[id - 1].dur : 0));
        if (k > 0) nl_queue(js, (size_t)k);
        return;
    }

    ESP_LOGW(TAG, "NL 未识别命令: %s", cmd);
}

void gatt_miot_nl_dump(void)
{
    ESP_LOGW(TAG, "nl: power=%u bright=%u lux=%u snm=%u dur=%lu rx=%lu",
             (unsigned)s_nl.power, (unsigned)s_nl.bright, (unsigned)s_nl.lux,
             (unsigned)s_nl.snm, (unsigned long)s_nl.dur, (unsigned long)s_nl.rx_cnt);
    for (int i = 0; i < NL_SCENE_N; i++) {
        ESP_LOGW(TAG, "nl: scene%d bright=%u dur=%lu", i + 1,
                 (unsigned)s_nl.sc[i].bright, (unsigned long)s_nl.sc[i].dur);
    }
}

/* ===== NUS 通道的访问回调 =====
 * RX=6E400002 收 ASCII 命令 → gatt_miot_nl_input() → 30ms 后在 TX 上回 JSON；
 * TX=6E400003 只通知（可读，长度 0）。串口 `nus <hex>` 可手工回一帧留作调试。 */
static int nus_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    (void)conn_handle;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[128];
        uint8_t raw[128];
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        uint16_t cp = n > sizeof(raw) ? sizeof(raw) : n;
        ble_hs_mbuf_to_flat(ctxt->om, raw, cp, &cp);
        uint16_t k = cp > sizeof(buf) - 1 ? (uint16_t)(sizeof(buf) - 1) : cp;
        memcpy(buf, raw, k);
        buf[k] = 0;
        ESP_LOGW(TAG, "NUS rx h=%u len=%u: %s", attr_handle, n, buf);
        gatt_miot_nl_input(buf);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t z = 0;
        return os_mbuf_append(ctxt->om, &z, 0);   /* 空值 */
    }

    return 0;
}

static const struct ble_gatt_chr_def s_nus_chrs[] = {
    {   /* RX：App → 设备 */
        .uuid = &s_nus_rx_uuid.u,
        .access_cb = nus_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_hn_rx,
    },
    {   /* TX：设备 → App（通知）。额外允许写：App 对 6E400003 同时发起了
         * 订阅和写（两个 cmd 的参数一模一样），属性上放开以免又被本地拒掉。 */
        .uuid = &s_nus_tx_uuid.u,
        .access_cb = nus_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ |
                 BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_hn_tx,
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
    ESP_LOGI(TAG, "0xFE95 service registered (0001/0002/0004/0010/0013/0014/0019/001A/001B)");
    ESP_LOGI(TAG, "NUS service registered (6E400001: 6E400002 write / 6E400003 notify)");
}

int gatt_miot_nus_notify(const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_hn_tx == 0) {
        ESP_LOGW(TAG, "NUS notify: no conn/handle (conn=%d h=%u)", s_conn, s_hn_tx);
        return -1;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)len);
    if (!om) return -1;
    int rc = ble_gatts_notify_custom(s_conn, s_hn_tx, om);
    ESP_LOGW(TAG, "NUS tx h=%u len=%u rc=%d", s_hn_tx, (unsigned)len, rc);
    return rc;
}

void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev)
{
    s_auth = auth;
    s_dev = dev;
}

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu)
{    s_conn = conn_handle;
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    s_wr10 = s_wr19 = s_wr1a = s_wr1b = 0;
    s_wr01 = 0;
    s_tok_this_conn = 0;
    s_conn_seq++;

    /* ★重启后 s_ltok 是空的（纯内存变量），而 bound/token 是从 NVS 恢复的。
     * 不补这一步的话，登录轮的 GETRAND 会误走「ECDH 多候选」分支，拿假密钥
     * 去应答，App 永远等不到正确答案 —— 表现为收到应答 1s 后断开。
     * 绑定过的设备，登录密钥就是 App 当初下发的那个 token。 */
    if (!s_legacy_tok_ok && s_auth && s_auth->bound) {
        const miot_auth_identity_t *id = miot_auth_identity(s_auth);
        if (id) {
            memcpy(s_ltok, id->token, 12);
            s_legacy_tok_ok = 1;
            ESP_LOGW(TAG, "legacy token 从 NVS 恢复: %02X%02X%02X%02X%02X%02X...",
                     s_ltok[0], s_ltok[1], s_ltok[2], s_ltok[3], s_ltok[4], s_ltok[5]);
        }
    }

    evt(1, (uint8_t)s_conn_seq, (uint8_t)s_mtu, (uint32_t)s_mtu);
    ESP_LOGI(TAG, "connected conn=%u mtu=%u seq=%u", conn_handle, s_mtu, (unsigned)s_conn_seq);
}

void gatt_miot_on_disconnect(void)
{
    evt(12, (uint8_t)s_wr10, (uint8_t)s_wr01, (uint32_t)s_wr1a);
    ESP_LOGI(TAG, "disconnected (wr 0010=%lu 0019=%lu 001A=%lu 001B=%lu)",
             (unsigned long)s_wr10, (unsigned long)s_wr19,
             (unsigned long)s_wr1a, (unsigned long)s_wr1b);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_ag_h = 0;              /* 新连接重新判定控制帧应答面 */
    s_rx_chan = 0;
    s_leg_h = 0;
    s_mtu = BLE_ATT_MTU_DFLT;
}

void gatt_miot_dump_handles(void)
{
    /* value handle；CCCD 一般紧跟其后（+1/+2 视是否多个描述符而定）。
     * App 的 SUBSCRIBE 事件里给的是 CCCD handle，对照这里就能知道它订的是哪条通道。 */
    ESP_LOGI(TAG, "handles: 0001=%u 0002=%u 0004=%u 0010=%u 0013=%u 0014=%u 0019=%u 001A=%u 001B=%u",
             s_h01, s_h02, s_h04, s_h10, s_h13, s_h14,
             s_h19, s_h1a, s_h1b);
}

/** 诊断：打印当前认证身份（串口 `tok`）。现场核对「我们手里的 token」和
 *  App 手里的 token 是否一致，是判断 GETRAND 应答被拒原因的唯一直证。 */
void gatt_miot_dump_token(void)
{
    const miot_auth_identity_t *id =
        (s_auth && s_auth->bound) ? miot_auth_identity(s_auth) : NULL;
    ESP_LOGW(TAG, "tok: legacy_ok=%d tok_this_conn=%d bound=%d strat=%d grframes=%d",
             s_legacy_tok_ok, s_tok_this_conn,
             s_auth ? (int)s_auth->bound : -1, s_strat, s_gr_frames);
    if (s_legacy_tok_ok) {
        ESP_LOGW(TAG, "  s_ltok  = %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 s_ltok[0], s_ltok[1], s_ltok[2], s_ltok[3], s_ltok[4], s_ltok[5],
                 s_ltok[6], s_ltok[7], s_ltok[8], s_ltok[9], s_ltok[10], s_ltok[11]);
    }
    if (id) {
        ESP_LOGW(TAG, "  nvs.tok = %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 id->token[0], id->token[1], id->token[2], id->token[3],
                 id->token[4], id->token[5], id->token[6], id->token[7],
                 id->token[8], id->token[9], id->token[10], id->token[11]);
        ESP_LOGW(TAG, "  nvs.bkey= %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 id->bind_key[0], id->bind_key[1], id->bind_key[2], id->bind_key[3],
                 id->bind_key[4], id->bind_key[5], id->bind_key[6], id->bind_key[7],
                 id->bind_key[8], id->bind_key[9], id->bind_key[10], id->bind_key[11],
                 id->bind_key[12], id->bind_key[13], id->bind_key[14], id->bind_key[15]);
    }
    ESP_LOGW(TAG, "  last rand=%02X%02X%02X%02X",
             s_rand4[0], s_rand4[1], s_rand4[2], s_rand4[3]);
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

uint32_t gatt_miot_rx_ms(void)
{
    return s_rx_ms;
}

void gatt_miot_disconnect(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    /* 模拟真机会话收尾：温湿度计实测 App 在 LOGIN 完成收完数据 ~4s 后主动断开，
     * 页面随即翻面。驱蚊器因 hook 模块把 PSK 连接重路由，Core 侧完成信号可能
     * 没传回原调用链导致会话挂死 —— 设备侧主动断，逼页面走「断开后展示数据」
     * 的路径。 */
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* 订阅模式表：App 可能写 CCCD=0x0001(notify) 或 0x0002(indicate)。
 * 之前只声明 NOTIFY，三个特征订阅后 cur_notify 一直是 0，怀疑 App 用的是
 * indicate —— 那样我们 notify 出去它收不到，正好表现为「回包后 1s 断开」。 */
static uint8_t s_ind_mode[64];

void gatt_miot_on_subscribe(uint16_t handle, int notify, int indicate)
{
    if (handle < 64) {
        s_ind_mode[handle] = indicate ? 1 : 0;
        s_sub[handle] = (notify || indicate) ? 1 : 0;
    }
    ESP_LOGI(TAG, "sub h=%u notify=%d indicate=%d -> %s", handle, notify, indicate,
             indicate ? "INDICATE" : "NOTIFY");
    if (notify && handle == s_h01) miot_token_subscribed_hook();

    /* 订阅一开就把认证应答面定下来，后面所有认证 notify 都往这条通道走。 */
    if (notify || indicate) {
        uint16_t ac = auth_chan();
        ESP_LOGI(TAG, "auth chan = h%u (%s)", ac,
                 ac == s_h19 ? "0x0019" : (ac == s_h1a ? "0x001A" : "0x001B"));
    }

    /* ★设备先说话探针（串口 `spec N` 开启）。
     * 现场事实：页面会话 connect → 发现 → 订阅 001B/001A/0001 → 20ms~1s 内
     * cancelOpen，全程零读写；连「strat=0 的正确答案」它也不认。
     * 唯一能让 App 立刻回话的是**设备先发一条控制帧**：
     *   spec 1 = ag2 START（已验证：App 90ms 内回 00 00 01 01，但之后无进展）
     *   spec 2 = mng 建链帧（本轮：目标是把它推进到标准认证 ECDH） */
    if ((notify || indicate) && (handle == s_h1b || handle == s_h1a)) {
        if (s_spec_probe == 1) {
            spec_probe_start(handle);
        } else if (s_spec_probe == 2) {
            spec_probe_link_once(handle);
        }
    }
}

static uint16_t s_notify_fail;

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

    int rc = (handle < 64 && s_ind_mode[handle])
                 ? ble_gatts_indicate_custom(s_conn, handle, om)
                 : ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0 && rc != BLE_HS_EDONE) {
        /* 未订阅时返回 ATT 错误是正常的：App 还没写 CCCD。
         * 但如果是「一直没人订阅」，这里就是数据到不了 App 的原因，
         * 所以前几次用 WARN 打出来，别让它静默消失。 */
        if (s_notify_fail < 6) {
            s_notify_fail++;
            ESP_LOGW(TAG, "notify h=%u len=%u rc=%d (nobody subscribed?)",
                     handle, (unsigned)len, rc);
        }
    } else {
        s_notify_fail = 0;
    }
    return (rc == 0 || rc == BLE_HS_EDONE) ? 0 : -1;
}

/** 往 0x0001（token 通道）推一帧。
 *
 * 页面模式（App 打开详情页）订阅 0x0001/001A/001B 之后会**等设备主动推帧**：
 * logcat 实证订阅完成整 1000ms 内零收包 -> 先退订三个通道再干净断开，
 * 然后 4~6 秒重来一次，界面就是「连接超时」。真机行为是订阅后立即推一帧
 * MiBeacon（v5 加密），之后每 2s 一次心跳。 */
int gatt_miot_notify_token(const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_h01) return -1;
    return notify_chr(s_h01, d, len);
}

/** 同一帧发到 0x0001 / 0x001A / 0x001B 三个已订阅通道。
 *
 * 页面模式只推 0x0001 时 App 仍会退订（实测），而它同时订阅了这三个，
 * 真机可能把首页数据挂在 0x001B（优化连接模型的 spec 通道）。三发成本极低。 */
int gatt_miot_notify_broadcast(const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    /* ★2026-11-10 13:0x 定论：只在 0x001A（spec 数据入）上推。
     * 之前三个通道同发，把 18B 的 MiBeacon v5 帧也塞进了 0x0001（token 认证
     * 通道）和 0x001B。App 在 0x0001 上等的是 4B/12B 认证帧，收到 18B 直接判
     * 协议错 —— 现场日志里每一轮都是「订阅 h=16 → 30ms 后 push 18B → App 不再
     * 发任何认证命令 → 1s 后退订断开」，正是页面「连接超时」的成因。
     * 0x0001 上的每一帧都必须服从认证状态机，绝不能混数据。 */
    if (!s_h1a) return -1;
    return notify_chr(s_h1a, d, len) == 0 ? 0 : -1;
}

int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* ★spec 数据双通道同发（2026-11-07 logcat 定位）：
     * - 0x0010：温湿度计实测的页面数据通道（非优化模型唯一路径）；
     * - 0x001B：优化连接模型（zimi.mosq.v1 命中 is6Optimize...）多订出来的
     *   通道（hook: "BLE notify req ... 0000001b _m_j.wy0"）。
     * 两条 App 都订阅了（0x001A 也订），各收一份由 spec tid 幂等去重；
     * 只发一边时另一边的消费方就一直等不到，页面停在「设备初始化」。 */
    notify_chr(s_h10, d, len);

    /* ★ag2 链路层应答必须回到 0x001A（23:45 定位）。
     * App 把自己的 spec 帧写在 0x001A 上，然后就在 0x001A 等链路层 ACK；
     * 我们之前把 ACK 只发到 0010/001B，它永远收不到应答，于是：
     *   wr 001A #N len=6: 00 00 00 00 03 00   ← START，声明 3 个分片
     *   之后一个分片都不来，30s 后重试（页面上就是「点了没反应」）。
     * 形状固定是 4 字节 00 00 01 xx，只把这种控制帧补一路到 001A，
     * 数据帧仍走 0010/001B，避免同一条数据被两条通道各处理一次。 */
    if (len == 4 && d[0] == 0 && d[1] == 0 && d[2] == 1)
        notify_chr(s_h1a, d, len);

    return notify_chr(s_h1b, d, len);
}

int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* 结果码（0x11 注册成功 / 0x21 登录成功 / 0xE2 失败）按协议必须落在 0x0010。
     * 但夜灯2 详情页根本没订阅 0x0010 —— 发给没订阅的特征直接失败，
     * 所以订阅缺失时改投认证通道，宁可在「App 收得到的地址」上说同一句话。 */
    if (s_h10 && s_h10 < 64 && s_sub[s_h10]) return notify_chr(s_h10, d, len);
    uint16_t ac = auth_chan();
    ESP_LOGW(TAG, "result code len=%u: 0x0010 未订阅, 改投 h%u", (unsigned)len, ac);
    return notify_chr(ac, d, len);
}

/** spec 数据面：START(`00 00 00`) / SINGLE(`00 00 02`) / 数据帧(首字节=seq)
 *  都只能走 0x001A —— docs/protocol.md §2 实测结论：App 只在 0x001A 收 notify，
 *  0x001B 上的 notify 被它当 IDLE 推送会话直接丢弃。
 *  而控制帧（ACK `00 00 01` / mng `00 00 04` / 测试包 `00 00 05`）必须回
 *  App 写进来的那条面（s_ag_h）。两者混用会整包拆到两条通道上，App 拼不起来。 */
static uint16_t spec_data_chan(void)
{
    if (s_h1a && s_h1a < 64 && s_sub[s_h1a]) return s_h1a;
    return auth_chan();
}

int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    uint16_t h;
    int is_ctrl = (len >= 2 && d[0] == 0x00 && d[1] == 0x00 &&
                   !(len >= 3 && (d[2] == 0x00 || d[2] == 0x02)));
    /* ★登录完成之前，device info / dev_rand(0x0D) / dev_confirm(0x0C) 这些
     * 认证 parcel 必须回 App 写入的那条面（0x0019）。原来一律走 0x001A，
     * App 的分片层照样 ACK（所以看起来"收到了"），但认证状态机只盯 0x0019，
     * 于是永远等不到 0x0C → 静默 15s → reason=0x213 断开。
     * 实测依据：S400 真机抓包 dev_rand/remote_info 都在 0x0019 上。 */
    int in_auth = !(s_auth && miot_auth_is_logged_in(s_auth));
    /* 优先逐帧回到「来路面」（s_rx_chan），它有订阅才用；否则退回旧逻辑。 */
    if (s_rx_chan && s_rx_chan < 64 && s_sub[s_rx_chan]) h = s_rx_chan;
    else h = (is_ctrl || in_auth) ? auth_chan() : spec_data_chan();
    int rc = notify_chr(h, d, len);
    ESP_LOGW(TAG, "auth notify %uB -> h%u rc=%d (%s ag_h=%u rx=%u)",
             (unsigned)len, h, rc, is_ctrl ? "ctrl" : "data", s_ag_h, s_rx_chan);
    return rc;
}
