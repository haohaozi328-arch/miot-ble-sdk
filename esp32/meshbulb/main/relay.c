/**
 * @file relay.c
 * @brief BLE 中转（relay）：对手机假装成 mesh 灯，同时作为客户端去连真灯，
 *        把 0010/0016/001A/001B/001C 的读写原样转发。
 *
 * 目的：真机只有在**真正与云端交互**的会话里才会吐出 pkg4。我们伪造不了云端
 * （真机会校验证书链），所以让真手机 + 真云端去跟真灯握手，我们只在中转里
 * **顺手抄一份 pkg4**。
 *
 * 关键设计：
 *   - ESP32 开机后立刻去连真灯（真灯一旦被连上就停止广播，App 扫描时只会
 *     看到我们的 ESP32，不会出现两台同型号设备）
 *   - 手机写进来的帧 → ble_gattc_write_flat 转给真灯
 *   - 真灯的 notify → 用本机 GATT 的对应特征转发给手机
 *   - 其余（PID/版本号/did 读取）仍由原固件应答
 */
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "relay.h"
#include "gatt_miot.h"

static const char *TAG = "RELAY";

#define UUID_MIOT_SVC 0xFE95

typedef struct {
    uint16_t uuid16;
    uint16_t val_handle;      /* 对端（真灯）特征值句柄 */
    uint16_t cccd_handle;
} relay_chr_t;

static relay_chr_t s_chr[] = {
    { 0x0010, 0, 0 },
    { 0x0016, 0, 0 },
    { 0x001A, 0, 0 },
    { 0x001B, 0, 0 },
    { 0x001C, 0, 0 },
};
#define N_CHR (sizeof(s_chr) / sizeof(s_chr[0]))

static ble_addr_t s_target;
static int s_have_target;
/* ★central 角色专用随机地址（static random，最高两 bit = 11）。
 * 为什么需要：relay 模式下我们的 public 地址被强制成真灯的 MAC（为了
 * /v2/blemesh/bind 的 mac-vs-证书 一致性），而**同一个地址不能既当
 * peripheral 又去连同地址的 peripheral** —— 真灯会忽略这种连接请求
 * （实测 status=13 超时，App 侧表现 -7）。所以连真灯时换随机地址。 */
static const uint8_t RND_OWN_ADDR[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC0 };
static uint16_t s_lamp = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static volatile int s_enabled;
static volatile int s_connecting;      /* 正在向真灯发起连接，防重复发起 */

/* ─────────────────────────────────────────────────────────────────────────
 * 写队列（2026-11-20，-28 的直接原因之一）
 *
 * App 往 0016 推数据帧是**连发**的（每帧 20B，间隔只有几毫秒），而对真灯的
 * 一次 GATT 写要等一个连接事件（~30ms）才回来。第二条帧进到
 * ble_gattc_write_flat 时上一条还没完成，NimBLE 回 BLE_HS_EBUSY，帧就丢了。
 * 丢一帧 → App 那边整包校验失败 → 报「数据传输过程中，数据发送失败」= -28。
 *
 * 所以这里必须排队串行发：一条在飞，其余进环形队列，完成回调里再发下一条。
 * 注意**不能**在 relay_forward 里原地重试 —— 那是跑在 NimBLE host task 上的，
 * 阻塞 host task 等自己的完成回调就是死锁。
 * ───────────────────────────────────────────────────────────────────────── */
#define Q_SLOTS  40
#define Q_MAXLEN 128
typedef struct {
    uint16_t uuid16;
    uint16_t len;
    uint8_t  d[Q_MAXLEN];
} q_item_t;

static q_item_t s_q[Q_SLOTS];
static int s_q_head, s_q_tail, s_q_cnt;
static volatile int s_wr_busy;
static uint32_t s_fwd_ok, s_fwd_drop_qfull, s_fwd_drop_nolamp, s_wr_err;

/* ── 小工具 ── */
static uint16_t uuid16_of(const ble_uuid_any_t *u)
{
    return u->u.type == BLE_UUID_TYPE_16 ? u->u16.value : 0xFFFF;
}
static relay_chr_t *find_chr(uint16_t uuid16)
{
    for (size_t i = 0; i < N_CHR; i++) {
        if (s_chr[i].uuid16 == uuid16) return &s_chr[i];
    }
    return NULL;
}

