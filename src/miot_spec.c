/**
 * @file miot_spec.c
 * @brief MIoT BLE Spec v2 帧编解码实现（从 S400 模拟秤固件抽象而来）
 */
#include "miot_ble/miot_spec.h"
#include <string.h>

/* ---------------- 内部小工具 ---------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/** 写入帧头占位：[00 00][tid16][op] */
static size_t hdr_begin(uint8_t *out, uint16_t tid, uint8_t op)
{
    out[0] = 0; out[1] = 0;                 /* len16 占位 */
    put16(out + 2, tid);
    out[4] = op;
    return 5;
}

/** 回填 len16：ver=2 高 4 位 | 总长低 12 位 */
static void hdr_finish(uint8_t *out, size_t n)
{
    uint16_t v = (uint16_t)((MIOT_SPEC_VER << 12) | (n & 0x0FFF));
    put16(out, v);
}

size_t miot_spec_encode_value(const miot_spec_value_t *v, uint8_t *out, size_t cap)
{
    uint8_t buf[8];
    uint8_t vlen = 0;

    switch (v->type) {
    case MIOT_TYPE_BOOL:
        buf[0] = v->val.u8 ? 1 : 0; vlen = 1; break;
    case MIOT_TYPE_U8:
        buf[0] = v->val.u8; vlen = 1; break;
    case MIOT_TYPE_U16:
        put16(buf, v->val.u16); vlen = 2; break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:
        memcpy(buf, &v->val.u32, 4); vlen = 4; break;
    case MIOT_TYPE_FLOAT:
        memcpy(buf, &v->val.f, 4); vlen = 4; break;
    case MIOT_TYPE_STRING: {
        size_t sl = strlen(v->val.str);
        if (sl > MIOT_SPEC_MAX_STR) sl = MIOT_SPEC_MAX_STR;
        if (2 + sl > cap) return 0;
        put16(out, (uint16_t)((v->type << 12) | sl));
        memcpy(out + 2, v->val.str, sl);
        return 2 + (size_t)sl;
    }
    default:
        return 0;
    }
    if (2 + vlen > cap) return 0;
    put16(out, (uint16_t)((v->type << 12) | vlen));
    memcpy(out + 2, buf, vlen);
    return 2 + (size_t)vlen;
}

/* ---------------- 组包 ---------------- */

