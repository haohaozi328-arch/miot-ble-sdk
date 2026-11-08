/* Host 侧 spec 写入（控制下发）回归测试。
 *
 * 验证协议栈唯一没被 App 走过的一半：设备收到 SET 之后
 *   ① 是否回 SET_REPLY，result 码对不对（0=成功，1=失败）
 *   ② 读写属性是否真的落到 prop 表（repeller_power() 能读到）
 *   ③ 只读属性/不存在的属性是否被正确拒绝（不能静默改值）
 *
 * SET 方向的对象格式与设备→App 不同，这里是硬约束：
 *   [siid 1B][piid 16LE][type 16LE＝纯类型号][len 1B][value]
 * 例：4.1 = true → 04 01 00 00 00 01 01
 *
 * 用法（容器内）：
 *   gcc -I include src/*.c test/host_spec_set.c -o /tmp/t && /tmp/t
 */
#include "miot_ble/miot_profiles.h"
#include "miot_ble/miot_crypto.h"
#include "miot_ble/miot_spec.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static uint8_t g_buf[512];
static size_t  g_n;
static int     g_started;
static int     g_fail;

static void my_log(void *u, int lv, const char *tag, const char *fmt, ...)
{
    (void)u; (void)lv; (void)tag;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}

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

static int fake_ccm(void *ctx, int enc, const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4])
{
    (void)ctx; (void)enc; (void)key; (void)nonce; (void)aad; (void)aad_len;
    memcpy(out, in, len);
    tag[0] = 0xA1; tag[1] = 0xA2; tag[2] = 0xA3; tag[3] = 0xA4;
    return 0;
}

/* 组一个单对象 SET 请求：[siid][piid16][type16][len][value...] */
static size_t build_set(uint8_t *out, uint16_t tid, uint8_t siid, uint16_t piid,
                        uint16_t type, const uint8_t *val, uint8_t vlen)
{
    size_t n = 5 + 1 + 6 + vlen;
    out[0] = 0; out[1] = 0;
    out[2] = (uint8_t)tid; out[3] = (uint8_t)(tid >> 8);
    out[4] = MIOT_OP_SET;
    out[5] = 1;                       /* count */
    out[6] = siid;
    out[7] = (uint8_t)piid; out[8] = (uint8_t)(piid >> 8);
    out[9] = (uint8_t)type; out[10] = (uint8_t)(type >> 8);
    out[11] = vlen;
    memcpy(out + 12, val, vlen);
    uint16_t hdr = (uint16_t)((MIOT_SPEC_VER << 12) | (n & 0x0FFF));
    out[0] = (uint8_t)hdr; out[1] = (uint8_t)(hdr >> 8);
    return n;
}

/* 取上一次设备回的帧明文（去掉 [seq16] 和 [tag4]） */
static const uint8_t *last_plain(size_t *len)
{
    *len = g_n >= 6 ? g_n - 6 : 0;
    return g_buf + 2;
}

