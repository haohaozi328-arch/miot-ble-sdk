/* ECDSA-P256 签名（pkg4 用），大端定长 r||s 输出。
 * IDF v6 的 mbedtls 4.x 把 ECDSA/ECP 挪到 mbedtls/private/ 下，
 * 与 port/esp32/miot_port_esp32.c 相同的 include 策略。 */
#include <string.h>
#include "esp_random.h"
#include "pkg4_sign.h"
#include "eph_key.h"

#if __has_include("mbedtls/private/ecdsa.h")
#include "mbedtls/private/ecdsa.h"
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/ecdh.h"
#include "mbedtls/private/sha256.h"
#else
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/sha256.h"
#endif

static int rng_blind(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

int mesh_sign_selftest(void);

int mesh_sign_sha256(const uint8_t *msg, size_t len, uint8_t out64[64])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    uint8_t hash[32];
    int ret;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_sha256(msg, len, hash, 0);
    if (ret) {
        goto out;
    }
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret) {
        goto out;
    }
    ret = mbedtls_mpi_read_binary(&d, g_eph_priv, sizeof(g_eph_priv));
    if (ret) {
        goto out;
    }
    ret = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, sizeof(hash),
                                     MBEDTLS_MD_SHA256, rng_blind, NULL);
    if (ret) {
        goto out;
    }
    ret = mbedtls_mpi_write_binary(&r, out64, 32);
    if (ret) {
        goto out;
    }
    ret = mbedtls_mpi_write_binary(&s, out64 + 32, 32);

out:
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);
    return ret;
}

/* 自检：签名 + 用 g_eph_pub 验签。返回 0 = 链路正确。 */
int mesh_sign_selftest(void)
{
    static const char msg[] = "miot-pkg4-selftest";
    uint8_t sig[64];
    uint8_t pub65[65];
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    int ret;

    if (mesh_sign_sha256((const uint8_t *)msg, sizeof(msg) - 1, sig) != 0) {
        return -1;
    }
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    pub65[0] = 0x04;
    memcpy(pub65 + 1, g_eph_pub, 64);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!ret) ret = mbedtls_ecp_point_read_binary(&grp, &Q, pub65, sizeof(pub65));
    if (!ret) ret = mbedtls_mpi_read_binary(&r, sig, 32);
    if (!ret) ret = mbedtls_mpi_read_binary(&s, sig + 32, 32);
    if (!ret) {
        uint8_t hash[32];
        ret = mbedtls_sha256((const uint8_t *)msg, sizeof(msg) - 1, hash, 0);
        if (!ret) ret = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s);
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ret;
}

void mesh_sha256(const uint8_t *msg, size_t len, uint8_t out32[32])
{
    mbedtls_sha256(msg, len, out32, 0);
}

/* 对 32B 摘要直接签名（不做 SHA-256 预处理，等价于「签一个哈希值」）。 */
int mesh_sign_digest(const uint8_t digest[32], uint8_t out64[64])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    int ret;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!ret) ret = mbedtls_mpi_read_binary(&d, g_eph_priv, sizeof(g_eph_priv));
    if (!ret) ret = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, digest, 32,
                                               MBEDTLS_MD_SHA256, rng_blind, NULL);
    if (!ret) ret = mbedtls_mpi_write_binary(&r, out64, 32);
    if (!ret) ret = mbedtls_mpi_write_binary(&s, out64 + 32, 32);

    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);
    return ret;
}

/* ECDH：Z = X 坐标(32B 大端) = (g_eph_priv × peer_pub).x
 * 与 Java 侧 SecretKey.getEncoded() 对齐（小米 App 用同一约定）。 */
int mesh_ecdh_shared(const uint8_t peer_pub64[64], uint8_t out32[32])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d, z;
    uint8_t pub65[65];
    int ret;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);

    pub65[0] = 0x04;
    memcpy(pub65 + 1, peer_pub64, 64);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!ret) ret = mbedtls_ecp_point_read_binary(&grp, &Q, pub65, sizeof(pub65));
    if (!ret) ret = mbedtls_ecp_check_pubkey(&grp, &Q);
    if (!ret) ret = mbedtls_mpi_read_binary(&d, g_eph_priv, sizeof(g_eph_priv));
    if (!ret) ret = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, rng_blind, NULL);
    if (!ret) ret = mbedtls_mpi_write_binary(&z, out32, 32);

    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ret;
}
