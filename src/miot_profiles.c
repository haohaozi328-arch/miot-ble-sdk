/**
 * @file miot_profiles.c
 * @brief 已验证设备档案实现
 *
 * 每个档案的 spec 表 / 事件格式都来自对应真机壳与米家 App 的实测互通，
 * 帧格式勿改（详见 docs/protocol.md）。
 */
#include "miot_ble/miot_profiles.h"
#include <stdio.h>
#include <string.h>

/* ================= 公共：测量事件 string 参数辅助 ================= */

static int send_event_str(miot_device_t *dev, uint8_t siid, uint16_t eiid,
                          uint16_t piid, const char *s)
{
    miot_spec_value_t a;
    memset(&a, 0, sizeof(a));
    a.piid = piid;
    a.type = MIOT_TYPE_STRING;
    snprintf(a.val.str, sizeof(a.val.str), "%s", s);
    return miot_device_send_event(dev, siid, eiid, &a, 1);
}

/* ================= 1. 体脂秤 S400 ================= */

static uint8_t  s400_battery = 92;      /* 3.1003 电量 % */
static uint8_t  s400_unit    = 0;       /* 0=kg 1=lb */
static uint32_t s400_weight  = 0;       /* 最近体重 0.01kg */
static uint32_t s400_imp     = 0;      /* 最近阻抗 0.1Ω */

static miot_prop_t s400_props[] = {
    { 3, 1003, MIOT_TYPE_U8,  MIOT_PROP_R,  1, &s400_battery, 0, 0,   NULL, "battery"   },
    { 3, 1004, MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &s400_unit,    0, 1,   NULL, "unit"      },
    { 3, 1005, MIOT_TYPE_U32, MIOT_PROP_R,  4, &s400_weight,  0, 0,   NULL, "weight"    },
    { 3, 1006, MIOT_TYPE_U32, MIOT_PROP_R,  4, &s400_imp,     0, 0,   NULL, "impedance" },
};

static bool s400_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 1001) { out->type = MIOT_TYPE_FLOAT; out->val.f = 0.0f; return true; }
    return false;
}

static bool s400_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void scale_s400_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, s400_props, sizeof(s400_props) / sizeof(s400_props[0]),
                       s400_get, s400_set, NULL);
    if (tbl) *tbl = s400_props;
    if (cnt) *cnt = sizeof(s400_props) / sizeof(s400_props[0]);
}

void scale_s400_report(miot_device_t *dev, uint32_t weight_01g,
                       uint32_t impedance_01, uint32_t ts)
{
    char s[48];

    /* ① 过程流（「双脚上秤」）：上秤 → 波动中 → 稳定 → 离秤标记 */
    send_event_str(dev, 8, 1, 1, "0,0");
    snprintf(s, sizeof(s), "%u,0", (unsigned)(weight_01g * 90 / 100));
    send_event_str(dev, 8, 1, 1, s);
    snprintf(s, sizeof(s), "%u,1", (unsigned)weight_01g);
    send_event_str(dev, 8, 1, 1, s);
    send_event_str(dev, 8, 1, 1, "0,6");

    /* ② 完整结果：九参 CSV（App 实测解析通过） */
    snprintf(s, sizeof(s), "0,0,0,%u,%u,0,0,0,0",
             (unsigned)weight_01g, (unsigned)impedance_01);
    send_event_str(dev, 8, 2, 1, s);

    /* ③ 11.1022 BLE 专用事件：duid(1B) + 复合字段(4B) + 时间戳(4B) */
    uint32_t metrics = (weight_01g & 0x7FF) | ((impedance_01 & 0x3FFF) << 18);
    miot_spec_value_t a3[3];
    memset(a3, 0, sizeof(a3));
    a3[0].piid = 1; a3[0].type = MIOT_TYPE_U8;  a3[0].val.u8  = 1;
    a3[1].piid = 2; a3[1].type = MIOT_TYPE_U32; a3[1].val.u32 = metrics;
    a3[2].piid = 3; a3[2].type = MIOT_TYPE_U32; a3[2].val.u32 = ts;
    miot_device_send_event(dev, 11, 1022, a3, 3);

    /* ④ properties_changed 11.2 / 11.3（双保险） */
    miot_spec_value_t pc[2];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 11; pc[0].piid = 2; pc[0].type = MIOT_TYPE_U32; pc[0].val.u32 = metrics;
    pc[1].siid = 11; pc[1].piid = 3; pc[1].type = MIOT_TYPE_U32; pc[1].val.u32 = ts;
    miot_device_send_props_changed(dev, pc, 2);
}

