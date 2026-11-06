/**
 * @file test_psk.c
 * @brief legacy PSK 状态机单测（host，无加密库）
 *
 * 只验证「帧序列与状态流转」，不验证密码学正确性——那需要真后端
 * （mbedTLS/OpenSSL）和一次真机实测，见 docs/psk-auth.md。
 *
 * 覆盖：
 *   1. PSK 模式：启动帧 → device info → 公钥 → 五帧握手 → server pub → did
 *   2. AUTO 模式：标准认证不受 PSK 分支影响
 *   3. STANDARD 模式：26B 启动帧不会误切 PSK
 *   4. 已绑定重连：KDF 换用 smartcfg-login-*（密钥与首绑不同）
 */
#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_crypto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================= 确定性加密后端 ================= */

static uint32_t s_seed = 0x12345678u;

static int st_hmac(void *c, const uint8_t *key, size_t klen,
                   const uint8_t *data, size_t dlen, uint8_t out[32])
{
    (void)c;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < klen; i++) h = (h ^ key[i]) * 16777619u;
    for (size_t i = 0; i < dlen; i++) h = (h ^ data[i]) * 16777619u;
    for (int i = 0; i < 32; i++) {
        h = h * 1103515245u + 12345u;
        out[i] = (uint8_t)(h >> 16);
    }
    return 0;
}

static int st_ccm(void *c, int enc, const uint8_t key[16], const uint8_t nonce[12],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4])
{
    (void)c; (void)aad; (void)aad_len;
    for (size_t i = 0; i < len; i++) out[i] = in[i] ^ key[i % 16] ^ nonce[i % 12];
    if (enc) memcpy(tag, nonce, 4);
    return 0;
}

static int st_keygen(void *c, uint8_t pub64[64])
{
    (void)c;
    for (int i = 0; i < 64; i++) pub64[i] = (uint8_t)(0x10 + i);
    return 0;
}

static int st_shared(void *c, const uint8_t app_pub[64], uint8_t secret[32])
{
    (void)c;
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(app_pub[i] ^ 0x5A ^ (uint8_t)i);
    return 0;
}

static int st_sign(void *c, const uint8_t *msg, size_t len, uint8_t sig64[64])
{
    (void)c;
    uint8_t h[32];
    st_hmac(NULL, (const uint8_t *)"k", 1, msg, len, h);
    memcpy(sig64, h, 32);
    memcpy(sig64 + 32, h, 32);
    return 0;
}

static int st_random(void *user, uint8_t *out, size_t len)
{
    (void)user;
    for (size_t i = 0; i < len; i++) {
        s_seed = s_seed * 1664525u + 1013904223u;
        out[i] = (uint8_t)(s_seed >> 24);
    }
    return 0;
}

static const miot_crypto_ops_t k_ops = {
    .ctx = NULL, .hmac_sha256 = st_hmac, .aes_ccm = st_ccm,
    .ecdh_keygen = st_keygen, .ecdh_shared = st_shared, .ecdsa_sign = st_sign,
};

/* ================= notify 采集 ================= */

static uint8_t s_frames[64][160];
static size_t  s_frame_len[64];
static int     s_nframes;
static uint8_t s_r10[16][8];
static int     s_nr10;

static void reset_capture(void)
{
    s_nframes = 0;
    s_nr10 = 0;
}

static int cap19(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    if (s_nframes < 64 && len <= 160) {
        memcpy(s_frames[s_nframes], d, len);
        s_frame_len[s_nframes] = len;
        s_nframes++;
    }
    return 0;
}

static int cap10(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    if (s_nr10 < 16 && len <= 8) {
        memcpy(s_r10[s_nr10], d, len);
        s_nr10++;
    }
    return 0;
}

/* 某 pkg_type 的 ag2 分片 START 帧是否出现过 */
static int saw_parcel(uint8_t pkg)
{
    for (int i = 0; i < s_nframes; i++)
        if (s_frame_len[i] >= 6 && s_frames[i][0] == 0x00 && s_frames[i][1] == 0x00 &&
            s_frames[i][2] == 0x00 && s_frames[i][3] == pkg)
            return 1;
    return 0;
}

