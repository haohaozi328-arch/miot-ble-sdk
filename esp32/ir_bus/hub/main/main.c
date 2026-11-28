/* ir_bus HUB（"1"）—— 红外接收 + BLE GATT 下发
 *
 * 职责：只干一件事，把「听到的红外码」变成「总线上的指令」。
 *   VS1838B(OUT) → GPIO3 → RMT → SIRC/NEC 解码 → 去抖 → BLE NOTIFY
 *
 * 为什么 Hub 是 peripheral：手头只有一块板，需要一个不用第二块板的对端。
 * PC 用 bleak 订阅 NOTIFY、手机用 nRF Connect 订阅，都能当这个对端。
 *
 * 验证方式（单板即可）：
 *   1. 烧这块板 → 串口打印 "IR-HUB ready"
 *   2. PC: python tools/ble_peer.py listen --name IR-HUB
 *   3. 让小爱音箱/手机发红外 → PC 上看到 "RX cmd=0x15 POWER"
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "../../common/ble_bus.h"

#define IR_GPIO        GPIO_NUM_3
#define RMT_RES_HZ     1000000
#define MEM_BLOCK      48
#define BUF_SYMBOLS    96
#define MAX_PULSES     400
#define REP_GAP_US     600000     /* 超时重置重复计数 */
#define TRIG_DEBOUNCE_US 1200000  /* 同一键 1.2s 只上报一次（吃掉 8 帧连发） */

static const char *DEV_NAME = "IR-HUB";

/* ---------------- 红外接收（RMT） ---------------- */

static QueueHandle_t s_q;
static rmt_symbol_word_t s_dma[BUF_SYMBOLS];
static rmt_symbol_word_t s_copy[BUF_SYMBOLS];
static volatile size_t s_num;

static IRAM_ATTR bool rx_done_cb(rmt_channel_handle_t ch,
                                 const rmt_rx_done_event_data_t *ev, void *ctx)
{
    size_t n = ev->num_symbols;
    if (n > BUF_SYMBOLS) n = BUF_SYMBOLS;
    memcpy(s_copy, ev->received_symbols, n * sizeof(rmt_symbol_word_t));
    s_num = n;
    BaseType_t hp = pdFALSE;
    uint32_t token = 0;
    xQueueSendFromISR(s_q, &token, &hp);
    return hp == pdTRUE;
}

typedef struct { uint16_t dur; uint8_t level; } pulse_t;   /* level 0=载波 */

static size_t flatten(pulse_t *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < s_num && k + 1 < cap; i++) {
        rmt_symbol_word_t s = s_copy[i];
        if (s.duration0) { out[k].level = s.level0; out[k].dur = s.duration0; k++; }
        if (s.duration1) { out[k].level = s.level1; out[k].dur = s.duration1; k++; }
    }
    return k;
}

/* 索尼 SIRC：帧头 2.4ms/0.6ms，位宽 mark 0.6ms=0 / 1.2ms=1，LSB first */
typedef struct { int nbits; uint8_t cmd; uint16_t dev; bool ok; } sirc_t;

static sirc_t sirc_decode(const pulse_t *p, size_t n)
{
    sirc_t r = { 0, 0, 0, false };
    size_t i = 0;
    while (i < n && p[i].level != 0) i++;
    if (i + 1 >= n) return r;
    if (p[i].dur < 2000 || p[i].dur > 2900) return r;
    if (p[i + 1].dur < 350 || p[i + 1].dur > 950) return r;
    i += 2;

    uint32_t bits = 0;
    int nb = 0;
    while (i < n && nb < 20) {
        if (p[i].level != 0) break;
        uint16_t m = p[i].dur;
        int v;
        if (m >= 350 && m <= 800) v = 0;
        else if (m >= 800 && m <= 1650) v = 1;
        else break;
        bits |= (uint32_t)v << nb;
        nb++;
        i++;
        if (i < n && p[i].level == 1) {
            if (p[i].dur < 300 || p[i].dur > 1000) break;
            i++;
        }
    }
    if (nb != 12 && nb != 15 && nb != 20) return r;

    r.nbits = nb;
    r.cmd = bits & 0x7F;
    r.dev = (uint16_t)((bits >> 7) & (nb == 12 ? 0x1F : 0xFF));
    r.ok = true;
    return r;
}

/* NEC（含 16bit 扩展地址）：帧头 9ms/4.5ms */
typedef struct { uint32_t code; bool ok; bool ext; bool bad; } nec_t;

static nec_t nec_decode(const pulse_t *p, size_t n)
{
    nec_t r = { 0, false, false, false };
    size_t i = 0;
    while (i < n && p[i].level != 0) i++;
    if (i + 1 >= n) return r;
    if (p[i].dur < 8000 || p[i].dur > 10000) return r;
    if (p[i + 1].dur < 4000 || p[i + 1].dur > 5000) return r;
    i += 2;

    uint32_t v = 0;
    for (int b = 0; b < 32; b++) {
        if (i + 1 >= n) return r;
        if (p[i].dur < 300 || p[i].dur > 900) return r;
        v >>= 1;
        if (p[i + 1].dur > 1000) v |= 0x80000000u;
        i += 2;
    }
    r.code = v;
    r.ok = true;

    uint8_t b0 = v & 0xFF, b1 = (v >> 8) & 0xFF;
    uint8_t b2 = (v >> 16) & 0xFF, b3 = (v >> 24) & 0xFF;
    r.bad = (b3 != (uint8_t)~b2);
    r.ext = (b1 != (uint8_t)~b0);
    return r;
}

