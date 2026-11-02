/**
 * @file miot_port_esp32.c
 * @brief ESP32 (mbedTLS) 加密后端参考实现
 *
 * 把 SDK 的 miot_crypto_ops_t 对接到 mbedTLS。
 * 任意平台移植照此模式实现 hmac_sha256 / aes_ccm 两个原语即可。
 */
#include "miot_ble/miot_crypto.h"
#include "mbedtls/md.h"
#include "mbedtls/ccm.h"
#include <string.h>

static int esp_hmac_sha256(void *ctx, const uint8_t *key, size_t klen,
                           const uint8_t *data, size_t dlen, uint8_t out[32])
{
    (void)ctx;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(md, key, klen, data, dlen, out);
}

static int esp_aes_ccm(void *ctx, int enc, const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4])
{
    (void)ctx;
    mbedtls_ccm_context c;
    mbedtls_ccm_init(&c);
    int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        if (enc) {
            rc = mbedtls_ccm_encrypt_and_tag(&c, len, nonce, 12, NULL, 0,
                                             in, out, tag, 4);
        } else {
            rc = mbedtls_ccm_auth_decrypt(&c, len, nonce, 12, NULL, 0,
                                          in, out, tag, 4);
        }
    }
    mbedtls_ccm_free(&c);
    return rc;
}

static const miot_crypto_ops_t s_esp_crypto = {
    .ctx = NULL,
    .hmac_sha256 = esp_hmac_sha256,
    .aes_ccm     = esp_aes_ccm,
};

void miot_port_esp32_crypto_init(void)
{
    miot_crypto_set_ops(&s_esp_crypto);
}