void scale_s400_sweep(miot_device_t *dev, const char *s1, const char *s2)
{
    static const uint8_t siids[] = { 2, 3, 4, 5, 6, 7, 9, 10, 12, 14, 15, 16 };
    for (size_t i = 0; i < sizeof(siids); i++) {
        send_event_str(dev, siids[i], 1, 1, s1);
        send_event_str(dev, siids[i], 2, 1, s2);
    }
}

/* ================= 2. 温湿度计（cgdk3 / t9 双 spec） =================
 * ★同一属性在两个 spec 里 siid 不同（cgdk3: 2.x；miaomiaoce-t9: 3.x），
 * 属性表两份都注册，插件用哪个 spec 都能读。 */

static float    th_temp  = 25.0f;    /* ℃ */
static uint8_t  th_hum_t9 = 56;      /* t9 spec 湿度是 uint8 % */
static uint8_t  th_battery = 88;

static miot_prop_t th_props[] = {
    /* cgdk3：2.1001 温度 float / 2.1008 湿度 float / 2.1003 电量 */
    { 2, 1003, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_battery, 0, 0, NULL, "battery" },
    /* t9：3.1001 温度 float / 3.1002 湿度 uint8 / 3.1003 电量 */
    { 3, 1003, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_battery, 0, 0, NULL, "battery" },
    { 3, 1002, MIOT_TYPE_U8, MIOT_PROP_R, 1, &th_hum_t9,  0, 0, NULL, "humidity" },
};

static bool th_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if ((siid == 2 && piid == 1001) || (siid == 3 && piid == 1001)) {
        out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true;   /* 温度 float */
    }
    if (siid == 2 && piid == 1008) { out->type = MIOT_TYPE_FLOAT; out->val.f = (float)th_hum_t9; return true; }
    if (siid == 2 && piid == 1002) { out->type = MIOT_TYPE_FLOAT; out->val.f = (float)th_hum_t9; return true; } /* dk1 兼容 */
    if (siid == 2 && piid == 1003) { out->type = MIOT_TYPE_U8; out->val.u8 = th_battery; return true; }
    if (siid == 2 && piid == 1)    { out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true; }
    if (siid == 2 && piid == 2)    { out->type = MIOT_TYPE_FLOAT; out->val.f = (float)th_hum_t9; return true; }
    if (siid == 3 && piid == 1)    { out->type = MIOT_TYPE_FLOAT; out->val.f = th_temp; return true; }
    return false;
}

static bool th_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void thermometer_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, th_props, sizeof(th_props) / sizeof(th_props[0]),
                       th_get, th_set, NULL);
    if (tbl) *tbl = th_props;
    if (cnt) *cnt = sizeof(th_props) / sizeof(th_props[0]);
}

void thermometer_report(miot_device_t *dev, float temp_c, float hum_pct, uint32_t ts)
{
    th_temp = temp_c;
    th_hum_t9 = (uint8_t)(hum_pct + 0.5f);

    /* properties_changed：温度 float（2.1001）+ 湿度 uint8（3.1002） */
    miot_spec_value_t pc[2];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 2; pc[0].piid = 1001; pc[0].type = MIOT_TYPE_FLOAT; pc[0].val.f = temp_c;
    pc[1].siid = 3; pc[1].piid = 1002; pc[1].type = MIOT_TYPE_U8;   pc[1].val.u8 = th_hum_t9;
    miot_device_send_props_changed(dev, pc, 2);
    (void)ts;
}

/* ================= 3. 开关型（夜灯 / 驱蚊器） ================= */

static uint8_t sw_on = 0;

static void sw_on_set(const struct miot_prop *p)
{
    (void)p;
    /* 真机行为：收到 set 后立刻回报一次 properties_changed(switch 0x1012 对应
     * spec 属性)，页面开关才停在正确位置 —— 副作用回调里置上报标志 */
}

static miot_prop_t sw_props[] = {
    { 2, 1012, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &sw_on, 0, 1, sw_on_set, "switch" },
};

static bool sw_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 1012) { out->type = MIOT_TYPE_BOOL; out->val.u8 = sw_on; return true; }
    return false;
}