/* ★完整帧留痕（2026-11-20 新增）
 * 之前只打前 4 字节，导致 pkg9 那 109 字节配网数据一句没留下，无法离线判断
 * 它到底是密文还是简单变换。这里把整帧按 32B 分块打出来，前缀固定为
 *   HX <TX|RX> <uuid> <已打字节>/<总长> <hex...>
 * 事后用 tools/pkg9_entropy.py 抓这行就能重组原始 payload。 */
static void log_hex(const char *dir, uint16_t uuid16, const uint8_t *d, size_t n)
{
    char buf[3 * 32 + 4];
    for (size_t off = 0; off < n; off += 32) {
        size_t m = (n - off) > 32 ? 32 : (n - off);
        size_t k = 0;
        for (size_t i = 0; i < m; i++)
            k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%02X ", d[off + i]);
        ESP_LOGW(TAG, "HX %s %04X %u/%u %s", dir, uuid16,
                 (unsigned)(off + m), (unsigned)n, buf);
    }
}

static void start_scan(void);
static void lamp_write_next(void);
static void connect_target(void);

/* ─────────────────────────────────────────────────────────────────────────
 * 保活（-28 的第二个原因）
 *
 * 实测（logs/relay4/5.log）：真灯对空闲链路约 19s 就主动断开，reason=0x213
 * （0x13 = Remote User Terminated）。米家配网从扫描到 pkg9 要 30s+，链路在
 * 传帧途中断掉 → App 报 -28「数据传输过程中，数据发送失败」。
 *
 * 先试过对 001B 发 Read：真灯直接回 ATT 0x02（Read Not Permitted，rc=258），
 * 而且**不刷新**它的空闲计时器 —— 断开节奏一点没变。说明它数的是"应用层报文"。
 *
 * 所以改成发 001C 的明文探活 `08 01 00`（就是 App 连上后问「你在吗」那一句），
 * 真灯必回 `08 06 00 "hello"`。为了不让手机收到我们自己探活引出的回包，用
 * s_ping_pending 记账，匹配到 hello 回包就吞掉，不转发给手机。
 * ───────────────────────────────────────────────────────────────────────── */
#define KEEP_UUID 0x001C
#define KEEP_MS   3000
#define KEEP_IDLE_MS 7000      /* 链路空闲超过这么久就补一次探活 */
static uint32_t s_keep_ok, s_keep_err;
static volatile int s_ping_pending;
static TickType_t s_last_act;
static TickType_t s_lamp_since;
static volatile int s_phone_on;

static const uint8_t HELLO_REQ[3] = { 0x08, 0x01, 0x00 };
static const uint8_t HELLO_RSP[8] = { 0x08, 0x06, 0x00, 'h', 'e', 'l', 'l', 'o' };

static int ping_cb(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s_wr_busy = 0;
    if (err->status == 0) {
        s_keep_ok++;
        if (s_keep_ok % 10 == 1) ESP_LOGI(TAG, "探活 ok #%lu", (unsigned long)s_keep_ok);
    } else {
        s_keep_err++;
        s_ping_pending = 0;
        ESP_LOGW(TAG, "探活失败 rc=%d (ok=%lu err=%lu)", err->status,
                 (unsigned long)s_keep_ok, (unsigned long)s_keep_err);
    }
    lamp_write_next();
    return 0;
}

static void keepalive_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KEEP_MS));
        if (!s_enabled || s_lamp == BLE_HS_CONN_HANDLE_NONE) continue;
        if (s_wr_busy || s_q_cnt) continue;                /* 正在转发，别插队 */
        if ((xTaskGetTickCount() - s_last_act) < pdMS_TO_TICKS(KEEP_IDLE_MS)) continue;
        relay_chr_t *c = find_chr(KEEP_UUID);
        if (!c || !c->val_handle) continue;
        s_ping_pending = 1;
        int rc = ble_gattc_write_flat(s_lamp, c->val_handle, HELLO_REQ, sizeof(HELLO_REQ),
                                      ping_cb, NULL);
        if (rc == 0) {
            s_wr_busy = 1;
            s_last_act = xTaskGetTickCount();
        } else {
            s_ping_pending = 0;
            if (rc != BLE_HS_EBUSY) ESP_LOGW(TAG, "探活发起失败 rc=%d", rc);
        }
    }
}

