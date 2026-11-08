/* Host 侧帧回归测试：不接 BLE、不接 App，直接把 repeller_report 组出来的
 * 明文帧打印出来，用来核对 siid/piid/类型/值是否与 zimi.mosq.v1 一致。
 * 用法（容器内）：gcc -I include src/*.c test/host_repeller_frame.c -o /tmp/t && /tmp/t
 */
#include "miot_ble/miot_profiles.h"
#include "miot_ble/miot_crypto.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static uint8_t g_buf[512];
static size_t  g_n;
static int     g_started;

static void my_log(void *u, int lv, const char *tag, const char *fmt, ...)
{
    (void)u; (void)lv; (void)tag;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}

/* ag2 分片重组：6B start 帧清零，其余数据片去掉 2B 片号追加 */
static int cap_send(void *u, const uint8_t *d, size_t n)
{
    (void)u;
    if (n == 6 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0) {
        g_n = 0; g_started = 1; return 0;
    }
    if (n > 2 && g_started) {
        memcpy(g_buf + g_n, d + 2, n - 2); g_n += n - 2;
    }
    return 0;
}

/* 直通加密：只关心明文结构，密文原样拷贝并填固定 tag */
static int fake_ccm(void *ctx, int enc, const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4])
{
    (void)ctx; (void)enc; (void)key; (void)nonce; (void)aad; (void)aad_len;
    memcpy(out, in, len);
    tag[0] = 0xA1; tag[1] = 0xA2; tag[2] = 0xA3; tag[3] = 0xA4;
    return 0;
}

int main(void)
{
    miot_crypto_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.aes_ccm = fake_ccm;
    miot_crypto_set_ops(&ops);

    miot_port_t port;
    memset(&port, 0, sizeof(port));
    port.send_notify = cap_send;
    port.log = my_log;

    miot_device_t dev;
    repeller_init(&dev, &port, NULL, NULL);
    uint8_t keys[MIOT_KEYS_LEN];
    memset(keys, 0x11, sizeof(keys));
    miot_device_set_session_keys(&dev, keys);

    printf("== repeller_report(power=1, batt=65, margin=71) ==\n");
    repeller_report(&dev, 1, 65, 71, 0);

    /* g_buf = [seq16][ct][tag4]，去掉前后得到纯明文 */
    size_t pt = g_n >= 6 ? g_n - 6 : 0;
    printf("\nwire=%zuB pt=%zuB\nplain:", g_n, pt);
    for (size_t i = 2; i < 2 + pt; i++) printf(" %02X", g_buf[i]);
    printf("\n");

    /* 期望（zimi.mosq.v1，type 码按 miot-spec DataType：BOOL=0 UINT8=1）：
     *   18 20 <tid> 00 04 03
     *   02 01 00 01 10 47   2.1 = 71 (uint8)
     *   03 01 00 01 10 41   3.1 = 65 (uint8)
     *   04 01 00 01 00 01   4.1 = 1  (bool)
     * App 的 gyb 解析器按这个码表反解，错一位就会把 bool 读成 uint8 并丢弃
     * 属性（logcat [PARSE] 实证），所以这里做硬断言。 */
    static const uint8_t want[] = {
        0x18, 0x20, 0x00, 0x00, 0x04, 0x03,
        0x02, 0x01, 0x00, 0x01, 0x10, 0x47,
        0x03, 0x01, 0x00, 0x01, 0x10, 0x41,
        0x04, 0x01, 0x00, 0x01, 0x00, 0x01
    };
    if (pt != sizeof(want) || memcmp(g_buf + 2, want, sizeof(want)) != 0) {
        printf("FAIL: 帧内容与 zimi.mosq.v1 期望不一致\n");
        return 1;
    }
    printf("OK: 帧内容匹配 zimi.mosq.v1\n");
    return 0;
}
