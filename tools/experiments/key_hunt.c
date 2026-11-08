/* 密钥猎手 v2：大范围穷举 (key,iv,nonce布局) 组合 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "miot_ble/miot_crypto.h"

static int unhex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int try_one(const char *kn, const char *nn, const uint8_t key[16],
                   const uint8_t *iv4, uint16_t seq, const uint8_t *ct,
                   size_t ctlen, const uint8_t *tag, int nonce_mode)
{
    uint8_t nonce[12] = { 0 };
    if (nonce_mode == 0) {              /* 标准：iv4 || 0*4 || seq16@8 */
        memcpy(nonce, iv4, 4);
        nonce[8] = (uint8_t)(seq & 0xFF);
        nonce[9] = (uint8_t)(seq >> 8);
    } else if (nonce_mode == 1) {       /* seq16@0 紧跟 iv */
        memcpy(nonce, iv4, 4);
        nonce[4] = (uint8_t)(seq & 0xFF);
        nonce[5] = (uint8_t)(seq >> 8);
    } else if (nonce_mode == 2) {       /* iv4 在尾 */
        memcpy(nonce + 8, iv4, 4);
        nonce[0] = (uint8_t)(seq & 0xFF);
        nonce[1] = (uint8_t)(seq >> 8);
    } else {                            /* 全 12B iv + seq 覆盖前 2B */
        memcpy(nonce, iv4, 4);
        memcpy(nonce + 4, iv4, 4);
        nonce[8] = (uint8_t)(seq & 0xFF);
        nonce[9] = (uint8_t)(seq >> 8);
    }
    uint8_t pt[64];
    uint8_t buf[64];
    memcpy(buf, ct, ctlen);
    memcpy(buf + ctlen, tag, 4);
    if (miot_ccm_decrypt(key, nonce, NULL, 0, buf, ctlen, pt) == 0) {
        printf(">>> HIT key=%s nonce=%s seq=%u\n", kn, nn, seq);
        printf("    pt: ");
        for (size_t i = 0; i < ctlen; i++) printf("%02x", pt[i]);
        printf("\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t E[48];
    unhex("ae66a2cd79fac67fe7fac7d2a7838eca"
          "9925f15fe4de3be22ec6cc3e237b1c2e"
          "b6785c6d63b38b2a43a4b07f0f04ccd"
          "1125fc203ddf5d9b52470f5268943dd0", E, 48);

    /* token（hook: CHIPSES in=489775c2a93c9a65501b091c） */
    uint8_t token[12];
    unhex("489775c2a93c9a65501b091c", token, 12);

    uint8_t mk[64], lg[64];
    miot_hkdf_sha256(E, 48, (const uint8_t *)"smartcfg-masterkey-salt", 22,
                     "smartcfg-masterkey-info", 22, mk, 64);
    miot_hkdf_sha256(E, 48, (const uint8_t *)"smartcfg-login-salt", 19,
                     "smartcfg-login-info", 18, lg, 64);

    /* 材料 = E, mk, lg, se, token+pad */
    uint8_t tk[16];
    memset(tk, 0, 16);
    memcpy(tk, token, 12);

    struct { const char *nm; const uint8_t *d; size_t n; } mats[] = {
        { "E",    E,  48 },
        { "mk",   mk, 64 },
        { "lg",   lg, 64 },
        { "tk",   tk, 16 },
    };

    /* 帧：seq / ct / tag */
    uint8_t c0[11], c1[30], c2[30];
    unhex("0000b8125f14c825674256", c0, 11);
    unhex("0100ba4e6a42338952cbe58f12603389be7f04ec19fd36e37f91f3174824", c1, 30);
    unhex("0200261b34a36bee29097fbf181e23bb38463637863656d99d7baf80c08a", c2, 30);
    struct { uint16_t seq; uint8_t *f; size_t ctlen; const char *nm; } frames[] = {
        { 0, c0, 5,  "ver0" },
        { 1, c1, 24, "props1" },
        { 2, c2, 24, "props2" },
    };

    int hits = 0;
    const char *nms[] = { "std", "seq@4", "iv@8", "ivx2" };
    for (size_t m = 0; m < sizeof(mats) / sizeof(mats[0]); m++) {
        for (size_t ko = 0; ko + 16 <= mats[m].n; ko += 4) {
            for (size_t io = 0; io + 4 <= mats[m].n; io += 4) {
                for (int nm = 0; nm < 4; nm++) {
                    for (size_t f = 0; f < 3; f++) {
                        char kn[32], nn[32];
                        snprintf(kn, sizeof kn, "%s+%zu", mats[m].nm, ko * 8);
                        snprintf(nn, sizeof nn, "%s+%zu/%s", mats[m].nm, io * 8, nms[nm]);
                        if (try_one(kn, nn, mats[m].d + ko, mats[m].d + io,
                                    frames[f].seq, frames[f].f + 2,
                                    frames[f].ctlen, frames[f].f + 2 + frames[f].ctlen, nm))
                            hits++;
                    }
                }
            }
        }
    }
    printf("hits=%d\n", hits);
    return 0;
}