/* ── GATT 发现 ── */
static void subscribe_next(int idx);

static int sub_done_cb(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg)
{
    int idx = (int)(intptr_t)arg;
    (void)conn; (void)attr;
    ESP_LOGI(TAG, "subscribe %04X rc=%d", s_chr[idx].uuid16, err->status);
    subscribe_next(idx + 1);
    return 0;
}

static void subscribe_next(int idx)
{
    while (idx < (int)N_CHR && !s_chr[idx].val_handle) idx++;
    if (idx >= (int)N_CHR) {
        ESP_LOGI(TAG, "全部特征已订阅，中继就绪（补发积压 %d 帧）", s_q_cnt);
        s_last_act = xTaskGetTickCount();
        lamp_write_next();          /* ★把手机在建链期间发来的帧补发给真灯 */
        return;
    }
    static uint8_t v[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(s_lamp, s_chr[idx].cccd_handle, v, sizeof(v),
                                  sub_done_cb, (void *)(intptr_t)idx);
    if (rc) {
        ESP_LOGW(TAG, "subscribe %04X 发起失败 rc=%d", s_chr[idx].uuid16, rc);
        subscribe_next(idx + 1);
    }
}
static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && chr) {
        uint16_t u = uuid16_of(&chr->uuid);
        relay_chr_t *c = find_chr(u);
        if (c) {
            c->val_handle = chr->val_handle;
            c->cccd_handle = (uint16_t)(chr->val_handle + 1);
            ESP_LOGI(TAG, "lamp chr %04X val=%u cccd=%u", u, c->val_handle, c->cccd_handle);
        }
    } else if (err->status == BLE_HS_EDONE) {
        subscribe_next(0);      /* 串行订阅：同时下发会被 NimBLE 判 ENOMEM */
    }
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && svc) {
        if (uuid16_of(&svc->uuid) == UUID_MIOT_SVC) {
            s_svc_start = svc->start_handle;
            s_svc_end = svc->end_handle;
            ESP_LOGI(TAG, "lamp svc FE95 %u..%u", s_svc_start, s_svc_end);
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (s_svc_start) {
            ble_gattc_disc_all_chrs(s_lamp, s_svc_start, s_svc_end, chr_cb, NULL);
        } else {
            ESP_LOGE(TAG, "lamp 没有 FE95 服务");
        }
    }
    return 0;
}

