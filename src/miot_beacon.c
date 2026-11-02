/**
 * @file miot_beacon.c
 * @brief MiBeacon v5 加密广播帧实现（格式逆向自真机，实测互证）
 */
#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_crypto.h"
#include <string.h>

/* ---------------- 对象编码：[obj_id 2B LE][len 1B][value] ---------------- */

size_t miot_beacon_obj_u8(uint8_t *p, uint16_t obj_id, uint8_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 1; p[3] = v;
    return 4;
}

size_t miot_beacon_obj_u16(uint8_t *p, uint16_t obj_id, uint16_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 2;
    p[3] = (uint8_t)(v & 0xFF); p[4] = (uint8_t)(v >> 8);
    return 5;
}

size_t miot_beacon_obj_i16(uint8_t *p, uint16_t obj_id, int16_t v)
{
    return miot_beacon_obj_u16(p, obj_id, (uint16_t)v);
}

size_t miot_beacon_obj_u32(uint8_t *p, uint16_t obj_id, uint32_t v)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = 4;
    p[3] = (uint8_t)(v & 0xFF);         p[4] = (uint8_t)((v >> 8) & 0xFF);
    p[5] = (uint8_t)((v >> 16) & 0xFF); p[6] = (uint8_t)((v >> 24) & 0xFF);
    return 7;
}

size_t miot_beacon_obj_f32(uint8_t *p, uint16_t obj_id, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return miot_beacon_obj_u32(p, obj_id, bits);
}

size_t miot_beacon_obj_bytes(uint8_t *p, uint16_t obj_id, const uint8_t *v, size_t len)
{
    p[0] = (uint8_t)(obj_id & 0xFF); p[1] = (uint8_t)(obj_id >> 8);
    p[2] = (uint8_t)len;
    memcpy(p + 3, v, len);
    return 3 + len;
}

/* ---------------- v5 加密帧 ----------------
 * FC(2) | PID(2) | cnt(1) | MAC(6) | ct(N) | counter_ext(3) | MIC(4)
 * nonce = MAC(6) | pid_le(2) | cnt_le(4)；aad = 0x11；tag 4B 放帧尾 */

int miot_beacon_frame_v5(const miot_beacon_cfg_t *cfg, uint32_t frame_cnt,
                         const uint8_t *objs, size_t objs_len,
                         uint8_t *out, size_t cap)
{
    if (objs_len > 13) return -2;              /* 保证含公司码广播 ≤31B */
    const size_t total = 12 + objs_len + 3 + 4;
    if (cap < total) return -3;

    uint8_t nonce[12];
    memcpy(nonce, cfg->mac_air, 6);
    nonce[6] = (uint8_t)(cfg->pid & 0xFF);
    nonce[7] = (uint8_t)(cfg->pid >> 8);
    nonce[8]  = (uint8_t)(frame_cnt & 0xFF);
    nonce[9]  = (uint8_t)((frame_cnt >> 8) & 0xFF);
    nonce[10] = (uint8_t)((frame_cnt >> 16) & 0xFF);
    nonce[11] = (uint8_t)((frame_cnt >> 24) & 0xFF);

    int n = 0;
    out[n++] = 0x58;                            /* FC lo: obj|mac|enc */
    out[n++] = 0x59;                            /* FC hi: v5|registered（未注册 0x58 被 App 无视） */
    out[n++] = nonce[6];                        /* PID LE */
    out[n++] = nonce[7];
    out[n++] = nonce[8];                        /* frame_cnt（低字节） */
    memcpy(out + n, cfg->mac_air, 6);           /* MAC 字段：App 归属判定依据 */
    n += 6;

    /* 加密：miot_ccm_encrypt 输出 [ct][tag4]，帧内顺序要求
     * counter_ext(3) 夹在 ct 与 MIC 之间，故 tag 先落临时区再回填 */
    uint8_t aad = 0x11;
    uint8_t tmp[13 + 4];
    if (objs_len > sizeof(tmp) - 4) return -2;
    if (miot_ccm_encrypt(cfg->beacon_key, nonce, &aad, 1,
                         objs, objs_len, tmp) != 0)
        return -1;                              /* 未注册加密后端 */
    memcpy(out + n, tmp, objs_len);             /* ct */
    n += (int)objs_len;

    out[n++] = nonce[9];                        /* counter_ext(3) 在 MIC 之前 */
    out[n++] = nonce[10];
    out[n++] = nonce[11];
    memcpy(out + n, tmp + objs_len, 4);         /* MIC(4) 必须在最后 */
    n += 4;
    return n;
}
