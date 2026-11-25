/**
 * @file print_channel.c
 * @brief 米家标签打印机私有打印通道实现（A3 帧 / AES-128-CBC / TLV 命令）
 *
 * 2026-11-25：协议取自公开逆向工程 jasiek/xiaomi-mjbqdyj1-wc-reveng，
 * 经 ArtrixTech/mijia-label-printer 在真机上验证可打印。
 * 在此实现为「假打印机」设备端：收命令、应 ACK、发心跳、聚位图、串口预览。
 */
#include "print_channel.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
/* mbedTLS 4.x（IDF v6.0）：aes.h 搬进 private/，同 port 层一样做双路径探测 */
#if __has_include("mbedtls/private/aes.h")
#include "mbedtls/private/aes.h"
#else
#include "mbedtls/aes.h"
#endif
#include "gatt_miot.h"

static const char *TAG = "PRINT-CH";

/* ---------------- 协议常量（来自插件 CommandHelper，公开逆向） ---------------- */
static const uint8_t S_AES_KEY[16] = {
    0x99, 0xB8, 0x29, 0x43, 0x6C, 0xDD, 0x56, 0x47,
    0xAA, 0xDB, 0x88, 0x16, 0xF7, 0x3E, 0x86, 0x44,
};
static const uint8_t S_AES_IV[16] = {
    0x00, 0x01, 0x02, 0x0F, 0x3C, 0xF8, 0x99, 0xAB,
    0xAB, 0xCD, 0x25, 0x31, 0x8D, 0xF4, 0x46, 0xB1,
};
#define CRC_SEED 0x76953521u

/* ---------------- 状态 ---------------- */
#define RX_BUF_CAP   4096          /* 单帧最长：1800B 位图块 → 帧约 1832B */
#define RASTER_CAP   (16 * 1024)   /* 96 点宽 × 600 点长 = 7200B，留余量 */

static uint8_t  s_rx[RX_BUF_CAP];
static size_t   s_rx_len;

static uint8_t  s_raster[RASTER_CAP];
static size_t   s_raster_len;
static uint16_t s_chunk_idx, s_chunk_total;
static uint8_t  s_job_width, s_printing;
static uint32_t s_cmds;
static uint32_t s_crc_fail, s_aes_fail, s_overflow;

/* ── 打印状态机（artrix 实机时序：提交 → ACK → 状态 bit7=1 printing → 出纸 → bit7=0 空闲）──
 * flagsA: bit0 盖开 / bit1 缺纸 / bit2 卡纸 / bit3 过热 / bit7 正在打印
 * flagsB: bit0 刀具故障 / bit1 字库 / bit2 PSRAM / bit3 纸校准 / bit4-5 关机流程 / bit6 电池过热 */
#define FLAG_PRINTING   0x80
static uint8_t  s_flags_a, s_flags_b;
static uint32_t s_buffer_free = 16384;  /* 模拟打印队列空闲 RAM（插件流控阈值 >500） */
static uint32_t s_pages_done;
static esp_timer_handle_t s_finish_timer;
static volatile int s_finishing;        /* 出纸延时进行中 */