/* ── GAP 事件 ── */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!s_have_target) return 0;
        if (memcmp(ev->disc.addr.val, s_target.val, 6) == 0) {
            /* ★relay 模式下我们自己的 BT 地址已改成真灯的（见 app_main 的
             *   MAC 强制逻辑）。控制器一般会上报自己的广播，这里必须把
             *   「本机地址」过滤掉，否则会连到自己身上，链路永远建不起来。 */
            uint8_t own[6];
            if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, own, NULL) == 0 &&
                memcmp(own, ev->disc.addr.val, 6) == 0) {
                return 0;
            }
            ESP_LOGI(TAG, "发现真灯 %02X:%02X:%02X:%02X:%02X:%02X -> 连接",
                     s_target.val[5], s_target.val[4], s_target.val[3],
                     s_target.val[2], s_target.val[1], s_target.val[0]);
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &ev->disc.addr, 10000, NULL,
                                     gap_event, NULL);
            ESP_LOGI(TAG, "ble_gap_connect rc=%d", rc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_lamp = ev->connect.conn_handle;
            s_connecting = 0;
            s_svc_start = s_svc_end = 0;
            s_wr_busy = 0;
            /* ★不要清队列：手机在我们建链的这 1~2s 里已经发来的帧必须留着，
             *   等发现服务完成后再补发（见 subscribe_next 末尾的 flush）。 */
            s_ping_pending = 0;
            s_last_act = xTaskGetTickCount();
            s_lamp_since = xTaskGetTickCount();
            for (size_t i = 0; i < N_CHR; i++) {
                s_chr[i].val_handle = 0;
                s_chr[i].cccd_handle = 0;
            }
            ESP_LOGI(TAG, "真灯已连上 conn=%u，开始发现服务", s_lamp);
            /* 链路参数实验开关（2026-11-20）：
             *   0 = 不请求，用真灯自己的默认值（30~50ms），看 20s 断链是否消失
             *   1 = 请求 7.5~15ms（原来那版，转发快但真灯固定在 ~19.8s 断开）*/
#ifndef RELAY_FAST_PARAMS
#define RELAY_FAST_PARAMS 1
#endif
#if RELAY_FAST_PARAMS
            struct ble_gap_upd_params up = {
                .itvl_min = 6, .itvl_max = 12,
                .latency = 0, .supervision_timeout = 800,
                .min_ce_len = 0, .max_ce_len = 0,
            };
            int prc = ble_gap_update_params(s_lamp, &up);
            ESP_LOGI(TAG, "conn params update rc=%d", prc);
#else
            ESP_LOGI(TAG, "conn params 用真灯默认值（实验）");
#endif
            ble_gattc_disc_all_svcs(s_lamp, svc_cb, NULL);
        } else {
            s_connecting = 0;
            ESP_LOGW(TAG, "连接真灯失败 status=%d，1s 后重试", ev->connect.status);
            s_lamp = BLE_HS_CONN_HANDLE_NONE;
            if (s_enabled) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                connect_target();
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (ev->disconnect.conn.conn_handle == s_lamp) {
            ESP_LOGW(TAG, "真灯断开 reason=0x%x（已转 %lu 帧，排队 %d，写错 %lu）",
                     ev->disconnect.reason, (unsigned long)s_fwd_ok, s_q_cnt,
                     (unsigned long)s_wr_err);
            s_lamp = BLE_HS_CONN_HANDLE_NONE;
            s_wr_busy = 0;
            s_lamp_since = 0;
            for (size_t i = 0; i < N_CHR; i++) {
                s_chr[i].val_handle = 0;
                s_chr[i].cccd_handle = 0;
            }
            /* 队列保留：真灯侧尚未收到的帧在重连后继续补发。
             * ★无论手机在不在线都要立刻重连 —— 断了真灯就会重新广播，
             *   空中出现两台同地址设备，App 可能连错。 */
            if (s_enabled) connect_target();
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        /* 只看真灯这条链路：它若协商出大 MTU，真灯推的帧就可能 >20B，
         * 手机那条链路（MTU 小）会装不下，转发就被丢掉。 */
        if (ev->mtu.conn_handle == s_lamp)
            ESP_LOGW(TAG, "真灯链路 MTU=%u", ev->mtu.value);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (ev->notify_rx.conn_handle != s_lamp) return 0;
        uint16_t attr = ev->notify_rx.attr_handle;
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        uint8_t buf[512];
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &got) != 0) return 0;
        for (size_t i = 0; i < N_CHR; i++) {
            if (s_chr[i].val_handle == attr) {
                s_last_act = xTaskGetTickCount();
                /* 我们自己探活引出的 hello 回包：吞掉，别让手机收到 */
                if (s_ping_pending && s_chr[i].uuid16 == 0x001C &&
                    got == sizeof(HELLO_RSP) && memcmp(buf, HELLO_RSP, got) == 0) {
                    s_ping_pending = 0;
                    ESP_LOGI(TAG, "探活回包 hello（已吞，不转给手机）");
                    return 0;
                }
                log_hex("RX", s_chr[i].uuid16, buf, got);
                gatt_miot_relay_notify(s_chr[i].uuid16, buf, got);
                return 0;
            }
        }        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_lamp == BLE_HS_CONN_HANDLE_NONE) {
            /* 扫完了没找到，2 秒后再来 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            start_scan();
        }
        return 0;
    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (!s_have_target) return;
    struct ble_gap_disc_params p = { 0 };
    p.itvl = 0x60;
    p.window = 0x30;
    p.filter_duplicates = 0;   /* ★我们的地址和真灯相同，重复过滤会把真灯的广播一起吞掉 */
    p.passive = 0;
    int rc = ble_gap_disc(BLE_OWN_ADDR_RANDOM, BLE_HS_FOREVER, &p, gap_event, NULL);
    ESP_LOGI(TAG, "扫描真灯 rc=%d", rc);
}

