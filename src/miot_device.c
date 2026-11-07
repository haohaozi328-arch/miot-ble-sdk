/**
 * @file miot_device.c
 * @brief 设备层组合 API 实现
 */
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------- 日志 ---------------- */

static void dev_log(miot_device_t *d, int lv, const char *fmt, ...)
{
    if (!d->port || !d->port->log) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    d->port->log(d->port->user, lv, "miot", "%s", buf);
}

/* ---------------- 底层发送 ---------------- */

static void ag2_send_cb(void *user, const uint8_t *frame, size_t len)
{
    miot_device_t *d = (miot_device_t *)user;
    if (d->port && d->port->send_notify) d->port->send_notify(d->port->user, frame, len);
}

static void ag2_frame_cb(void *user, const uint8_t *k61, size_t n)
{
    miot_device_t *d = (miot_device_t *)user;
    if (!d->session_ok || n < 6) return;

    uint16_t seq = (uint16_t)(k61[0] | (k61[1] << 8));
    size_t ct_len = n - 2 - 4;                  /* 去掉 seq16 与 tag4 */
    uint8_t pt[256];
    if (ct_len > sizeof(pt)) return;
    if (miot_spec_frame_decrypt(d->keys + 16, d->keys + 36, seq, k61 + 2, ct_len, pt)) {
        dev_log(d, MIOT_LOG_WARN, "spec decrypt fail seq=%u", seq);
        return;
    }
    dev_log(d, MIOT_LOG_INFO, "spec req seq=%u pt=%uB", seq, (unsigned)ct_len);
    miot_device_dispatch_plain(d, pt, ct_len);
}

/* ---------------- 加密发送（明文 → k61 → ag2） ---------------- */

static int spec_reply(miot_device_t *d, const uint8_t *pt, size_t pt_len)
{
    if (!d->session_ok) return -1;
    if (pt_len > 300) return -2;

    uint8_t out[2 + 300 + 4];
    if (miot_spec_frame_encrypt(d->keys, d->keys + 32, d->tx_seq, pt, pt_len, out + 2)) {
        /* 最常见原因：忘记注册加密后端（miot_crypto_set_ops） */
        static int warned;
        if (!warned++) {
            dev_log(d, MIOT_LOG_ERROR,
                    "spec encrypt fail — crypto backend registered? see miot_crypto_set_ops / port/esp32");
        }
        return -3;
    }
    out[0] = (uint8_t)(d->tx_seq & 0xFF);
    out[1] = (uint8_t)(d->tx_seq >> 8);
    d->tx_seq++;
    miot_ag2_send(&d->ag2, out, pt_len + 6);
    return 0;
}

/* ---------------- 生命周期 ---------------- */

void miot_device_init(miot_device_t *d, miot_port_t *port)
{
    memset(d, 0, sizeof(*d));
    d->port = port;
    d->tid = 0;
    miot_ag2_init(&d->ag2);
    d->ag2.send = ag2_send_cb;
    d->ag2.on_frame = ag2_frame_cb;
    d->ag2.user = d;
}

void miot_device_attach(miot_device_t *d, miot_prop_t *tbl, size_t cnt,
                        miot_get_fn get, miot_set_fn set, void *user)
{
    d->props = tbl;
    d->prop_cnt = cnt;
    d->on_get = get;
    d->on_set = set;
    d->user = user;
}

void miot_device_set_session_keys(miot_device_t *d, const uint8_t keys[MIOT_KEYS_LEN])
{
    memcpy(d->keys, keys, MIOT_KEYS_LEN);
    d->tx_seq = 0;
    d->session_ok = 1;
    dev_log(d, MIOT_LOG_INFO, "spec session ready (keys set)");
}

void miot_device_session_reset(miot_device_t *d)
{
    d->session_ok = 0;
    miot_ag2_init(&d->ag2);
    d->ag2.send = ag2_send_cb;
    d->ag2.on_frame = ag2_frame_cb;
    d->ag2.user = d;
}

