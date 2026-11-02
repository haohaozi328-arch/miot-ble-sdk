/**
 * @file miot_port_esp32.c
 * @brief ESP32 (mbedTLS) 加密后端参考实现
 *
 * 把 SDK 的 miot_crypto_ops_t 对接到 mbedTLS。
 * 任意平台移植照此模式实现五个原语即可：
 *   hmac_sha256 / aes_ccm / ecdh_keygen / ecdh_shared / random
 */
#include "miot_ble/miot_crypto.h"
#include "mbedtls/md.h"
#include "mbedtls/ccm.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include <string.h>

/* ---- ECDH 后端上下文：持有设备私钥 ---- */
typedef struct {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;              /* 私钥 */
    mbedtls_ecp_point Q;        /* 设备公钥 */
    int ready;
} ecdh_ctx_t;

static ecdh_ctx_t s_ecdh;

static int esp_hmac_sha256(void *ctx, const uint8_t *key, size_t klen,
                           const uint8_t *data, size_t dlen, uint8_t out[32])
{
    (void)ctx;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return mbedtls_md_hmac(md, key, klen, data, dlen, out);
}

static int esp_aes_ccm(void *ctx, int enc, const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[4])
{
    (void)ctx;
    mbedtls_ccm_context c;
    mbedtls_ccm_init(&c);
    int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0) {
        if (enc) {
            rc = mbedtls_ccm_encrypt_and_tag(&c, len, nonce, 12, aad, aad_len,
                                             in, out, tag, 4);
        } else {
            rc = mbedtls_ccm_auth_decrypt(&c, len, nonce, 12, aad, aad_len,
                                          in, out, tag, 4);
        }
    }
    mbedtls_ccm_free(&c);
    return rc;
}

static int esp_ecdh_keygen(void *ctx, uint8_t pubkey64[64])
{
    ecdh_ctx_t *e = (ecdh_ctx_t *)ctx;
    if (!e->ready) {
        mbedtls_ecp_group_init(&e->grp);
        mbedtls_mpi_init(&e->d);
        mbedtls_ecp_point_init(&e->Q);
        if (mbedtls_ecp_group_load(&e->grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return -1;
        e->ready = 1;
    }
    /* 密钥对只生成一次（预生成：ESP32-C3 上 ~390ms，放在 a2 处理里
     * 会导致 device info 迟到、App 以 -51 断开 —— 固件 v15 实测） */
    if (mbedtls_ecp_gen_keypair(&e->grp, &e->d, &e->Q, mbedtls_ctr_drbg_random, NULL) != 0)
        return -1;
    return mbedtls_mpi_write_binary(&e->Q.MBEDTLS_PRIVATE(X), pubkey64, 32) |
           mbedtls_mpi_write_binary(&e->Q.MBEDTLS_PRIVATE(Y), pubkey64 + 32, 32);
}

static int esp_ecdh_shared(void *ctx, const uint8_t app_pubkey64[64], uint8_t secret[32])
{
    ecdh_ctx_t *e = (ecdh_ctx_t *)ctx;
    if (!e->ready) return -1;

    mbedtls_ecp_point qapp;
    mbedtls_ecp_point_init(&qapp);
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);

    uint8_t pt65[65];
    pt65[0] = 0x04;
    memcpy(pt65 + 1, app_pubkey64, 64);

    int rc = mbedtls_ecp_point_read_binary(&e->grp, &qapp, pt65, sizeof(pt65));
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(&e->grp, &qapp);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&e->grp, &z, &qapp, &e->d,
                                                  mbedtls_ctr_drbg_random, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&z, secret, 32);

    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&qapp);
    return rc;
}

static const miot_crypto_ops_t s_esp_crypto = {
    .ctx          = &s_ecdh,
    .hmac_sha256  = esp_hmac_sha256,
    .aes_ccm      = esp_aes_ccm,
    .ecdh_keygen  = esp_ecdh_keygen,
    .ecdh_shared  = esp_ecdh_shared,
};

void miot_port_esp32_crypto_init(void)
{
    miot_crypto_set_ops(&s_esp_crypto);
}
