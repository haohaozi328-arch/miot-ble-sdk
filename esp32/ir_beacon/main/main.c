/* 红外接收测试：VS1838B(OUT) -> ESP32-C3 GPIO3
 *
 * VS1838B 自带 38kHz 带通与解调，输出基带数字信号（有载波=低电平）。
 * 本程序用 RMT 捕获时序：
 *   - NEC 码（含 16bit 地址的扩展 NEC）→ 直接打印 addr/cmd
 *   - 其它协议（空调长帧、匹配向导的测试码等）→ 打印完整时序 + 指纹
 *   - 同一帧连续重复（按住连发 / 向导自动循环）→ 折叠成 REP，不再刷屏
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

#define IR_GPIO        GPIO_NUM_3
#define ACT_GPIO       GPIO_NUM_4       /* 执行输出：继电器 / 指示灯 / 触发脚 */
#define ACT_DEV        0x01             /* 索尼电视设备号 */
#define TRIG_DEBOUNCE_US  1200000       /* 同一键 1.2s 内只触发一次（连发帧降噪） */
#define RMT_RES_HZ     1000000          /* 1 tick = 1us */
#define MEM_BLOCK      48               /* ESP32-C3 每通道 48 symbols */
#define BUF_SYMBOLS    96               /* 非 DMA 接收上限 = mem_block*2 */
#define MAX_PULSES     400
#define REP_GAP_US     600000           /* 超过 600ms 无帧则重置重复计数 */

static QueueHandle_t s_q;
static rmt_symbol_word_t s_dma[BUF_SYMBOLS];   /* 驱动写入 */
static rmt_symbol_word_t s_copy[BUF_SYMBOLS];  /* 回调拷出 */
static volatile size_t s_num;

static IRAM_ATTR bool rx_done_cb(rmt_channel_handle_t ch,
                                 const rmt_rx_done_event_data_t *ev,
                                 void *ctx)
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

typedef struct {
    uint16_t dur;   /* us */
    uint8_t  level; /* 0=载波(mark) 1=空闲(space) */
} pulse_t;

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

typedef enum { NEC_NONE = 0, NEC_STD, NEC_EXT, NEC_BAD } nec_kind_t;

/* NEC: 9ms mark + 4.5ms space；32bit LSB first；bit0=560+560us，bit1=560+1690us */
static nec_kind_t nec_decode(const pulse_t *p, size_t n, uint32_t *code)
{
    size_t i = 0;
    while (i < n && p[i].level != 0) i++;
    if (i + 1 >= n) return NEC_NONE;
    if (p[i].dur < 8000 || p[i].dur > 10000) return NEC_NONE;
    if (p[i + 1].dur < 4000 || p[i + 1].dur > 5000) return NEC_NONE;
    i += 2;

    uint32_t v = 0;
    for (int b = 0; b < 32; b++) {
        if (i + 1 >= n) return NEC_NONE;
        if (p[i].dur < 300 || p[i].dur > 900) return NEC_NONE;   /* mark 560us */
        uint16_t sp = p[i + 1].dur;
        v >>= 1;
        if (sp > 1000) v |= 0x80000000u;                        /* space 1690us = 1 */
        i += 2;
    }
    *code = v;

    uint8_t b0 = v & 0xFF, b1 = (v >> 8) & 0xFF;
    uint8_t b2 = (v >> 16) & 0xFF, b3 = (v >> 24) & 0xFF;
    if (b3 != (uint8_t)~b2) return NEC_BAD;        /* cmd 取反位不匹配 */
    if (b1 == (uint8_t)~b0) return NEC_STD;        /* 8bit 地址 */
    return NEC_EXT;                                /* 16bit 地址 */
}

/* 索尼 SIRC：帧头 2.4ms mark + 0.6ms space；位=mark 宽度（0.6ms=0, 1.2ms=1），
 * 位间 space 固定 0.6ms；LSB first，前 7 位是命令，后面是设备号。
 * 12bit=25 段, 15bit=31 段, 20bit=41 段。 */
typedef struct { int nbits; uint8_t cmd; uint16_t dev; bool ok; } sirc_t;