void miot_device_on_rx(miot_device_t *d, const uint8_t *data, size_t len)
{
    miot_ag2_rx(&d->ag2, data, len);
}

/* ---------------- 属性表驱动的 get/set ---------------- */

static void fill_value(miot_spec_value_t *v, uint8_t siid, uint16_t piid,
                       uint8_t type, uint16_t result, const void *src)
{
    memset(v, 0, sizeof(*v));
    v->siid = siid;
    v->piid = piid;
    v->type = type;
    v->result = result;
    if (!src) return;
    switch (type) {
    case MIOT_TYPE_BOOL:
    case MIOT_TYPE_U8:    v->val.u8 = *(const uint8_t *)src; break;
    case MIOT_TYPE_U16:   v->val.u16 = *(const uint16_t *)src; break;
    case MIOT_TYPE_U32:
    case MIOT_TYPE_I32:   v->val.u32 = *(const uint32_t *)src; break;
    case MIOT_TYPE_FLOAT: v->val.f = *(const float *)src; break;
    default: break;
    }
}

static void handle_get(miot_device_t *d, const miot_spec_packet_t *pkt)
{
    miot_spec_value_t vals[MIOT_SPEC_MAX_ARGS];
    for (int i = 0; i < pkt->argc; i++) {
        uint8_t siid = pkt->args[i].siid;
        uint16_t piid = pkt->args[i].piid;
        memset(&vals[i], 0, sizeof(vals[i]));

        miot_prop_t *mp = miot_props_find(d->props, d->prop_cnt, siid, piid);
        if (mp) {
            /* 从本地存储读出再按声明类型编码 */
            int32_t iv = (mp->size == 1) ? (int32_t)(*(uint8_t *)mp->ptr) : *(int32_t *)mp->ptr;
            fill_value(&vals[i], siid, piid, mp->type, 0, &iv);
            continue;
        }
        if (d->on_get && d->on_get(d->user, siid, piid, &vals[i])) {
            vals[i].siid = siid;
            vals[i].piid = piid;
            vals[i].result = 0;
            continue;
        }
        fill_value(&vals[i], siid, piid, 0, 1, NULL);   /* result=1 不认识 */
    }
    uint8_t pt[192];
    /* 把 App 实际请求的 siid.piid 打出来：页面缺读数时能一眼看出
     * 是「App 要的属性没注册」还是「注册了但值没上报」。 */
    char trace[160];
    int tn = 0;
    trace[0] = 0;
    for (int i = 0; i < pkt->argc; i++) {
        if (tn < (int)sizeof(trace) - 20) {
            tn += snprintf(trace + tn, sizeof(trace) - (size_t)tn, "%s%u.%u=%s",
                           i ? " " : "", pkt->args[i].siid, pkt->args[i].piid,
                           vals[i].result ? "ERR" : "ok");
        }
    }
    dev_log(d, MIOT_LOG_INFO, "spec GET %s", trace);
    int n = miot_spec_build_get_reply(pkt->tid, vals, pkt->argc, pt, sizeof(pt));
    if (n > 0) spec_reply(d, pt, (size_t)n);
}

static void handle_set(miot_device_t *d, const miot_spec_packet_t *pkt)
{
    miot_spec_value_t res[MIOT_SPEC_MAX_ARGS];
    for (int i = 0; i < pkt->argc; i++) {
        const miot_spec_value_t *in = &pkt->args[i];
        uint16_t result = 1;
        miot_prop_t *mp = miot_props_find(d->props, d->prop_cnt, in->siid, in->piid);
        if (mp) {
            uint8_t raw[8];
            size_t rl = 0;
            switch (in->type) {
            case MIOT_TYPE_BOOL:
            case MIOT_TYPE_U8:    raw[0] = in->val.u8; rl = 1; break;
            case MIOT_TYPE_U16:   raw[0] = (uint8_t)in->val.u16; raw[1] = (uint8_t)(in->val.u16 >> 8); rl = 2; break;
            case MIOT_TYPE_U32:
            case MIOT_TYPE_I32:   memcpy(raw, &in->val.u32, 4); rl = 4; break;
            case MIOT_TYPE_FLOAT: memcpy(raw, &in->val.f, 4); rl = 4; break;
            default: break;
            }
            if (rl && miot_props_apply(mp, in->type, raw, (uint8_t)rl)) result = 0;
        } else if (d->on_set && d->on_set(d->user, in->siid, in->piid, in)) {
            result = 0;
        }
        memset(&res[i], 0, sizeof(res[i]));
        res[i].siid = in->siid;
        res[i].piid = in->piid;
        res[i].result = result;
    }
    uint8_t pt[64];
    int n = miot_spec_build_set_reply(pkt->tid, res, pkt->argc, pt, sizeof(pt));
    if (n > 0) spec_reply(d, pt, (size_t)n);
}