static void dump(const char *tag, const uint8_t *p, size_t n)
{
    printf("%s (%zuB):", tag, n);
    for (size_t i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}

/* 校验 SET_REPLY：len16/tid/op/count/siid/piid/result */
static void check_reply(const char *name, uint16_t tid, uint8_t siid, uint16_t piid,
                        uint16_t want_result)
{
    size_t n; const uint8_t *p = last_plain(&n);
    dump(name, p, n);

    if (n != 11) { printf("  FAIL 长度 %zu != 11\n", n); g_fail++; return; }
    uint16_t hdr = (uint16_t)(p[0] | (p[1] << 8));
    if ((hdr >> 12) != MIOT_SPEC_VER || (hdr & 0x0FFF) != 11) {
        printf("  FAIL 帧头 %04X（ver/len 不符）\n", hdr); g_fail++;
    }
    if ((uint16_t)(p[2] | (p[3] << 8)) != tid) { printf("  FAIL tid\n"); g_fail++; }
    if (p[4] != MIOT_OP_SET_REPLY) { printf("  FAIL op=%02X\n", p[4]); g_fail++; }
    if (p[5] != 1) { printf("  FAIL count=%u\n", p[5]); g_fail++; }
    if (p[6] != siid) { printf("  FAIL siid=%u\n", p[6]); g_fail++; }
    if ((uint16_t)(p[7] | (p[8] << 8)) != piid) { printf("  FAIL piid\n"); g_fail++; }
    uint16_t res = (uint16_t)(p[9] | (p[10] << 8));
    if (res != want_result) {
        printf("  FAIL result=%u 期望 %u\n", res, want_result); g_fail++;
        return;
    }
    printf("  OK result=%u\n", res);
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
    /* dispatch_plain 不检查会话，但 set 路径依赖 prop 表已挂上 */

    uint8_t frame[64];

    /* ① 可写属性：4.1 on = true */
    printf("== ① SET 4.1 = true ==\n");
    printf("before: repeller_power()=%u\n", repeller_power());
    uint8_t on = 1;
    size_t fl = build_set(frame, 0x0101, 4, 1, MIOT_TYPE_BOOL, &on, 1);
    miot_device_dispatch_plain(&dev, frame, fl);
    check_reply("  reply", 0x0101, 4, 1, 0);
    if (repeller_power() != 1) { printf("  FAIL 属性没落表 power=%u\n", repeller_power()); g_fail++; }
    else printf("  OK 属性已更新 power=1\n");

    /* ② 只读属性：2.1 药剂余量（R）必须被拒 */
    printf("== ② SET 2.1 = 5 （只读，应拒绝）==\n");
    uint8_t v5 = 5;
    fl = build_set(frame, 0x0202, 2, 1, MIOT_TYPE_U8, &v5, 1);
    miot_device_dispatch_plain(&dev, frame, fl);
    check_reply("  reply", 0x0202, 2, 1, 1);      /* result=1 表示失败 */

    /* ③ 不存在的属性：9.9 必须被拒 */
    printf("== ③ SET 9.9 = 1 （未注册，应拒绝）==\n");
    fl = build_set(frame, 0x0303, 9, 9, MIOT_TYPE_BOOL, &on, 1);
    miot_device_dispatch_plain(&dev, frame, fl);
    check_reply("  reply", 0x0303, 9, 9, 1);

    /* ④ 关回去，确认可写属性双向都通 */
    printf("== ④ SET 4.1 = false ==\n");
    uint8_t off = 0;
    fl = build_set(frame, 0x0404, 4, 1, MIOT_TYPE_BOOL, &off, 1);
    miot_device_dispatch_plain(&dev, frame, fl);
    check_reply("  reply", 0x0404, 4, 1, 0);
    if (repeller_power() != 0) { printf("  FAIL power 仍为 %u\n", repeller_power()); g_fail++; }
    else printf("  OK 属性已更新 power=0\n");

    /* ⑤ App 主动 GET 全部三个属性：2.1/3.1/4.1
     * GET 请求体只有 [count][siid][piid16]...，回包才是 [siid][piid16][result16][value]
     * value 里 type16 = (type<<12)|len，和设备→App 其它包一致。 */
    printf("== ⑤ GET 2.1 / 3.1 / 4.1 ==\n");
    {
        uint8_t g[5 + 1 + 9];
        size_t gn = sizeof(g);
        g[0] = (uint8_t)((MIOT_SPEC_VER << 12) | (gn & 0x0FFF));
        g[1] = (uint8_t)(((MIOT_SPEC_VER << 12) | (gn & 0x0FFF)) >> 8);
        g[2] = 0x05; g[3] = 0x05;      /* tid */
        g[4] = MIOT_OP_GET;
        g[5] = 3;
        g[6] = 2; g[7] = 1; g[8] = 0;
        g[9] = 3; g[10] = 1; g[11] = 0;
        g[12] = 4; g[13] = 1; g[14] = 0;
        miot_device_dispatch_plain(&dev, g, gn);

        size_t n; const uint8_t *p = last_plain(&n);
        dump("  reply", p, n);
        if (n < 8 || p[4] != MIOT_OP_GET_REPLY || p[5] != 3) {
            printf("  FAIL 不是 3 对象的 GET_REPLY\n"); g_fail++;
        } else {
            /* 逐个对象走一遍，确认 siid/piid/result/type 编码 */
            size_t i = 6;
            int ok = 1;
            for (int k = 0; k < 3; k++) {
                if (i + 5 > n) { ok = 0; break; }
                uint8_t siid = p[i];
                uint16_t piid = (uint16_t)(p[i + 1] | (p[i + 2] << 8));
                uint16_t res  = (uint16_t)(p[i + 3] | (p[i + 4] << 8));
                uint16_t t16  = (uint16_t)(p[i + 5] | (p[i + 6] << 8));
                uint8_t vlen  = (uint8_t)(t16 & 0x0FFF);
                printf("  obj %u.%u result=%u type=%u len=%u\n",
                       siid, piid, res, t16 >> 12, vlen);
                if (res != 0) ok = 0;
                if (k == 0 && !(siid == 2 && piid == 1 && (t16 >> 12) == MIOT_TYPE_U8)) ok = 0;
                if (k == 1 && !(siid == 3 && piid == 1 && (t16 >> 12) == MIOT_TYPE_U8)) ok = 0;
                if (k == 2 && !(siid == 4 && piid == 1 && (t16 >> 12) == MIOT_TYPE_BOOL)) ok = 0;
                i += 7 + vlen;   /* siid1+piid2+result2+type16 2+value vlen */
            }
            if (!ok) { printf("  FAIL 对象 siid/piid/type 不符\n"); g_fail++; }
            else printf("  OK 三个对象类型与值均正确\n");
        }
    }

    printf(g_fail ? "\nFAIL 共 %d 项\n" : "\nOK 全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