static sirc_t sirc_decode(const pulse_t *p, size_t n)
{
    sirc_t r = { 0, 0, 0, false };
    size_t i = 0;
    while (i < n && p[i].level != 0) i++;
    if (i + 1 >= n) return r;
    if (p[i].dur < 2000 || p[i].dur > 2900) return r;      /* 2.4ms */
    if (p[i + 1].dur < 350 || p[i + 1].dur > 950) return r; /* 0.6ms */
    i += 2;

    uint32_t bits = 0;
    int nb = 0;
    while (i < n && nb < 20) {
        if (p[i].level != 0) break;                        /* 期望 mark */
        uint16_t m = p[i].dur;
        int v;
        if (m >= 350 && m <= 800) v = 0;                   /* 0.6ms 短 */
        else if (m >= 800 && m <= 1650) v = 1;             /* 1.2ms 长 */
        else break;
        bits |= (uint32_t)v << nb;
        nb++;
        i++;
        if (i < n && p[i].level == 1) {                    /* 位后 space */
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

/* 50us 分桶后算 FNV-1a 指纹，用来识别「同一帧重复」 */
static uint32_t frame_id(const pulse_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        uint32_t b = (p[i].dur + 25) / 50;
        h ^= (b & 0xFF) | ((b & 0xFF00) << 8) | ((uint32_t)p[i].level << 24);
        h *= 16777619u;
    }
    h ^= (uint32_t)n * 2654435761u;
    return h;
}

static void dump_all(const pulse_t *p, size_t n)
{
    printf("  DUR n=%u (us, 从 mark 开始交替):\n  ", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        printf("%u ", (unsigned)p[i].dur);
        if ((i + 1) % 12 == 0 && i + 1 < n) printf("\n  ");
    }
    printf("\n");
}

/* ---- 动作层：认固定码表，触发一次就干一次活 ---- */

static const char *key_name(uint8_t cmd)
{
    switch (cmd) {
    case 0x15: return "POWER";
    case 0x12: return "VOL+";
    case 0x13: return "VOL-";
    default:   return NULL;
    }
}

static int64_t s_trig_us;
static uint8_t s_trig_cmd;

static void on_key(uint8_t cmd, int64_t now)
{
    const char *name = key_name(cmd);
    if (!name) return;
    if (cmd == s_trig_cmd && now - s_trig_us < TRIG_DEBOUNCE_US) return;  /* 连发降噪 */
    s_trig_cmd = cmd;
    s_trig_us = now;

    if (cmd == 0x15) {                                  /* 电源 = 翻转执行输出 */
        int lvl = gpio_get_level(ACT_GPIO) ? 0 : 1;
        gpio_set_level(ACT_GPIO, lvl);
        printf(">>> TRIGGER %s  -> ACT_GPIO(GPIO%d)=%s\n",
               name, (int)ACT_GPIO, lvl ? "ON" : "OFF");
    } else {
        printf(">>> TRIGGER %s\n", name);
    }
}

void app_main(void)
{
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
        .signal_range_min_ns = 2000,      /* 滤掉 <2us 毛刺 */
        .signal_range_max_ns = 12000000,  /* 12ms 空闲判定帧结束 */
    };
    ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));

    printf("\n=== ir_beacon ready: VS1838B OUT -> GPIO%d ===\n", (int)IR_GPIO);
    printf("执行输出: GPIO%d（电源键翻转）\n", (int)ACT_GPIO);
    printf("码表: dev=0x%02X  POWER=0x15  VOL+=0x12  VOL-=0x13\n\n", ACT_DEV);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ACT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(ACT_GPIO, 0);

    static pulse_t pulses[MAX_PULSES];
    uint32_t last_id = 0;
    uint32_t rep = 0;
    int64_t last_us = 0;

    while (1) {
        uint32_t token;
        if (xQueueReceive(s_q, &token, portMAX_DELAY) != pdTRUE) continue;

        int64_t now = esp_timer_get_time();
        size_t n = flatten(pulses, MAX_PULSES);
        if (n < 4) {                        /* 毛刺，丢掉 */
            ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));
            continue;
        }

        uint32_t code = 0;
        nec_kind_t k = nec_decode(pulses, n, &code);
        sirc_t s = sirc_decode(pulses, n);

        /* 指纹优先后解码结果：时序抖动 ±60us 不影响 dev/cmd，
         * 用原始微秒算指纹会把同一帧算成不同帧，去重就失效。 */
        uint32_t id;
        if (s.ok) {
            id = 0x50000000u ^ ((uint32_t)s.nbits << 16) ^ ((uint32_t)s.dev << 8) ^ s.cmd;
        } else if (k != NEC_NONE) {
            id = 0xA0000000u ^ code;
        } else {
            id = frame_id(pulses, n);
        }
        bool same = (id == last_id) && (now - last_us < REP_GAP_US);
        last_id = id;
        last_us = now;

        if (s.ok && s.dev == ACT_DEV) on_key(s.cmd, now);

        if (same) {
            rep++;
            printf("REP id=%08X x%u\n", (unsigned)id, (unsigned)rep);
        } else {
            rep = 1;
            printf("NEW id=%08X n=%u", (unsigned)id, (unsigned)n);
            bool known = true;
            if (s.ok) {
                printf("  SIRC%d dev=0x%02X cmd=0x%02X",
                       s.nbits, (unsigned)s.dev, (unsigned)s.cmd);
            } else if (k == NEC_STD) {
                printf("  NEC addr=0x%02X cmd=0x%02X",
                       (unsigned)(code & 0xFF), (unsigned)((code >> 16) & 0xFF));
            } else if (k == NEC_EXT) {
                printf("  NEC-EXT addr=0x%04X cmd=0x%02X",
                       (unsigned)(code & 0xFFFF), (unsigned)((code >> 16) & 0xFF));
            } else if (k == NEC_BAD) {
                printf("  NEC? cmd/~cmd 不匹配 code=0x%08X", (unsigned)code);
            } else {
                /* 未知协议：打印帧头 + 完整时序，方便下机解码 */
                known = false;
                size_t i = 0;
                while (i < n && pulses[i].level != 0) i++;
                if (i + 1 < n)
                    printf("  leader=%u/%uus", (unsigned)pulses[i].dur,
                           (unsigned)pulses[i + 1].dur);
            }
            printf("\n");
            if (!known) dump_all(pulses, n);
        }
        fflush(stdout);

        ESP_ERROR_CHECK(rmt_receive(ch, s_dma, sizeof(s_dma), &rc));
    }
}