/* ---------------- BLE 总线（GATT peripheral） ---------------- */

static ble_uuid128_t s_svc_uuid   = BUS_SVC_UUID_INIT;
static ble_uuid128_t s_cmd_uuid   = BUS_CMD_UUID_INIT;
static ble_uuid128_t s_state_uuid = BUS_STATE_UUID_INIT;

static uint16_t s_cmd_val_handle;
static uint16_t s_state_val_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static uint8_t  s_seq;
static bool     s_subscribed;

static int bus_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;

    if (attr_handle == s_cmd_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[16];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        printf("[hub] 中央写入 CMD %u 字节（Hub 一般不用，供手工测试）\n", len);
        return 0;
    }

    if (attr_handle == s_state_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = 1;                       /* Hub 状态：1 = 在跑 */
        return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_cmd_uuid.u,
        .access_cb = bus_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_cmd_val_handle,
    },
    {
        .uuid = &s_state_uuid.u,
        .access_cb = bus_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_state_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

static void adv_start(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            printf("[hub] 中央已连接 (conn=%d)\n", s_conn);
        } else {
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        printf("[hub] 中央断开 (reason=%d)\n", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_cmd_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            printf("[hub] 对端%s了指令通知\n", s_subscribed ? "订阅" : "取消订阅");
        }
        return 0;

    default:
        return 0;
    }
}

static void adv_start(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) printf("[hub] adv fields rc=%d\n", rc);

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) printf("[hub] adv start rc=%d\n", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    adv_start();
    printf("[hub] BLE 就绪，广播名 %s\n", DEV_NAME);
}

static void on_reset(int reason)
{
    printf("[hub] BLE reset, reason=%d\n", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void bus_publish(uint8_t cmd, uint8_t arg)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        printf("[hub] 无对端订阅，指令丢弃 cmd=0x%02X\n", cmd);
        return;
    }
    bus_msg_t m = { .cmd = cmd, .arg = arg, .seq = ++s_seq };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&m, sizeof(m));
    if (!om) { printf("[hub] mbuf 分配失败\n"); return; }
    int rc = ble_gatts_notify_custom(s_conn, s_cmd_val_handle, om);
    if (rc) printf("[hub] notify 失败 rc=%d\n", rc);
}

/* ---------------- 主流程 ---------------- */

static int64_t s_trig_us;
static uint8_t s_trig_cmd;

static void on_key(uint8_t cmd)
{
    int64_t now = esp_timer_get_time();
    if (cmd == s_trig_cmd && now - s_trig_us < TRIG_DEBOUNCE_US) return;
    s_trig_cmd = cmd;
    s_trig_us = now;

    printf(">>> IR cmd=0x%02X\n", cmd);
    bus_publish(cmd, 0);
}

void app_main(void)
{
    printf("\n=== IR-HUB ready: VS1838B OUT -> GPIO%d ===\n", (int)IR_GPIO);

    s_q = xQueueCreate(4, sizeof(uint32_t));

    rmt_channel_handle_t ch = NULL;
    rmt_rx_channel_config_t cfg = {
        .gpio_num = IR_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = MEM_BLOCK,
        .flags.invert_in = false,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&cfg, &ch));

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ch, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(ch));

    rmt_receive_config_t rc = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 12000000,
    };
    ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));

    /* BLE */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { printf("[hub] nimble init 失败: %d\n", err); return; }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEV_NAME);

    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);

    nimble_port_freertos_init(host_task);

    static pulse_t pulses[MAX_PULSES];
    uint32_t last_id = 0;
    int64_t last_us = 0;

    while (1) {
        uint32_t token;
        if (xQueueReceive(s_q, &token, portMAX_DELAY) != pdTRUE) continue;

        int64_t now = esp_timer_get_time();
        size_t n = flatten(pulses, MAX_PULSES);
        if (n < 4) {
            ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));
            continue;
        }

        sirc_t s = sirc_decode(pulses, n);
        nec_t  k = nec_decode(pulses, n);

        /* 指纹用解码结果，不用原始时序（抖动会让同一帧算成不同帧） */
        uint32_t id;
        if (s.ok) {
            id = 0x50000000u ^ ((uint32_t)s.nbits << 16) ^ ((uint32_t)s.dev << 8) ^ s.cmd;
        } else if (k.ok) {
            id = 0xA0000000u ^ k.code;
        } else {
            id = 0;                                  /* 认不出就不去重 */
        }

        bool same = (id != 0) && (id == last_id) && (now - last_us < REP_GAP_US);
        last_id = id;
        last_us = now;

        if (!same) {
            if (s.ok) {
                printf("NEW SIRC%d dev=0x%02X cmd=0x%02X\n", s.nbits, s.dev, s.cmd);
                on_key(s.cmd);
            } else if (k.ok) {
                printf("NEW NEC%s addr=0x%04X cmd=0x%02X%s\n",
                       k.ext ? "-EXT" : "", (unsigned)(k.code & 0xFFFF),
                       (unsigned)((k.code >> 16) & 0xFF), k.bad ? " (校验失败)" : "");
                /* NEC 的 cmd 也在低 8 位之外，这里只转发 cmd，够用 */
                on_key((uint8_t)((k.code >> 16) & 0xFF));
            } else {
                printf("NEW 未知协议 n=%u（未转发）\n", (unsigned)n);
            }
        }

        ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));
    }
}
