/**
 * @file miot_crypto.c
 * @brief 加密抽象层实现（算法部分本地实现，原语走已注册后端）
 */
#include "miot_ble/miot_crypto.h"
#include <string.h>

static const miot_crypto_ops_t *s_ops = NULL;

void miot_crypto_set_ops(const miot_crypto_ops_t *ops) { s_ops = ops; }

/* ---------------- RC4（旧版 beacon 鉴权） ---------------- */

void miot_rc4(const uint8_t *key, size_t klen, const uint8_t *in, uint8_t *out, size_t len)
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

/* mixA/mixB：m=设备 MAC 空中序，pid 小端两字节（等价米家 tool_mix_1/2） */
void miot_mix_a(const uint8_t m[6], uint16_t pid, uint8_t k[8])
{
    k[0] = m[0]; k[1] = m[2]; k[2] = m[5];
    k[3] = (uint8_t)(pid & 0xFF); k[4] = (uint8_t)(pid & 0xFF);
    k[5] = m[4]; k[6] = m[5]; k[7] = m[1];
}

void miot_mix_b(const uint8_t m[6], uint16_t pid, uint8_t k[8])
{
    k[0] = m[0]; k[1] = m[2]; k[2] = m[5];
    k[3] = (uint8_t)(pid >> 8);
    k[4] = m[4]; k[5] = m[0]; k[6] = m[5]; k[7] = (uint8_t)(pid & 0xFF);
}

/* ---------------- HKDF-SHA256（extract + expand） ---------------- */

int miot_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *salt, size_t salt_len,
                     const char *info, size_t info_len,
                     uint8_t *out, size_t out_len)
{
    if (!s_ops || !s_ops->hmac_sha256) return -1;

    uint8_t prk[32];
    static const uint8_t zeros[32] = {0};
    if (!salt || salt_len == 0) { salt = zeros; salt_len = 32; }
    int rc = s_ops->hmac_sha256(s_ops->ctx, salt, salt_len, ikm, ikm_len, prk);
    if (rc) return rc;

    uint8_t t[32], buf[80];
    size_t tlen = 0, pos = 0;
    uint8_t c = 1;
    while (pos < out_len) {
        memcpy(buf, t, tlen);
        if (info && info_len) memcpy(buf + tlen, info, info_len);
        buf[tlen + info_len] = c++;
        rc = s_ops->hmac_sha256(s_ops->ctx, prk, 32, buf, tlen + info_len + 1, t);
        if (rc) return rc;
        size_t n = (out_len - pos < 32) ? (out_len - pos) : 32;
        memcpy(out + pos, t, n);
        pos += n;
        tlen = 32;
    }
    return 0;
}

/* ---------------- spec 通道 k61 帧加解密 ----------------
 * 帧 = [seq16 LE][ct][tag4]；nonce = iv(4) || 00 00 00 00 || seq32 LE */

int miot_spec_frame_encrypt(const uint8_t key[16], const uint8_t iv[4], uint16_t seq,
                            const uint8_t *pt, size_t len, uint8_t *out)
{
    if (!s_ops || !s_ops->aes_ccm) return -1;
    uint8_t nonce[12];
    memcpy(nonce, iv, 4);
    memset(nonce + 4, 0, 4);
    nonce[8] = (uint8_t)(seq & 0xFF);
    nonce[9] = (uint8_t)(seq >> 8);
    return s_ops->aes_ccm(s_ops->ctx, 1, key, nonce, pt, len, out, out + len);
}

int miot_spec_frame_decrypt(const uint8_t key[16], const uint8_t iv[4], uint16_t seq,
                            const uint8_t *in, size_t len, uint8_t *pt)
{
    if (!s_ops || !s_ops->aes_ccm) return -1;
    uint8_t nonce[12];
    memcpy(nonce, iv, 4);
    memset(nonce + 4, 0, 4);
    nonce[8] = (uint8_t)(seq & 0xFF);
    nonce[9] = (uint8_t)(seq >> 8);
    return s_ops->aes_ccm(s_ops->ctx, 0, key, nonce, in, len, pt, pt + len);
}