/* 已知地址就直接连，不用再扫 —— 断开后重连只要 ~0.3s，比「扫描 0.3~3s」
 * 快一个数量级。手机连上时重置链路就靠它抢时间。 */
static void connect_target(void)
{
    if (!s_have_target || s_connecting) return;
    s_connecting = 1;
    int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &s_target, 5000, NULL,
                             gap_event, NULL);
    ESP_LOGI(TAG, "直连真灯 rc=%d", rc);
    if (rc) {
        s_connecting = 0;
        start_scan();
    }
}

/* ── 对外接口 ── */
static void lamp_write_next(void);

static int wr_done_cb(uint16_t conn, const struct ble_gatt_error *err,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status) {
        s_wr_err++;
        /* 0x0C = ATT 写超时；0x0E = 链路断了。前者说明真灯没回 ATT 响应 */
        ESP_LOGW(TAG, "lamp wr 完成 rc=%d (errs=%lu)", err->status, (unsigned long)s_wr_err);
    }
    s_wr_busy = 0;
    lamp_write_next();
    return 0;
}

static void lamp_write_next(void)
{
    if (s_wr_busy || s_q_cnt == 0) return;
    /* 真灯还没连上 / 特征还没发现：帧留在队列里等，绝不丢 */
    if (s_lamp == BLE_HS_CONN_HANDLE_NONE) return;
    q_item_t *it = &s_q[s_q_tail];
    relay_chr_t *c = find_chr(it->uuid16);
    if (!c) {                                   /* 不认识的 uuid：直接丢 */
        s_q_tail = (s_q_tail + 1) % Q_SLOTS;
        s_q_cnt--;
        lamp_write_next();
        return;
    }
    if (!c->val_handle) return;                 /* 特征还没发现完：等，别丢 */
    int rc = ble_gattc_write_flat(s_lamp, c->val_handle, it->d, it->len,
                                  wr_done_cb, NULL);
    if (rc) {
        if (rc == BLE_HS_EBUSY) return;      /* 保留在队首，等完成回调再试 */
        ESP_LOGW(TAG, "lamp wr 发起失败 rc=%d -> 丢 %04X", rc, it->uuid16);
        s_q_tail = (s_q_tail + 1) % Q_SLOTS;
        s_q_cnt--;
        lamp_write_next();
        return;
    }
    s_wr_busy = 1;
    s_q_tail = (s_q_tail + 1) % Q_SLOTS;
    s_q_cnt--;
}

int relay_forward(uint16_t uuid16, const uint8_t *data, size_t len)
{
    if (!s_enabled) return 0;
    relay_chr_t *c = find_chr(uuid16);
    if (!c) return 0;

    /* 手机上来的每一帧完整留痕（q= 队列深度，便于看拥塞） */
    ESP_LOGW(TAG, "-> lamp %04X %uB q=%d", uuid16, (unsigned)len, s_q_cnt);
    log_hex("TX", uuid16, data, len);

    if (s_lamp == BLE_HS_CONN_HANDLE_NONE || !c->val_handle) {
        if (s_lamp == BLE_HS_CONN_HANDLE_NONE) s_fwd_drop_nolamp++;
        /* ★不再丢弃：先入队，等真灯连上/发现完成后自动补发。
         *   之前这里直接 return 1，导致手机在建链那 1~2 秒里发的帧全丢，
         *   App 只能干等到 -7。 */
    }
    if (len > Q_MAXLEN) {
        ESP_LOGW(TAG, "  丢弃：帧太长 %u > %u", (unsigned)len, (unsigned)Q_MAXLEN);
        return 1;
    }
    if (s_q_cnt >= Q_SLOTS) {
        s_fwd_drop_qfull++;
        ESP_LOGW(TAG, "  丢弃：队列满（累计 %lu）", (unsigned long)s_fwd_drop_qfull);
        return 1;
    }
    q_item_t *it = &s_q[s_q_head];
    it->uuid16 = uuid16;
    it->len = (uint16_t)len;
    memcpy(it->d, data, len);
    s_q_head = (s_q_head + 1) % Q_SLOTS;
    s_q_cnt++;
    s_fwd_ok++;
    s_last_act = xTaskGetTickCount();
    lamp_write_next();
    return 1;
}