/* device info：channel data 帧 [00 00 02 00 ...] */
static int saw_device_info(void)
{
    for (int i = 0; i < s_nframes; i++)
        if (s_frame_len[i] >= 8 && s_frames[i][0] == 0x00 && s_frames[i][1] == 0x00 &&
            s_frames[i][2] == 0x02 && s_frames[i][3] == 0x00)
            return 1;
    return 0;
}

static int saw_result(uint8_t code)
{
    for (int i = 0; i < s_nr10; i++)
        if (s_r10[i][0] == code) return 1;
    return 0;
}

/* ================= 断言 ================= */

static int s_fail;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else      { printf("  FAIL %s\n", msg); s_fail++; } \
} while (0)

/* ================= 构造 App → 设备 帧 ================= */

/* ag2 SINGLE：[00 00][02][pkgType][payload] */
static size_t mk_single(uint8_t *out, uint8_t pkg, const uint8_t *p, size_t n)
{
    out[0] = 0x00; out[1] = 0x00; out[2] = 0x02; out[3] = pkg;
    memcpy(out + 4, p, n);
    return n + 4;
}

/* ================= 用例 ================= */

static void case_psk_full_flow(void)
{
    printf("[1] PSK 首绑全流程\n");
    miot_auth_t a;
    miot_auth_port_t port = {
        .notify_0019 = cap19, .notify_0010 = cap10, .random = st_random,
    };
    miot_auth_init(&a, &port);
    miot_auth_set_mode(&a, MIOT_AUTH_MODE_PSK);
    miot_auth_set_psk_options(&a, 0, MIOT_PSK_SIGN_DEV_APP_RAND);
    reset_capture();

    uint8_t f[128];

    /* 1. 启动帧 26B */
    uint8_t start[26];
    for (int i = 0; i < 26; i++) start[i] = (uint8_t)(0xA0 + i);
    size_t n = mk_single(f, 0x00, start, sizeof(start));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_state == MIOT_PSK_WAIT_START, "启动帧后进入 WAIT_START");
    CHECK(a.psk_start_len == 26, "启动帧原样留存 26B");
    CHECK(saw_device_info(), "回了 device info（App 才会继续）");

    /* 2. App 公钥 64B */
    reset_capture();
    uint8_t app_pub[64];
    for (int i = 0; i < 64; i++) app_pub[i] = (uint8_t)(0x80 + i);
    n = mk_single(f, 0x00, app_pub, sizeof(app_pub));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_ecdh_ok == 1, "ECDH 完成");
    CHECK(a.psk_state == MIOT_PSK_WAIT_SERVER_PUB, "进入 WAIT_SERVER_PUB");
    CHECK(saw_parcel(0x0D), "发出 devRandom(0x0D)");
    CHECK(saw_parcel(0x03), "发出 devPubKey(0x03)");
    CHECK(saw_parcel(0x0C), "发出 devConfirm(0x0C)");
    CHECK(saw_parcel(0x04), "发出 devSign(0x04) ← PSK 唯一增量帧");
    CHECK(a.psk_sign_len == 64, "签名长度 64B");

    /* 3. server pub 16B */
    uint8_t spub[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    n = mk_single(f, 0x00, spub, sizeof(spub));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_state == MIOT_PSK_WAIT_BIND, "server pub 后进入 WAIT_BIND");

    /* 4. 绑定结果 13B：[0x00][12B ASCII did] */
    reset_capture();
    uint8_t bind[13] = {0x00, '1','2','3','4','5','6','7','8','9','0','1','2'};
    n = mk_single(f, 0x00, bind, sizeof(bind));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_state == MIOT_PSK_DONE, "绑定完成");
    CHECK(a.bound == 1, "bound=1");
    CHECK(memcmp(a.id.did, "123456789012", 12) == 0, "did 从 13B 结果帧解析");
    CHECK(saw_result(MIOT_AUTH_R_REG_OK), "0x0010 回 0x11 结果码");
}