static bool sw_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;   /* 属性表已覆盖 */
}

void switch_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, sw_props, sizeof(sw_props) / sizeof(sw_props[0]),
                       sw_get, sw_set, NULL);
    if (tbl) *tbl = sw_props;
    if (cnt) *cnt = sizeof(sw_props) / sizeof(sw_props[0]);
}

/* ================= 4. 无线按钮（XMWXKG01LM） ================= */

static uint16_t btn_cfg = 1;    /* 3.1020 按钮配置：插件下发后需读回 */

static miot_prop_t btn_props[] = {
    { 3, 1020, MIOT_TYPE_U16, MIOT_PROP_RW, 2, &btn_cfg, 0, 0, NULL, "btn-config" },
    { 3, 1003, MIOT_TYPE_U8,  MIOT_PROP_R,  1, &th_battery, 0, 0, NULL, "battery" },
};

static bool btn_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 3 && piid == 1020) { out->type = MIOT_TYPE_U16; out->val.u16 = btn_cfg; return true; }
    return false;
}

static bool btn_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

void button_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, btn_props, sizeof(btn_props) / sizeof(btn_props[0]),
                       btn_get, btn_set, NULL);
    if (tbl) *tbl = btn_props;
    if (cnt) *cnt = sizeof(btn_props) / sizeof(btn_props[0]);
}

/* ================= 5. 夜灯2（MUE4094RT） =================
 * v54 固件完整打通：2.1 亮度%(1-100) / 2.2 延时ms(5000-60000) /
 * 2.3 开关 / 3.1 电量。on_set 副作用 = 存 NVS + 重设 LED。 */

static uint8_t  nl_bright  = 50;
static uint32_t nl_dur     = 10000;
static uint8_t  nl_power   = 0;
static uint8_t  nl_battery = 80;

static void nl_on_any(const struct miot_prop *p) { (void)p; /* nl_save(); led_set(); */ }

static miot_prop_t nl_props[] = {
    { 2, 1, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &nl_bright,  1,    100,   nl_on_any, "bright"  },
    { 2, 2, MIOT_TYPE_U32,  MIOT_PROP_RW, 4, &nl_dur,     5000, 60000, nl_on_any, "dur"     },
    { 2, 3, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &nl_power,   0,    0,     nl_on_any, "power"   },
    { 3, 1, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &nl_battery, 0,    0,     NULL,      "battery" },
};

void night_light_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, nl_props, sizeof(nl_props) / sizeof(nl_props[0]),
                       NULL, NULL, NULL);
    if (tbl) *tbl = nl_props;
    if (cnt) *cnt = sizeof(nl_props) / sizeof(nl_props[0]);
}

/* ================= 6. 驱蚊器智能版（WX08ZM，PID 0x040A） =================
 * 2.1 开关（唯一可写）/ 3.1 电量。真机行为：set 后立刻回报 0x1012。 */

static uint8_t rp_on      = 0;
static uint8_t rp_battery = 65;

static void rp_on_power(const struct miot_prop *p)
{
    (void)p;    /* 真机：置 pending 标志，下一拍发 properties_changed(2.1) */
}

static miot_prop_t rp_props[] = {
    { 2, 1, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &rp_on,      0, 0, rp_on_power, "power"   },
    { 3, 1, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &rp_battery, 0, 0, NULL,        "battery" },
};

void repeller_init(miot_device_t *dev, miot_port_t *port,
                   miot_prop_t **tbl, size_t *cnt)
{
    miot_device_init(dev, port);
    miot_device_attach(dev, rp_props, sizeof(rp_props) / sizeof(rp_props[0]),
                       NULL, NULL, NULL);
    if (tbl) *tbl = rp_props;
    if (cnt) *cnt = sizeof(rp_props) / sizeof(rp_props[0]);
}

/* ================= 通用：auth ↔ device 标准接线 ================= */

void profiles_wire_auth(miot_auth_t *auth, miot_device_t *dev,
                        int (*notify_0010)(void *, const uint8_t *, size_t),
                        int (*notify_0019)(void *, const uint8_t *, size_t),
                        void *user)
{
    static miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.user = user;
    ap.notify_0010 = notify_0010;
    ap.notify_0019 = notify_0019;
    miot_auth_init(auth, &ap);
    (void)dev;
}