static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* CRC32：zlib 多项式表算法，初值 0x76953521（插件私有，错的会被设备静默丢弃） */
static uint32_t crc32_custom(const uint8_t *buf, size_t len)
{
    uint32_t crc = ~CRC_SEED;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

/* ---------------- AES-128-CBC（零填充） ----------------
 * 注意：插件每帧都用同一个固定 IV 加密，解密时同样喂固定 IV；
 * mbedtls 会原地修改 IV 缓冲，所以必须传副本，不能传常量区。 */
static int aes_crypt(const uint8_t *in, size_t len, uint8_t *out, int decrypt)
{
    if (len == 0 || len % 16) return -1;
    uint8_t iv[16];
    memcpy(iv, S_AES_IV, sizeof(iv));
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = decrypt ? mbedtls_aes_setkey_dec(&ctx, S_AES_KEY, 128)
                     : mbedtls_aes_setkey_enc(&ctx, S_AES_KEY, 128);
    if (rc == 0)
        rc = mbedtls_aes_crypt_cbc(&ctx, decrypt ? MBEDTLS_AES_DECRYPT : MBEDTLS_AES_ENCRYPT,
                                   len, iv, in, out);
    mbedtls_aes_free(&ctx);
    return rc;
}

/* ---------------- 发送（0x0020 notify） ---------------- */
/* 明文心跳/应答统一走 A3 00 明文帧（真机自发通知就是这种形式） */
static int send_plain(const uint8_t *plain, size_t len)
{
    static uint8_t frame[5 + 512 + 4];
    if (len > sizeof(frame) - 8) return -1;
    frame[0] = 0xA3;
    frame[1] = 0x00;                    /* 明文标记 */
    wr_le16(&frame[2], (uint16_t)len);
    memcpy(&frame[4], plain, len);
    wr_le32(&frame[4 + len], crc32_custom(&frame[4], len));
    return gatt_miot_notify_0020(frame, (uint16_t)(4 + len + 4));
}

/* 心跳：buffer_free 必须足够大，插件靠它做流控（>500 才继续发下一块）；
 * tag2 flags：bit7=1 正在打印（artrix 实机：提交后 App 等这个位翻转判断打印生命周期） */
static void send_heartbeat(void)
{
    uint8_t hb[23] = {
        0x10, 0x01, 0x1F, 0x12, 0x00,
        0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,   /* tag1: buffer LE32（下方填） */
        0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,   /* tag2: flagsA flagsB 0 0 */
        0x03, 0x01, 0x00, 0x20,                     /* tag3: 电量（原始值 32） */
    };
    wr_le32(&hb[8], (uint16_t)s_buffer_free);
    hb[14] = s_flags_a;
    hb[15] = s_flags_b;
    send_plain(hb, sizeof(hb));
}

/* 出纸延时到点：bit7 清零回空闲、buffer 复位，App 才认为这一页真正打完 */
static void finish_timer_cb(void *arg)
{
    s_finishing = 0;
    s_flags_a &= (uint8_t)~FLAG_PRINTING;
    s_buffer_free = 16384;
    ESP_LOGI(TAG, "=== 第 %u 页出纸完成，状态回空闲（bit7=0） ===", s_pages_done);
    send_heartbeat();
}

static void start_finish_timer(void)
{
    /* 出纸时长按位图行数估算：真机约 6 行/100ms，clamp 2~8 秒 */
    uint32_t rows = (uint32_t)(s_raster_len / 12u);
    uint32_t ms = rows * 100u / 60u;
    if (ms < 2000) ms = 2000;
    if (ms > 8000) ms = 8000;
    s_finishing = 1;
    const esp_timer_create_args_t args = {
        .callback = finish_timer_cb, .name = "print_fin",
    };
    if (s_finish_timer) {
        esp_timer_stop(s_finish_timer);
        esp_timer_start_once(s_finish_timer, (uint64_t)ms * 1000u);
    } else {
        esp_timer_create(&args, &s_finish_timer);
        esp_timer_start_once(s_finish_timer, (uint64_t)ms * 1000u);
    }
    ESP_LOGI(TAG, ">>> 状态 printing=1 已上报，%u ms 后回空闲 <<<", (unsigned)ms);
}

/* 应答帧：21 <grp> <cmd> <len LE16> <body> */
static void send_ack(uint8_t grp, uint8_t cmd)
{
    uint8_t plain[5] = { 0x21, grp, cmd, 0x00, 0x00 };
    send_plain(plain, sizeof(plain));
}

/* 状态查询应答（01/13、01/12）：TLV{tag1: 4B flags, tag2: 1B} */
static void send_status_reply(uint8_t cmd)
{
    const uint8_t plain[] = {
        0x21, 0x01, cmd, 0x0B, 0x00,
        0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,   /* flags 全 0：盖合/有纸/不热/未打印 */
        0x02, 0x01, 0x00, 0x00,
    };
    send_plain(plain, sizeof(plain));
}

/* ---------------- 串口预览：把位图打成 ASCII（压到 48 列，取前 N 行） ---------------- */
static void dump_raster_preview(size_t rows)
{
    ESP_LOGI(TAG, "=== 位图预览（%u 点宽 × %u 点高，共 %uB） ===",
             s_job_width, (unsigned)s_raster_len / 12u, (unsigned)s_raster_len);
    size_t show = rows < s_raster_len / 12u ? rows : s_raster_len / 12u;
    for (size_t y = 0; y < show; y++) {
        char line[49];
        for (size_t x = 0; x < 48; x++) {
            uint8_t byte = s_raster[y * 12 + (x / 8)];
            line[x] = (byte & (0x80 >> (x % 8))) ? '#' : '.';
        }
        line[48] = '\0';
        ESP_LOGI(TAG, "|%s|", line);
    }
}

/* ---------------- 命令分发 ---------------- */
static void handle_command(const uint8_t *plain, size_t plain_len)
{
    if (plain_len < 5 || plain[0] != 0x11) {
        ESP_LOGW(TAG, "未知内层帧 marker=%02X（len=%u）", plain_len ? plain[0] : 0, (unsigned)plain_len);
        return;
    }
    uint8_t grp = plain[1], cmd = plain[2];
    uint16_t blen = rd_le16(&plain[3]);
    const uint8_t *body = &plain[5];
    s_cmds++;

    ESP_LOGI(TAG, "cmd %02X/%02X body=%uB", grp, cmd, blen);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, body, blen > 48 ? 48 : blen, ESP_LOG_INFO);

    switch (grp) {
    case 0x01:                              /* 控制与状态 */
        switch (cmd) {
        case 0x1E:                          /* 标记连接 */
            ESP_LOGI(TAG, ">>> App 已连接打印通道 <<<");
            send_ack(grp, cmd);
            send_heartbeat();
            break;
        case 0x13:                          /* 查询状态 */
        case 0x12:
            send_status_reply(cmd);
            send_heartbeat();
            break;
        case 0x1A:                          /* 电量（LE16 原始值，真机 8000） */
        case 0x0E: {
            const uint8_t r[] = { 0x21, 0x01, cmd, 0x02, 0x00, 0x40, 0x1F };
            send_plain(r, sizeof(r));
            break;
        }
        case 0x19:                          /* 校时（+8h 北京时间） */
            if (blen >= 4) ESP_LOGI(TAG, "校时 epoch=%u", (unsigned)rd_le32(body));
            send_ack(grp, cmd);
            break;
        default:
            ESP_LOGW(TAG, "grp01 cmd=%02X 未实现，仅 ACK", cmd);
            send_ack(grp, cmd);
            break;
        }
        break;

    case 0x05:                              /* 打印作业 */
        switch (cmd) {
        case 0x0B: {                        /* 开始打印：60 00 <高度 LE16> 01 00 00 */
            if (blen < 6) break;
            uint16_t w = rd_le16(&body[0]);
            uint16_t h = rd_le16(&body[2]);
            s_job_width = (uint8_t)(w / 8); /* 96 点 = 12B/行 */
            s_raster_len = 0;
            s_printing = 1;
            s_chunk_idx = s_chunk_total = 0;
            s_buffer_free = 16384;
            s_flags_a |= FLAG_PRINTING;     /* ★bit7=1：进入「正在打印」 */
            ESP_LOGI(TAG, ">>> 打印开始：第 %u 页，宽 %u 点（%u B/行）长 %u 点 <<<",
                     s_pages_done + 1, w, s_job_width, h);
            send_ack(grp, cmd);
            send_heartbeat();
            break;
        }
        case 0x0D: {                        /* 位图块：<idx LE16> <total LE16> 10 0C 00 00 00 00 00 <raster> */
            if (blen < 11) break;
            uint16_t idx = rd_le16(&body[0]);
            uint16_t total = rd_le16(&body[2]);
            const uint8_t *raster = &body[11];
            size_t rlen = blen - 11;
            s_chunk_idx = idx;
            s_chunk_total = total;
            if (s_raster_len + rlen > RASTER_CAP) {
                s_overflow++;
                ESP_LOGE(TAG, "位图缓冲溢出（已有 %u + %u > %d）",
                         (unsigned)s_raster_len, (unsigned)rlen, RASTER_CAP);
                break;
            }
            memcpy(&s_raster[s_raster_len], raster, rlen);
            s_raster_len += rlen;
            ESP_LOGI(TAG, "位图块 %u/%u：%uB（累计 %uB）",
                     idx, total, (unsigned)rlen, (unsigned)s_raster_len);
            /* 真机不逐块 ACK；心跳里 buffer 模拟队列占用（出纸消化），保持 >500 流控即可 */
            s_buffer_free = (s_buffer_free > rlen + 8192u) ? s_buffer_free - rlen : 12288u;
            send_heartbeat();
            break;
        }
        case 0x0C: {                        /* 提交：<…> II（0=最后一页 1=还有下一页） */
            uint8_t more = blen >= 9 ? body[8] : 0;
            s_pages_done++;
            ESP_LOGI(TAG, "*** 第 %u 页提交完毕（more_pages=%u），位图 %uB / %u 行 ***",
                     s_pages_done, more, (unsigned)s_raster_len, (unsigned)(s_raster_len / 12u));
            dump_raster_preview(32);
            send_ack(grp, cmd);             /* 真机实测：21 05 0C 00 00（artrix 称「提交已应答」） */
            if (more) {
                /* 多页：保持 printing，位图等下一个 05/0B 重置 */
                ESP_LOGI(TAG, ">>> more_pages=1：保持打印中，等待下一页 <<<");
                send_heartbeat();
                s_printing = 1;
            } else {
                /* 单页/末页：保持 bit7=1 一小段（模拟出纸），到点回空闲 */
                send_heartbeat();
                start_finish_timer();
                s_printing = 0;
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "grp05 cmd=%02X 未实现，仅 ACK", cmd);
            send_ack(grp, cmd);
            break;
        }
        break;

    case 0x02:                              /* RAM 下载（资源/模板） */
        switch (cmd) {
        case 0x01:
            ESP_LOGI(TAG, "下载开始（总长 %uB）", blen >= 13 ? (unsigned)rd_le32(&body[7]) : 0);
            s_raster_len = 0;
            send_ack(grp, cmd);
            break;
        case 0x03: {
            if (blen < 10) break;
            size_t clen = rd_le16(&body[2]);
            if (s_raster_len + clen > RASTER_CAP) { s_overflow++; break; }
            memcpy(&s_raster[s_raster_len], &body[10], clen);
            s_raster_len += clen;
            ESP_LOGI(TAG, "下载块 idx=%u：%uB（累计 %uB）",
                     rd_le16(&body[0]), (unsigned)clen, (unsigned)s_raster_len);
            send_heartbeat();
            break;
        }
        case 0x02:
            ESP_LOGI(TAG, "*** 下载完成：%uB ***", (unsigned)s_raster_len);
            send_ack(grp, cmd);
            break;
        default:
            send_ack(grp, cmd);
            break;
        }
        break;

    default:
        ESP_LOGW(TAG, "grp=%02X 未实现，仅 ACK", grp);
        send_ack(grp, cmd);
        break;
    }
}

/* ---------------- 组帧（0x001F 的 BLE 写切片 → 完整 A3 帧） ---------------- */
void print_channel_on_rx(const uint8_t *data, size_t len)
{
    if (!data || !len) return;

    if (s_rx_len + len > RX_BUF_CAP) {
        s_overflow++;
        ESP_LOGE(TAG, "RX 缓冲溢出，重置");
        s_rx_len = 0;
        return;
    }
    memcpy(&s_rx[s_rx_len], data, len);
    s_rx_len += len;

    while (s_rx_len >= 8) {
        /* 同步到帧头 */
        if (s_rx[0] != 0xA3 || (s_rx[1] != 0x20 && s_rx[1] != 0x00)) {
            memmove(s_rx, &s_rx[1], --s_rx_len);
            continue;
        }
        uint16_t blen = rd_le16(&s_rx[2]);
        size_t total = 4 + blen + 4;
        if (s_rx_len < total) break;        /* 还没收完，等下一片 */

        const uint8_t *body = &s_rx[4];
        uint32_t crc_got = rd_le32(&s_rx[4 + blen]);
        uint32_t crc_exp = crc32_custom(body, blen);
        if (crc_got != crc_exp) {
            s_crc_fail++;
            ESP_LOGW(TAG, "CRC 不匹配 got=%08X exp=%08X（密钥/初值不对就会被真机静默丢）",
                     crc_got, crc_exp);
        } else if (s_rx[1] == 0x20) {
            /* 加密命令：AES-CBC 解密 */
            static uint8_t plain[RX_BUF_CAP];
            if (blen % 16 || aes_crypt(body, blen, plain, 1) != 0) {
                s_aes_fail++;
                ESP_LOGE(TAG, "AES 解密失败（blen=%u）", blen);
            } else {
                /* 内层：11 grp cmd len LE16 body……按内层长度去零填充 */
                if (plain[0] == 0x11 && blen >= 5) {
                    uint16_t ilen = rd_le16(&plain[3]);
                    size_t want = 5 + ilen;
                    if (want <= blen) handle_command(plain, want);
                    else handle_command(plain, blen);
                } else {
                    handle_command(plain, blen);
                }
            }
        } else {
            /* 主机发明文帧？照常解析 */
            handle_command(body, blen);
        }
        memmove(s_rx, &s_rx[total], s_rx_len - total);
        s_rx_len -= total;
    }
    /* 尾部垃圾（非 A3 开头）直接丢弃 */
    while (s_rx_len && (s_rx[0] != 0xA3)) {
        memmove(s_rx, &s_rx[1], --s_rx_len);
    }
}

void print_channel_reset(void)
{
    s_rx_len = 0;
    s_raster_len = 0;
    s_printing = 0;
    s_flags_a = s_flags_b = 0;
    s_buffer_free = 16384;
    s_pages_done = 0;
    s_finishing = 0;
    if (s_finish_timer) esp_timer_stop(s_finish_timer);
}

uint32_t print_channel_cmd_count(void) { return s_cmds; }
