/**
 * @file scale_s400.c
 * @brief S400 体脂秤模拟器示例 —— 用 SDK 接口复现实测可用的测量上报序列
 *
 * 展示：
 *  1. 属性表注册（电量等 spec 属性自动 get/set）
 *  2. 单次测量的事件序列（8.1 实时体重 / 8.2 完整结果 / 11.1022 BLE 专用事件
 *     / 11.2+11.3 properties_changed 双保险）
 *  3. siid 扫射（私有 spec 事件 siid 未知时的探查手段）
 *
 * 事件参数格式为米家 App 实测解析通过的编码（勿改字段顺序）：
 *   8.1 实时体重:  args = "weight01g,flag"  （如 "64500,1"，flag 0=测量中 1=稳定）
 *   8.2 完整结果:  args = "0,0,0,weight01g,imp01,0,0,hrBPM,0" 九参 CSV
 */
#include "miot_ble/miot_device.h"
#include <stdio.h>
#include <string.h>

/* ---------------- 模拟器状态与属性表 ---------------- */

static uint8_t  s_battery = 92;         /* 3.1003 电量 % */
static uint8_t  s_unit   = 0;           /* 单位 0=kg 1=lb */
static uint32_t s_weight = 0;           /* 最近体重 0.01kg */
static uint32_t s_impedance = 0;        /* 最近阻抗 0.1Ω */

static void on_unit_set(const struct miot_prop *p)
{
    (void)p;
    /* 真机行为：单位切换后 LEDs 闪一下 —— 副作用写这里 */
}

static miot_prop_t s_props[] = {
    { 3, 1003, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &s_battery,  0,   0, NULL,        "battery" },
    { 3, 1004, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &s_unit,     0,   1, on_unit_set, "unit"    },
    { 3, 1005, MIOT_TYPE_U32,  MIOT_PROP_R,  4, &s_weight,   0,   0, NULL,        "weight"  },
    { 3, 1006, MIOT_TYPE_U32,  MIOT_PROP_R,  4, &s_impedance,0,   0, NULL,        "impedance"},
};

/* ---------------- 测量上报序列 ---------------- */

/** 发一个 string 参数事件（args[0] 需已填好 str） */
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

/**
 * 单次完整测量上报（推荐顺序）：
 *  ① 8.1 x N  实时体重（不稳定态 flag=0 → 稳定态 flag=1）——「双脚上秤」过程流
 *  ② 8.2      完整结果（含阻抗/心率）
 *  ③ 11.1022  BLE 专用事件（obj 0x6E16 的 spec 对应物）
 *  ④ op=4     properties_changed 推 11.2/11.3（双保险）
 */
void scale_report_measurement(miot_device_t *dev, uint32_t weight_01g,
                              uint32_t impedance_01, uint32_t ts)
{
    char s[48];

    /* ① 过程流：上秤（0,0）→ 波动中 → 稳定 */
    send_event_str(dev, 8, 1, 1, "0,0");
    snprintf(s, sizeof(s), "%u,0", (unsigned)(weight_01g * 90 / 100));
    send_event_str(dev, 8, 1, 1, s);            /* 90% 重量，测量中 */
    snprintf(s, sizeof(s), "%u,1", (unsigned)weight_01g);
    send_event_str(dev, 8, 1, 1, s);            /* 稳定 */
    send_event_str(dev, 8, 1, 1, "0,6");        /* 离秤标记（App 实测样本） */

    /* ② 完整结果：9 参数 CSV */
    snprintf(s, sizeof(s), "0,0,0,%u,%u,0,0,0,0",
             (unsigned)weight_01g, (unsigned)impedance_01);
    send_event_str(dev, 8, 2, 1, s);

    /* ③ 11.1022 BLE 专用事件：duid(1B) + 复合字段(4B) + 时间戳(4B) */
    uint32_t metrics = (weight_01g & 0x7FF) | ((impedance_01 & 0x3FFF) << 18);
    miot_spec_value_t a3[3];
    memset(a3, 0, sizeof(a3));
    a3[0].piid = 1; a3[0].type = MIOT_TYPE_U8;  a3[0].val.u8  = 1;      /* duid */
    a3[1].piid = 2; a3[1].type = MIOT_TYPE_U32; a3[1].val.u32 = metrics;
    a3[2].piid = 3; a3[2].type = MIOT_TYPE_U32; a3[2].val.u32 = ts;
    miot_device_send_event(dev, 11, 1022, a3, 3);

    /* ④ properties_changed 11.2 / 11.3 */
    miot_spec_value_t pc[2];
    memset(pc, 0, sizeof(pc));
    pc[0].siid = 11; pc[0].piid = 2; pc[0].type = MIOT_TYPE_U32; pc[0].val.u32 = metrics;
    pc[1].siid = 11; pc[1].piid = 3; pc[1].type = MIOT_TYPE_U32; pc[1].val.u32 = ts;
    miot_device_send_props_changed(dev, pc, 2);
}

/**
 * siid 扫射：私有 spec 的真实事件 siid 未知时，把事件广播到候选 siid。
 * 插件按 topic 匹配，不匹配的事件被 JS 层丢弃，无副作用。
 */
void scale_sweep(miot_device_t *dev, const char *s1, const char *s2)
{
    static const uint8_t siids[] = {2, 3, 4, 5, 6, 7, 9, 10, 12, 14, 15, 16};
    for (size_t i = 0; i < sizeof(siids); i++) {
        send_event_str(dev, siids[i], 1, 1, s1);
        send_event_str(dev, siids[i], 2, 1, s2);
    }
}

/* ---------------- 自定义 get/set 兜底（属性表外） ---------------- */

static bool scale_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 1001) {            /* 温度（演示 float 属性） */
        out->type = MIOT_TYPE_FLOAT;
        out->val.f = 25.0f;
        return true;
    }
    return false;
}

static bool scale_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user; (void)siid; (void)piid; (void)in;
    return false;
}

/* ---------------- 初始化入口 ---------------- */

miot_device_t *scale_device_create(miot_port_t *port, const uint8_t keys[40])
{
    static miot_device_t dev;
    miot_device_init(&dev, port);
    miot_device_attach(&dev, s_props, sizeof(s_props) / sizeof(s_props[0]),
                       scale_get, scale_set, NULL);
    if (keys) miot_device_set_session_keys(&dev, keys);
    return &dev;
}