static void case_auto_standard_untouched(void)
{
    printf("[2] AUTO 模式：标准认证不受影响\n");
    miot_auth_t a;
    miot_auth_port_t port = { .notify_0019 = cap19, .notify_0010 = cap10, .random = st_random };
    miot_auth_init(&a, &port);            /* 默认 AUTO */
    reset_capture();

    uint8_t cmd[4] = { MIOT_AUTH_CMD_REG, 0, 0, 0 };
    miot_auth_on_0010(&a, cmd, 4);
    CHECK(a.state == MIOT_AUTH_ST_REG_PUBKEY, "0xA2 进入标准 REGISTER");
    CHECK(a.psk_state == MIOT_PSK_IDLE, "PSK 分支未被激活");
    CHECK(saw_device_info(), "标准 device info 照常发出");

    /* 标准注册的 64B 公钥不应被 PSK 抢走 */
    reset_capture();
    uint8_t pk[64];
    memset(pk, 0x33, sizeof(pk));
    uint8_t f[128];
    size_t n = mk_single(f, 0x00, pk, sizeof(pk));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.state == MIOT_AUTH_ST_REG_ENCDATA, "标准认证进入 REG_ENCDATA");
    CHECK(a.psk_ecdh_ok == 0, "PSK 未接管");
    CHECK(saw_parcel(0x03) && saw_parcel(0x0D) && saw_parcel(0x0C), "标准三帧照常发出");
    CHECK(!saw_parcel(0x04), "标准认证不发 0x04（不属于该协议）");
}

static void case_standard_mode_ignores_start(void)
{
    printf("[3] STANDARD 模式：26B 启动帧不误切 PSK\n");
    miot_auth_t a;
    miot_auth_port_t port = { .notify_0019 = cap19, .notify_0010 = cap10, .random = st_random };
    miot_auth_init(&a, &port);
    miot_auth_set_mode(&a, MIOT_AUTH_MODE_STANDARD);
    reset_capture();

    uint8_t f[128], start[26];
    memset(start, 0xA5, sizeof(start));
    size_t n = mk_single(f, 0x00, start, sizeof(start));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_state == MIOT_PSK_IDLE && a.psk_start_len == 0, "未切 PSK");
}

static void case_relogin_uses_login_label(void)
{
    printf("[4] 已绑定重连：改用 smartcfg-login-* 派生\n");
    miot_auth_t a;
    miot_auth_port_t port = { .notify_0019 = cap19, .notify_0010 = cap10, .random = st_random };
    miot_auth_init(&a, &port);
    miot_auth_set_mode(&a, MIOT_AUTH_MODE_PSK);
    reset_capture();

    uint8_t f[128], start[26];
    memset(start, 0xA0, sizeof(start));
    size_t n = mk_single(f, 0x00, start, sizeof(start));
    miot_auth_on_0019(&a, f, n);

    uint8_t app_pub[64];
    for (int i = 0; i < 64; i++) app_pub[i] = (uint8_t)(0x80 + i);
    n = mk_single(f, 0x00, app_pub, sizeof(app_pub));
    miot_auth_on_0019(&a, f, n);
    uint8_t setup_key[32];
    memcpy(setup_key, a.psk_key, 32);

    /* 模拟已绑定状态后重新握手，应走 login 标签 */
    a.bound = 1;
    miot_auth_on_disconnect(&a);
    miot_auth_set_mode(&a, MIOT_AUTH_MODE_PSK);
    reset_capture();

    uint8_t cmd[4] = { MIOT_AUTH_CMD_LOGIN, 0, 0, 0 };
    miot_auth_on_0010(&a, cmd, 4);
    CHECK(a.state == MIOT_AUTH_ST_LOGIN_AWAIT_RAND, "0x24 进入 LOGIN 等待");
    CHECK(saw_parcel(0x03), "重连先发设备公钥(0x03)");

    n = mk_single(f, 0x00, app_pub, sizeof(app_pub));
    miot_auth_on_0019(&a, f, n);
    CHECK(a.psk_ecdh_ok == 1, "重连 ECDH 完成");
    CHECK(memcmp(setup_key, a.psk_key, 32) != 0, "login 标签派生出不同密钥");
}

int main(void)
{
    miot_crypto_set_ops(&k_ops);
    case_psk_full_flow();
    case_auto_standard_untouched();
    case_standard_mode_ignores_start();
    case_relogin_uses_login_label();

    printf("\n%s (%d failed)\n", s_fail ? "FAILED" : "ALL PASSED", s_fail);
    return s_fail ? 1 : 0;
}