int miot_spec_build_set_reply(uint16_t tid, const miot_spec_value_t *res, size_t count,
                              uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 5) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_SET_REPLY);
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        out[n++] = res[i].siid;
        put16(out + n, res[i].piid); n += 2;
        put16(out + n, res[i].result); n += 2;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_get_reply(uint16_t tid, const miot_spec_value_t *vals, size_t count,
                              uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 15) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_GET_REPLY);
    out[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en;
        if (vals[i].result != 0) {
            en = 0;
        } else {
            en = miot_spec_encode_value(&vals[i], enc, sizeof(enc));
            if (en == 0) return -2;
        }
        if (n + 5 + en > cap) return -3;
        out[n++] = vals[i].siid;
        put16(out + n, vals[i].piid); n += 2;
        put16(out + n, vals[i].result); n += 2;
        if (en) { memcpy(out + n, enc, en); n += en; }
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_props_changed(uint16_t tid, const miot_spec_value_t *props, size_t count,
                                  uint8_t *out, size_t cap)
{
    if (count > MIOT_SPEC_MAX_ARGS || cap < 6 + count * 15) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_PROP_CHANGED);
    out[n++] = (uint8_t)count;               /* ★ gyb 解析器要求前置 count */
    for (size_t i = 0; i < count; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en = miot_spec_encode_value(&props[i], enc, sizeof(enc));
        if (en == 0) return -2;
        if (n + 3 + en > cap) return -3;
        out[n++] = props[i].siid;
        put16(out + n, props[i].piid); n += 2;
        memcpy(out + n, enc, en); n += en;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_action_reply(uint16_t tid, uint16_t resp_code,
                                 uint8_t *out, size_t cap)
{
    if (cap < 7) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_ACTION_REPLY);
    put16(out + n, resp_code); n += 2;       /* ★ 仅 respCode，无 siid/aiid */
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_event(uint16_t tid, uint8_t siid, uint16_t eiid,
                          const miot_spec_value_t *args, size_t argc,
                          uint8_t *out, size_t cap)
{
    if (argc > MIOT_SPEC_MAX_ARGS || cap < 8 + argc * (MIOT_SPEC_MAX_STR + 4)) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_EVENT);
    out[n++] = siid;
    put16(out + n, eiid); n += 2;
    out[n++] = (uint8_t)argc;
    for (size_t i = 0; i < argc; i++) {
        uint8_t enc[MIOT_SPEC_MAX_STR + 2];
        size_t en = miot_spec_encode_value(&args[i], enc, sizeof(enc));
        if (en == 0) return -2;
        if (n + 2 + en > cap) return -3;
        put16(out + n, args[i].piid); n += 2;   /* ★ 每参数前置 piid16（ud5 要求） */
        memcpy(out + n, enc, en); n += en;
    }
    hdr_finish(out, n);
    return (int)n;
}

int miot_spec_build_version_reply(uint16_t tid, uint8_t *out, size_t cap)
{
    if (cap < 5) return -1;
    size_t n = hdr_begin(out, tid, MIOT_OP_VERSION_XCH);
    hdr_finish(out, n);
    return (int)n;
}

/* ---------------- 解包 ---------------- */

static int parse_object(const uint8_t *p, size_t remain, miot_spec_value_t *v)
{
    if (remain < 6) return -1;
    v->siid = p[0];
    v->piid = get16(p + 1);
    uint16_t tl = get16(p + 3);
    v->type = (uint8_t)(tl >> 12);
    uint8_t vlen = (uint8_t)(tl & 0x0FFF);
    if (remain < (size_t)6 + vlen) return -1;
    const uint8_t *val = p + 5;
    switch (v->type) {
    case MIOT_TYPE_BOOL:
    case MIOT_TYPE_U8:   v->val.u8  = val[0]; break;
    case MIOT_TYPE_U16:  v->val.u16 = get16(val); break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:  memcpy(&v->val.u32, val, 4); break;
    case MIOT_TYPE_FLOAT: memcpy(&v->val.f, val, 4); break;
    case MIOT_TYPE_STRING:
        if (vlen > MIOT_SPEC_MAX_STR) return -2;
        memcpy(v->val.str, val, vlen);
        v->val.str[vlen] = 0;
        break;
    default:
        return -2;
    }
    return (int)(6 + vlen);
}

int miot_spec_parse(const uint8_t *in, size_t len, miot_spec_packet_t *out)
{
    if (len < 5) return -1;
    uint16_t hdr = get16(in);
    if ((hdr >> 12) != MIOT_SPEC_VER) return -2;
    memset(out, 0, sizeof(*out));
    out->tid = get16(in + 2);
    out->op  = in[4];

    const uint8_t *p = in + 5;
    size_t remain = len - 5;

    switch (out->op) {
    case MIOT_OP_VERSION_XCH:
        return 0;
    case MIOT_OP_GET:
        if (remain < 1) return -3;
        out->argc = p[0];
        p++; remain--;
        for (int i = 0; i < out->argc && i < MIOT_SPEC_MAX_ARGS; i++) {
            if (remain < 3) return -3;
            out->args[i].siid = p[0];
            out->args[i].piid = get16(p + 1);
            p += 3; remain -= 3;
        }
        return 0;
    case MIOT_OP_SET:
        if (remain < 1) return -3;
        out->argc = p[0];
        p++; remain--;
        for (int i = 0; i < out->argc && i < MIOT_SPEC_MAX_ARGS; i++) {
            int c = parse_object(p, remain, &out->args[i]);
            if (c < 0) return -4;
            p += c; remain -= (size_t)c;
        }
        return 0;
    case MIOT_OP_ACTION:
        if (remain < 3) return -3;
        out->siid = p[0];
        out->piid = get16(p + 1);       /* aiid */
        out->argc = (remain >= 3) ? p[3] : 0;
        p += 4; remain -= 4;
        for (int i = 0; i < out->argc && i < MIOT_SPEC_MAX_ARGS; i++) {
            int c = parse_object(p, remain, &out->args[i]);
            if (c < 0) return -4;
            p += c; remain -= (size_t)c;
        }
        return 0;
    case MIOT_OP_ACTION_REPLY:
        if (remain < 2) return -3;
        out->resp_code = get16(p);
        return 0;
    default:
        return -5;   /* 事件/属性变更应答为设备→App 方向，一般无需解析 */
    }
}