int relay_active(void) { return s_enabled; }

/* 手机（App）连上/断开时由 gatt_miot 通知。
 *
 * ★2026-11-20 实测关键结论：真灯对**单条连接**有约 19.1s 的硬上限（探活
 *   001C hello 每 3s 一次也照样到点就断，reason 0x213）。所以「一直挂着
 *   链路等 App 来用」是错的 —— App 连上时链路可能只剩 1s 寿命，配网必然
 *   传到一半断掉（-28）。
 *   正确做法：手机一连上，就把旧链路主动断掉、直连重建，让这 19s 窗口
 *   从 App 会话的起点开始计时。 */
void relay_on_phone(int connected)
{
    if (!s_enabled) return;
    if (!connected) {
        s_phone_on = 0;
        /* ★不要断开真灯：一旦断开，真灯立刻重新广播，空中又变成两台同地址
         *   设备，下一轮 App 就可能连到真灯本体。所以链路照常占着。 */
        ESP_LOGI(TAG, "手机断开（真灯链路继续占着，避免它重新广播）");
        return;
    }
    s_phone_on = 1;
    /* 新会话开始：清掉上一轮残留的帧，避免把旧 pkg 发给真灯 */
    s_q_head = s_q_tail = s_q_cnt = 0;
    s_wr_busy = 0;

    uint32_t age = s_lamp_since
                 ? (uint32_t)((xTaskGetTickCount() - s_lamp_since) * portTICK_PERIOD_MS)
                 : 0xFFFFFFFF;
    if (s_lamp == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "手机连上，真灯未连 -> 立刻建链（帧先入队）");
        connect_target();
    } else if (age > 2000) {
        /* ★真灯每条连接寿命固定 20s（实测，与链路参数无关）。手机连上时
         *   这条链路往往已经用掉十几秒，只剩几秒 → 必然断在传 pkg 的中途，
         *   那一帧丢了 App 就直接 -7。所以这里主动砍掉重建，把完整 20s
         *   让给配网流程（重建 + 重新订阅约 1.5s，App 那会儿还在做发现，
         *   帧会先入队，不会丢）。 */
        ESP_LOGW(TAG, "手机连上，真灯链路已 %lums（上限 20s）→ 砍掉重建",
                 (unsigned long)age);
        ble_gap_terminate(s_lamp, BLE_ERR_REM_USER_CONN_TERM);
        /* 断开事件里会立刻重连 */
    } else {
        ESP_LOGW(TAG, "手机连上，真灯链路刚建（%lums），直接用", (unsigned long)age);
    }
}

void relay_prepare(void)
{
    if (!RELAY_TARGET_MAC[0]) return;
    /* 必须在 adv_start 之前调用：广播开着时 HCI LE Set Random Address 会被
     * 控制器拒（rc=524 = HCI 0x0C Command Disallowed）。 */
    int rc = ble_hs_id_set_rnd(RND_OWN_ADDR);
    ESP_LOGI(TAG, "central 随机地址设置 rc=%d", rc);
}

void relay_start(const char *mac)
{
    unsigned v[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        ESP_LOGE(TAG, "MAC 解析失败: %s", mac);
        return;
    }
    for (int i = 0; i < 6; i++) s_target.val[i] = (uint8_t)v[5 - i];   /* 小端 */
    s_target.type = 0;
    s_have_target = 1;
    s_enabled = 1;
    ESP_LOGI(TAG, "relay 模式启动，目标 %s", mac);
    static int started;
    if (!started) {
        started = 1;
        xTaskCreate(keepalive_task, "relay_keep", 3072, NULL, 5, NULL);
    }
    /* ★开机就占住真灯（2026-11-20 二次修正）：
     *   真灯一旦被连上就**停止广播**。如果让它自己一直广播，空中会出现
     *   两台同地址设备（我们 careco654 + 真灯 xry.switch.sw4b01），App 连接
     *   时极可能连到真灯本体，中继就什么都收不到。
     *   代价是真灯链路 20s 会被它自己踢一次，我们靠「断开即重连(150~500ms)」
     *   续上；配网期间手机发来的帧全部入队，重连后按序补发。 */
    connect_target();
}