/* ---------------- 明文请求分派 ---------------- */

void miot_device_dispatch_plain(miot_device_t *d, const uint8_t *pt, size_t len)
{
    miot_spec_packet_t pkt;
    int rc = miot_spec_parse(pt, len, &pkt);
    if (rc != 0) {
        dev_log(d, MIOT_LOG_WARN, "spec parse fail rc=%d len=%uB", rc, (unsigned)len);
        return;
    }
    switch (pkt.op) {
    case MIOT_OP_VERSION_XCH: {
        uint8_t r[5];
        int n = miot_spec_build_version_reply(pkt.tid, r, sizeof(r));
        if (n > 0) spec_reply(d, r, (size_t)n);
        dev_log(d, MIOT_LOG_INFO, "spec version exchange done (dev ver=2)");
        break;
    }
    case MIOT_OP_ACTION: {
        /* 应答格式仅 [op=6][respCode16=0]，带 siid/aiid 会被 App 解析为失败并无限重发 */
        uint8_t r[7];
        int n = miot_spec_build_action_reply(pkt.tid, 0, r, sizeof(r));
        if (n > 0) spec_reply(d, r, (size_t)n);
        dev_log(d, MIOT_LOG_WARN, "spec ACTION siid=%u aiid=%u -> respCode=0",
                pkt.siid, pkt.piid);
        break;
    }
    case MIOT_OP_GET:
        handle_get(d, &pkt);
        break;
    case MIOT_OP_SET:
        handle_set(d, &pkt);
        break;
    default:
        dev_log(d, MIOT_LOG_WARN, "spec op=0x%02x unsupported", pkt.op);
        break;
    }
}

/* ---------------- 主动上报 ---------------- */

int miot_device_send_event(miot_device_t *d, uint8_t siid, uint16_t eiid,
                           const miot_spec_value_t *args, size_t argc)
{
    if (!d->session_ok) return -1;
    uint8_t pt[128];
    uint16_t tid = d->tid++;
    int n = miot_spec_build_event(tid, siid, eiid, args, argc, pt, sizeof(pt));
    if (n <= 0) return n;
    spec_reply(d, pt, (size_t)n);
    return n;
}

int miot_device_send_props_changed(miot_device_t *d,
                                   const miot_spec_value_t *props, size_t count)
{
    if (!d->session_ok) return -1;
    uint8_t pt[128];
    uint16_t tid = d->tid++;
    int n = miot_spec_build_props_changed(tid, props, count, pt, sizeof(pt));
    if (n <= 0) return n;
    spec_reply(d, pt, (size_t)n);
    return n;
}

int miot_device_send_action_reply(miot_device_t *d, uint16_t tid, uint16_t resp_code)
{
    uint8_t r[7];
    int n = miot_spec_build_action_reply(tid, resp_code, r, sizeof(r));
    if (n <= 0) return n;
    return spec_reply(d, r, (size_t)n);
}

int miot_device_send_version_reply(miot_device_t *d, uint16_t tid)
{
    uint8_t r[5];
    int n = miot_spec_build_version_reply(tid, r, sizeof(r));
    if (n <= 0) return n;
    return spec_reply(d, r, (size_t)n);
}
