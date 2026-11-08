/**
 * @file app_main.c
 * @brief 统一多型号固件 —— 用 idf.py menuconfig 选一个型号，编译烧录即可模拟该设备
 *
 * 为什么要有这个工程：
 *   复核「纯净 App 能否添加」这张矩阵（docs/clean-app-matrix.md）时，每换一个
 *   型号就要一份工程，改 PID、改属性表、改上报周期，散在十几份 app_main.c 里
 *   极易走偏（驱蚊器 4.1 开关被当成 2.1 药量那种错就是这么来的）。
 *   这里把「型号差异」全部收敛到 Kconfig + 两张常量表，链路代码只有一份。
 *
 * 链路（与 esp32/thermo_gatt 完全一致，已被纯净 App 实测验证）：
 *   广播        未绑定 hello 形态（12B，带 Capability、registered=0）
 *   绑定        0xA2 → ECDH → 云下发 did(92B) → 回 0x11
 *   会话        0x24 → appRand/devRand → 0x21
 *   数据        spec 属性帧推 0x0010（并同发 0x001B，覆盖优化连接模型）
 *
 * 型号切换注意：
 *   NVS 里存的是「上一次绑定的身份 + 该身份属于哪个 PID」，启动时若 PID 与当前
 *   型号不符会自动整片擦除，不需要手动 erase-flash。
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_profiles.h"

#include "gatt_miot.h"

void miot_port_esp32_crypto_init(void);

/* ==================== 型号表（运行期选择） ====================
 * 11 个档案全部编进同一份固件，序号持久化在 NVS；串口 `dev N` 随时切换。
 *
 * 为什么不在编译期选：每换一次型号要全量重编约 7 分钟（sdkconfig.h 变化会
 * 触发整个 IDF 重编），11 台就是一小时以上，而且现场换型号必须重新烧录。
 * 运行期切换后，一次烧录可跑完全部档案，切换只花一次重启（~2s）。
 *
 * kind：0 = spec 连接型（建 GATT、推属性帧）  1 = 广播型（只发 MiBeacon）
 * beacon_kind：spec 型里 1 = 温湿度计那套 temp/hum/batt 三帧轮播
 * slots：广播帧轮播槽位数，必须与 build_objs() / slot_names 的 case 对齐
 * init：spec 型的属性表初始化；广播型为 NULL（不注册属性表）
 */
typedef struct {
    const char  *tag;
    const char  *name;
    const char  *label;
    uint16_t     pid;
    uint8_t      kind;
    uint8_t      beacon_kind;
    uint8_t      slots;
    const char *const *slot_names;
    void       (*init)(miot_device_t *, miot_port_t *, miot_prop_t **, size_t *);
} dev_desc_t;

static const char *const sn_batt[]   = { "battery" };
static const char *const sn_thb[]    = { "temperature", "humidity", "battery" };
static const char *const sn_hcho[]   = { "hcho", "temperature", "humidity", "battery" };
static const char *const sn_door[]   = { "door", "light", "battery" };
static const char *const sn_motion[] = { "motion", "no_motion", "button", "battery" };
static const char *const sn_leak[]   = { "leak", "battery" };

static const dev_desc_t k_devs[] = {
    { "NIGHTL",   "MUE4094RT",  "夜灯2 (MUE4094RT)",          0x07F6, 0, 0, 1, sn_batt,   night_light_init },
    { "THERMO",   "LYWSD03MMC", "蓝牙温湿度计2 (LYWSD03MMC)",  0x055B, 0, 1, 3, sn_thb,    thermometer_init },
    { "SCALE",    "MJJ",        "米家体脂秤 S400",             0x30D9, 0, 0, 1, sn_batt,   scale_s400_init  },
    { "BTN2",     "XMWXKG01YL", "无线开关 双键 (XMWXKG01YL)",  0x1949, 0, 0, 1, sn_batt,   button_dev_init  },
    { "BTN1",     "XMWXKG01CM", "无线开关 单键 (XMWXKG01CM)",  0x2387, 0, 0, 1, sn_batt,   button_dev_init  },
    { "REPELLER", "MJ_MR_V1",   "驱蚊器智能版 (WX08ZM)",       0x040A, 0, 0, 1, sn_batt,   repeller_init    },
    { "HCHO",     "MJ_HCHO",    "甲醛检测仪 (JQJCY01YM)",      0x02DF, 1, 0, 4, sn_hcho,   NULL },
    { "DOOR",     "MJ_DOOR",    "门窗传感器2 (MCCGQ02HL)",     0x098B, 1, 0, 3, sn_door,   NULL },
    { "MOTION",   "MJ_MOTION",  "人体传感器2 (RTCGQ02LM)",     0x0A8D, 1, 0, 4, sn_motion, NULL },
    { "LEAK",     "MJ_LEAK",    "水浸传感器 (SJWS01LM)",       0x0863, 1, 0, 2, sn_leak,   NULL },
    { "T3M",      "MJ_T3M",     "温湿度计3 mini (MJWSD06MMC)", 0x55B5, 1, 0, 3, sn_thb,    NULL },
};

#define DEV_COUNT ((int)(sizeof(k_devs) / sizeof(k_devs[0])))

static int               s_sel;     /* 当前档案序号 */
static const dev_desc_t *D;         /* 当前档案 */
static const char       *TAG = "MIOT";

#define DEV_PID         (D->pid)
#define DEV_LABEL       (D->label)
#define DEV_KIND        (D->kind)
#define DEV_BEACON_KIND (D->beacon_kind)
#define DEV_SLOTS       (D->slots)

#define ADV_PERIOD_MS  1500
#define SPEC_PERIOD_MS 5000
#define NVS_NS         "miot"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"
#define NVS_KEY_PID    "devpid"
#define NVS_KEY_SEL    "devsel"

/* ---------------- 模拟读数 ----------------
 * 值必须缓慢波动：网关对「没变化」的属性只上报一次，写死会让云端曲线永远空白。 */
static uint32_t s_fcnt;
static int16_t  v_temp   = 256;    /* 0.1℃ */
static uint16_t v_hum    = 580;    /* 0.1% */
static uint8_t  v_batt   = 88;     /* %    */
static uint8_t  v_margin = 76;     /* 驱蚊片余量 %（驱蚊器档案用） */
static uint16_t v_hcho   = 8;      /* 0.01 mg/m³（甲醛档案用） */

static void sim_tick(void)
{
    v_temp = 250 + (int16_t)((s_fcnt >> 3) % 15);
    v_hum  = 575 + (uint16_t)((s_fcnt >> 4) % 11);
}

static const char *slot_name(int i)
{
    if (i < 0 || i >= DEV_SLOTS) return "?";
    return D->slot_names[i];
}

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;
static miot_prop_t  *s_tbl;
static size_t        s_tbl_cnt;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static int      s_idx;
static uint8_t  s_flen;
static int      s_logged_in;
static uint32_t s_login_ms;         /* 会话建立时刻，用来排除握手期误触发 */
static int      s_was_bound;
static volatile int s_weigh_req;    /* BOOT 短按置位，主循环消费 */
/* 称重序列的分帧状态机（见 scale_s400_report_step：一帧一步，必须节流） */
static int      s_wstep = -1;
static uint32_t s_wnext, s_ww, s_wimp, s_wts;
/* 每步之后到下一步的间隔：8.1 过程流 250ms 一跳，结果前留 400ms，
 * 1022 / 属性变更各留 900ms —— App 的 ag2 窗口同一毫秒只吃一帧。 */
static const uint16_t k_wgap[SCALE_S400_STEPS] = { 250, 250, 250, 400, 900, 900, 0 };

/* 「点击测量」这条路的慢速节奏。为什么不能立刻发：App 点完按钮要 4~5s 才把
 * 界面切到「请上秤」，这段时间发过去的事件全被丢在空窗里（23:31 实测：点击
 * 后 20ms 就推完整序列，7 帧全部 ACK，页面一点反应都没有）。所以先等 2.5s
 * 再开口，并把每帧间隔拉长到 1.5s，让数据持续落在「请上秤」窗口内。 */
static const uint16_t k_wgap_slow[SCALE_S400_STEPS] = { 1800, 1800, 1200, 900, 900, 300, 0 };
static uint16_t s_wdelay;                       /* 开口前的静默时间 */
static const uint16_t *s_wgap_p = k_wgap;       /* 当前使用的节奏表 */
static volatile int s_click_pending;            /* 本次称重是「点击」触发的 */

/* 每次按 BOOT 轮流用一组不同的体重/阻抗，方便在 App 历史里看到多条独立记录。
 * 单位：体重=克（profile 内部换算成 kg×10），阻抗=0.1Ω。 */
static const uint32_t k_wcycle[][2] = {
    { 64500u, 500u },   /* 64.5kg */
    { 63800u, 460u },   /* 63.8kg */
    { 65200u, 540u },   /* 65.2kg */
};
static int s_widx;

/* App「点击测量」的识别：插件点击后不发任何“开始称重”指令，唯一痕迹是
 * 连发两次属性请求（实测间隔稳定 6.0~6.1s，而周期心跳是 30~36s）。 */
static uint32_t s_last_app_req;
static uint32_t s_last_weigh;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* gatt_miot.c 里的弱符号，这里给强实现。
 *
 * 为什么需要它：米家插件的「测量」按钮**不会**给设备发“开始称重”指令 ——
 * 官方秤的测量起点是人踩上去（称重传感器中断），App 只负责连接、订阅、
 * 把界面切到“请上秤”，然后等 4~5 秒，等不到数据就退回“重新上秤”。
 * 所以设备端想“知道用户点了”，只能从它请求的节奏去猜：
 *   点击测量 → 连发两次属性请求（间隔稳定 6s）
 *   平时心跳 → 30~36s 一次
 * 命中「两次间隔 ≤8s」就替用户称一次，这样点不点按钮都能出数；
 * BOOT 键那条路保持独立可用（等价于真机上“人踩上去”）。
 * 15s 冷却防止反复触发。
 *
 * ★点击触发必须慢半拍：App 从点击到把界面切到「请上秤」有 4~5s 的过渡，
 *   这段窗口里发过去的数据会被直接丢掉（23:31 实测 7 帧全 ACK、页面无反应）。
 *   所以这里只置 s_click_pending，由主循环套慢速节奏表（先等 2.5s）。 */
void miot_demo_app_req_hook(void)
{
    uint32_t t = now_ms();
    uint32_t gap = s_last_app_req ? (t - s_last_app_req) : 0;

    /* 握手阶段会连打 3 次请求（间隔 120ms / 31ms），必须排除，否则一进页面
     * 就误判成「点击测量」自己称一次（23:52 实测）。真正的点击是稳定 6s 间隔。 */
    if (gap >= 3000u && gap <= 9000u &&
        s_login_ms && (t - s_login_ms) > 6000u &&
        s_sel == 2 && s_auth.logged_in && s_wstep < 0 &&
        (s_last_weigh == 0 || (t - s_last_weigh) > 15000u)) {
        ESP_LOGW(TAG, "App 点击测量（请求间隔 %ums）-> 2.5s 后开始慢速上秤流",
                 (unsigned)gap);
        s_click_pending = 1;
        s_weigh_req = 1;
    }
    s_last_app_req = t;
}

/* App 点「测量」时唯一可观测的信号：ACTION siid=10 aiid=1（实测 23:57 成对出现，
 * 一次在页面打开，一次在点测量）。收到就替用户称一次，等价于真机上「人踩上去」。
 * ★在 NimBLE 任务里回调，只置标志，真正的发送交给主循环。 */
void miot_demo_app_action_hook(uint8_t siid, uint16_t aiid)
{
    uint32_t t = now_ms();
    if (siid != 10 || aiid != 1) return;
    if (s_sel != 2 || !s_auth.logged_in) return;
    if (s_wstep >= 0 || s_weigh_req) return;                    /* 正在称，别叠加 */
    if (s_last_weigh && (t - s_last_weigh) < 8000u) return;     /* 冷却，防连击 */
    ESP_LOGW(TAG, "App ACTION 10.1 = 用户点了「测量」-> 4s 后开始慢速上秤流");
    s_click_pending = 1;
    s_weigh_req = 1;
}

/* 体脂秤记录时间戳：板上没有 RTC，也没连 WiFi，只能自己造一个 wall clock。
 * 做法是拿**编译时刻**（__DATE__/__TIME__）当锚点，再加运行时长 —— 误差等于
 * 「编译 → 上电」的间隔，实测够用；填 0 会被云端判非法（报告页 1970 + 「服务器异常」）。
 * ★编译容器 espressif/idf:v6.0 的 TZ 是 UTC，__TIME__ 已经是 UTC，不要再减 8 小时。 */
static uint32_t build_epoch(void)
{
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;             /* "Nov  8 2026"（日 <10 时是空格填充） */
    const char *t = __TIME__;             /* "14:57:03" */
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    int day = (d[4] == ' ') ? (d[5] - '0') : ((d[4] - '0') * 10 + (d[5] - '0'));
    /* "Nov  8 2026"：年份在 [7..10]，四位；只取两位会算成 2020（23:02 实测踩过） */
    int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    int mon = 1;
    for (int i = 0; i < 12; i++) {
        if (!strncmp(d, mons + i * 3, 3)) { mon = i + 1; break; }
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[3] - '0') * 10 + (t[4] - '0');
    int ss = (t[6] - '0') * 10 + (t[7] - '0');

    long days = (long)(year - 1970) * 365 + (year - 1969) / 4
              - (year - 1901) / 100 + (year - 1601) / 400
              + cum[mon - 1] + day - 1;
    if (mon > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
    return (uint32_t)(days * 86400L + hh * 3600 + mm * 60 + ss);
}

static uint32_t scale_now(void) { return build_epoch() + now_ms() / 1000; }

static uint32_t port_time_now(void *user)
{
    (void)user;
    return now_ms();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------------- 身份持久化 ---------------- */
static void id_save(void *user, const miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_ID, id, sizeof(*id));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "identity saved (%s), did[0..3]=%02X %02X %02X %02X",
             err == ESP_OK ? "ok" : "FAIL", id->did[0], id->did[1], id->did[2], id->did[3]);
}

static bool id_load(void *user, miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*id);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_ID, id, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*id)) return false;
    ESP_LOGI(TAG, "identity loaded, did[0..3]=%02X %02X %02X %02X",
             id->did[0], id->did[1], id->did[2], id->did[3]);
    return true;
}

static void fcnt_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { s_fcnt = 1; return; }
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_FCNT, &v) != ESP_OK) v = 1;
    nvs_close(h);
    s_fcnt = v;
}

static void fcnt_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_FCNT, s_fcnt);
    nvs_commit(h);
    nvs_close(h);
}

/** 型号切换守卫：NVS 里的身份属于别的 PID 时自动整片擦除。
 *  否则换型号后 App 会拿旧 did 当「已绑定设备」，永远进不了添加流程。
 *  devpid 没记录过但存在身份 blob（旧固件留下）同样视为不匹配。 */
static void ensure_pid_match(void)
{
    uint16_t stored = 0;
    bool has_pid = false, has_ident = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        has_pid = (nvs_get_u16(h, NVS_KEY_PID, &stored) == ESP_OK);
        size_t sz = 0;
        has_ident = (nvs_get_blob(h, NVS_KEY_ID, NULL, &sz) == ESP_OK && sz > 0);
        nvs_close(h);
    }
    if ((has_pid && stored != DEV_PID) || (!has_pid && has_ident)) {
        ESP_LOGW(TAG, "NVS 身份与本型号不符 (devpid %s, 存在身份=%d, 当前 PID=0x%04X)"
                      " -> 擦除 NVS", has_pid ? "已记录且不同" : "未记录", has_ident, DEV_PID);
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, NVS_KEY_PID, DEV_PID);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- 日志与随机数 ---------------- */
static void port_log(void *user, int level, const char *tag, const char *fmt, ...)
{
    (void)user;
    va_list ap;
    va_start(ap, fmt);
    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (level == MIOT_LOG_ERROR)      ESP_LOGE(tag, "%s", buf);
    else if (level == MIOT_LOG_WARN)  ESP_LOGW(tag, "%s", buf);
    else                              ESP_LOGI(tag, "%s", buf);
}

static int port_random(void *user, uint8_t *out, size_t len)
{
    (void)user;
    esp_fill_random(out, len);
    return 0;
}

/* ---------------- 广播 ----------------
 * 广播型档案（kind=1）的对象表：每槽一帧，槽位数 = slots。
 * 单帧对象明文必须 ≤8B（帧 = 18B 固定头 + 对象 ≤ 26B 上限）。 */
static size_t build_objs_beacon(int idx, uint8_t *objs, size_t cap)
{
    (void)cap;
    switch (s_sel) {
    case 6:     /* 甲醛：0x1010 / 0x1004 / 0x1006 / 0x100A */
        switch (idx) {
        case 0: return miot_beacon_obj_u16(objs, MB_OBJ_HCHO_U16,
                                           (uint16_t)(v_hcho + (s_fcnt % 5)));
        case 1: return miot_beacon_obj_i16(objs, MB_OBJ_TEMP_I16, v_temp);
        case 2: return miot_beacon_obj_u16(objs, MB_OBJ_HUM_U16, v_hum);
        default: return miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
        }

    case 7: {   /* 门窗：0x1019 0=开 1=关 2=超时未关 3=复位 / 0x1018 光照 / 电量 */
        switch (idx) {
        case 0: {
            uint8_t v[1] = { (uint8_t)((s_fcnt / 10) & 3) };
            return miot_beacon_obj_bytes(objs, MB_OBJ_DOOR, v, 1);
        }
        case 1: {
            uint8_t v[1] = { (uint8_t)((s_fcnt % 2) ? 1 : 0) };
            return miot_beacon_obj_bytes(objs, MB_OBJ_LIGHT, v, 1);
        }
        default: return miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
        }
    }

    case 8: {   /* 人体：0x000F 移动+光照 / 0x1017 无人时长 / 0x1001 按键 / 电量 */
        switch (idx) {
        case 0: {
            uint8_t v[3] = { (uint8_t)((s_fcnt % 6) == 0),
                             (uint8_t)((s_fcnt % 2) ? 1 : 0), 0 };
            return miot_beacon_obj_bytes(objs, MB_OBJ_MOTION_LIGHT, v, 3);
        }
        case 1: return miot_beacon_obj_u32(objs, MB_OBJ_NO_MOTION, s_fcnt % 3600);
        case 2: {
            uint8_t v[3] = { 0, 0, (uint8_t)((s_fcnt % 20) ? 1 : 0) };
            return miot_beacon_obj_bytes(objs, MB_OBJ_BUTTON, v, 3);
        }
        default: return miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
        }
    }

    case 9: {   /* 水浸：0x1014 0=正常 >0=漏水 / 电量 */
        switch (idx) {
        case 0: {
            uint8_t v[1] = { (uint8_t)((s_fcnt % 40) < 10 ? 1 : 0) };
            return miot_beacon_obj_bytes(objs, MB_OBJ_LEAK, v, 1);
        }
        default: return miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
        }
    }

    case 10:    /* 温湿度计3 mini：0x4C01 f32 / 0x4C02 u8 / 0x4803 u8 */
        switch (idx) {
        case 0: return miot_beacon_obj_f32(objs, MB_OBJ_TEMP_F32, (float)v_temp / 10.0f);
        case 1: return miot_beacon_obj_u8(objs, MB_OBJ_HUM_U8, (uint8_t)(v_hum / 10));
        default: return miot_beacon_obj_u8(objs, MB_OBJ_BATT_U8_48, v_batt);
        }

    default:
        return miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
    }
}

static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = DEV_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;
    if (DEV_KIND == 1) {
        n = build_objs_beacon(idx, objs, sizeof(objs));
    } else if (DEV_BEACON_KIND == 1) {
        switch (idx) {
        case 0:  n = miot_beacon_obj_i16(objs, MB_OBJ_TEMP_I16, v_temp); break;
        case 1:  n = miot_beacon_obj_u16(objs, MB_OBJ_HUM_U16,  v_hum);  break;
        default: n = miot_beacon_obj_u8 (objs, MB_OBJ_BATTERY,  v_batt); break;
        }
    } else {
        n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
    }

    int fl = miot_beacon_frame_v5(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
        return fl;
    }
    s_flen = (uint8_t)fl;
    return fl;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;

    static const ble_uuid16_t uuid_fe95 = BLE_UUID16_INIT(0xFE95);

    uint8_t sd[32];
    int n = 0;
    sd[n++] = 0x95; sd[n++] = 0xFE;
    memcpy(sd + n, s_frame, s_flen);
    n += s_flen;

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    /* 必须显式带 16-bit Service UUID：App 刷新数据时按 0xFE95 过滤扫描，
     * 只有 service data 没有 UUID 列表时 Android 的 ScanFilter 匹配不上。 */
    f.uuids16 = &uuid_fe95;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.svc_data_uuid16 = sd;
    f.svc_data_uuid16_len = (uint8_t)n;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d (svc_data=%dB)", rc, n);
        return;
    }

    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(DEV_PID & 0xFF);
    mfr[m++] = (uint8_t)(DEV_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)D->name;
    rsp.name_len = (uint8_t)strlen(D->name);
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = s_auth.bound ? 800 : 160;
    p.itvl_max = s_auth.bound ? 800 : 320;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void adv_refresh(void)
{
    if (gatt_miot_is_connected()) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    adv_start();
}

/* ---------------- GAP 事件 ---------------- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            gatt_miot_on_connect(event->connect.conn_handle, BLE_ATT_MTU_DFLT);
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "phone connected -> adv stopped");
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        gatt_miot_on_disconnect();
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            build_frame(s_idx);
        }
        adv_start();
        ESP_LOGI(TAG, "disconnected reason=0x%02x -> adv resumed", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

/* ---------------- spec 上报（仅 kind=0） ---------------- */

/** 按属性表整体推一帧 properties_changed（通用型号用这个） */
static void push_table(void)
{
    miot_spec_value_t v[16];
    size_t n = 0;
    for (size_t i = 0; i < s_tbl_cnt && n < 16; i++) {
        miot_prop_t *p = &s_tbl[i];
        memset(&v[n], 0, sizeof(v[n]));
        v[n].siid = p->siid;
        v[n].piid = p->piid;
        v[n].type = p->type;
        switch (p->type) {
        case MIOT_TYPE_BOOL:
        case MIOT_TYPE_U8:    v[n].val.u8  = *(uint8_t  *)p->ptr; break;
        case MIOT_TYPE_U16:   v[n].val.u16 = *(uint16_t *)p->ptr; break;
        case MIOT_TYPE_U32:   v[n].val.u32 = *(uint32_t *)p->ptr; break;
        case MIOT_TYPE_I32:   v[n].val.i32 = *(int32_t  *)p->ptr; break;
        case MIOT_TYPE_FLOAT: v[n].val.f   = *(float    *)p->ptr; break;
        default: continue;
        }
        n++;
    }
    if (n) miot_device_send_props_changed(&s_dev, v, n);
}

/** 夜灯2：亮度在 30~70 之间缓慢摆动，页面滑条能看出「设备在推值」 */
static void night_light_tick(uint32_t ts)
{
    for (size_t i = 0; i < s_tbl_cnt; i++) {
        miot_prop_t *p = &s_tbl[i];
        if (p->siid == 2 && p->piid == 1 && p->type == MIOT_TYPE_U8) {
            *(uint8_t *)p->ptr = (uint8_t)(30 + (ts / 7000) % 41);
        }
    }
}

static void do_report(uint32_t ts)
{
    switch (s_sel) {
    case 1:     /* 温湿度计2：专用 profile 的 float/湿度双 siid 兼容编码 */
        thermometer_report(&s_dev, (float)v_temp / 10.0f, (float)v_hum / 10.0f, ts);
        break;
    case 5:     /* 驱蚊器：zimi.mosq.v1 三属性一帧 */
        repeller_report(&s_dev, repeller_power(), v_batt, v_margin, ts);
        break;
    default:    /* 其余 spec 型：按属性表整体推 */
        push_table();
        break;
    }
    ESP_LOGI(TAG, "spec properties pushed (%u props)", (unsigned)s_tbl_cnt);
}

/* ---------------- 档案选择（NVS + 串口命令） ---------------- */

/* sdkconfig 可能是在 Kconfig 加 DEFAULT_INDEX 之前生成的（现场改过 Kconfig 但
 * idf.py 不会因 Kconfig 变化自动 reconfigure），这里给个同级兜底，避免整个工程
 * 卡在一个「符号未定义」上。运行时 NVS 里的 sel 优先级更高。 */
#ifdef CONFIG_MIOT_DEV_DEFAULT_INDEX
#define DEV_DEFAULT_INDEX  CONFIG_MIOT_DEV_DEFAULT_INDEX
#else
#define DEV_DEFAULT_INDEX  2      /* 体脂秤 S400 */
#endif

static void sel_load(void)
{
    int sel = DEV_DEFAULT_INDEX;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_SEL, &v) == ESP_OK) sel = v;
        nvs_close(h);
    }
    if (sel < 0 || sel >= DEV_COUNT) sel = DEV_DEFAULT_INDEX;
    s_sel = sel;
    D = &k_devs[s_sel];
}

static void sel_store(int sel)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_SEL, (uint8_t)sel);
        nvs_commit(h);
        nvs_close(h);
    }
}

/** 串口命令任务（USB-Serial/JTAG 控制台）：
 *    dev [N]  打印档案表 / 切换到档案 N（擦 NVS 身份后重启）
 *    info     打印当前运行状态
 *    factory  擦 NVS 重启（回到未绑定）
 *    reboot   重启
 * 现场实测靠它免烧录切换 11 个档案。 */
static void cli_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (char *p = line; *p; p++) {
            if (*p == '\r' || *p == '\n') { *p = 0; break; }
        }

        if (!strncmp(line, "dev", 3)) {
            const char *arg2 = line + 3;
            while (*arg2 == ' ') arg2++;
            if (*arg2) {
                int n = atoi(arg2);
                if (n >= 0 && n < DEV_COUNT) {
                    ESP_LOGW(TAG, "switch -> [%d] %s (erase NVS, restart)", n, k_devs[n].label);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    nvs_flash_erase();
                    nvs_flash_init();
                    sel_store(n);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else {
                    ESP_LOGW(TAG, "序号越界：0..%d", DEV_COUNT - 1);
                }
            } else {
                for (int i = 0; i < DEV_COUNT; i++) {
                    ESP_LOGI(TAG, "  [%2d] %-9s 0x%04X %-6s %s%s", i, k_devs[i].tag,
                             k_devs[i].pid, k_devs[i].kind ? "beacon" : "spec",
                             k_devs[i].label, i == s_sel ? "   <== 当前" : "");
                }
            }
        } else if (!strcmp(line, "info")) {
            ESP_LOGI(TAG, "sel=%d tag=%s pid=0x%04X kind=%d bound=%d login=%d conn=%d heap=%u",
                     s_sel, D->tag, D->pid, D->kind, s_auth.bound, s_auth.logged_in,
                     gatt_miot_is_connected(), (unsigned)esp_get_free_heap_size());
        } else if (!strncmp(line, "weight", 6)) {
            /* 让秤「称一次」，验证 App 秤页面能不能落记录：
             *   weight           默认 64.5kg / 阻抗 500
             *   weight 70        70kg（< 2000 按 kg 算，只支持整数 kg）
             *   weight 70250 480 70.25kg（>= 2000 按克算）+ 阻抗 480
             * 依次发：上秤过程流（波动→稳定→离秤）→ 九参结果 CSV →
             * 11.1022 BLE 事件 → 11.2/11.3 properties_changed（双保险）。 */
            if (s_sel != 2) {
                ESP_LOGW(TAG, "weight 只在档案 [2] 体脂秤 S400 下有效（当前 sel=%d）", s_sel);
            } else if (!s_auth.logged_in) {
                ESP_LOGW(TAG, "App 未登录（login=0）：先在米家里进一次秤页面再发 weight");
            } else {
                const char *a1 = line + 6;
                while (*a1 == ' ') a1++;
                uint32_t g = *a1 ? (uint32_t)strtoul(a1, NULL, 10) : 64500u;
                if (g < 2000u) g *= 1000u;
                uint32_t imp = 500u;
                const char *a2 = strchr(a1, ' ');
                if (a2) {
                    while (*a2 == ' ') a2++;
                    if (*a2) imp = (uint32_t)strtoul(a2, NULL, 10);
                }
                ESP_LOGW(TAG, "weigh-in: %u.%03u kg  imp=%u  ts=%u",
                         (unsigned)(g / 1000u), (unsigned)(g % 1000u), (unsigned)imp,
                         (unsigned)scale_now());
                /* 交给主循环的分帧状态机，别在这里连发（ag2 窗口只吃第一帧） */
                s_ww = g; s_wimp = imp; s_wts = scale_now();
                s_wdelay = 0; s_wgap_p = k_wgap; s_click_pending = 0;
                s_wstep = 0; s_wnext = now_ms();
            }
        } else if (!strcmp(line, "frames")) {
            /* 逐槽构造一次真实广播帧并打印 —— 不依赖手机即可校验
             * 31B 预算、对象编码与槽位数是否对齐（现场评审的第一手证据）。 */
            ESP_LOGI(TAG, "frames for [%d] %s pid=0x%04X bound=%d",
                     s_sel, D->tag, D->pid, s_auth.bound);
            for (int i = 0; i < DEV_SLOTS; i++) {
                int n = build_frame(i);
                if (n < 0) { ESP_LOGE(TAG, "  slot %d rc=%d", i, n); continue; }
                char hex[3 * 32 + 4];
                size_t o = 0;
                for (int k = 0; k < n && k < 32; k++)
                    o += (size_t)snprintf(hex + o, sizeof(hex) - o, "%02X ", s_frame[k]);
                ESP_LOGI(TAG, "  slot %d %-13s %2dB  %s", i, slot_name(i), n, hex);
            }
        } else if (!strcmp(line, "factory")) {
            ESP_LOGW(TAG, "factory reset -> erase NVS, restart");
            vTaskDelay(pdMS_TO_TICKS(50));
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (!strcmp(line, "reboot")) {
            esp_restart();
        } else if (*line) {
            ESP_LOGW(TAG, "命令: dev [N] | info | frames | weight [值] [阻抗] | factory | reboot");
        }
    }
}

/* ---------------- BOOT 键（GPIO9，板载 BOOT）
 * 短按   = 让秤「称一次」（走 scale_s400_report 全链路）
 * 长按 5s = 恢复出厂（清绑定身份，回到未注册 hello 广播）
 *
 * 为什么按键触发而不是串口命令：这块板子的 console 走 USB-Serial-JTAG，
 * 每次打开串口都会复位芯片，App 的连接跟着断（22:53 实测：一边电话
 * 一边按串口查状态，conn/login 永远是 0）。按键完全不碰串口。 */
static void btn_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 9,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(200));

    for (;;) {
        if (gpio_get_level(9) == 0) {
            int ms = 0;
            while (gpio_get_level(9) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                ms += 100;
                if (ms >= 5000) {
                    ESP_LOGW(TAG, "BOOT held 5s -> erase NVS (factory reset)");
                    nvs_flash_erase();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            if (ms >= 60) {
                ESP_LOGW(TAG, "BOOT press %dms -> weigh-in requested", ms);
                s_weigh_req = 1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- 启动 ---------------- */
static void on_sync(void)
{
    uint8_t addr[6];
    if (ble_hs_util_ensure_addr(0) == 0 &&
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        /* MAC 必须按 LSB 序进帧：云端拼 nonce 用 (mac||pid||cnt||counter_ext)，
         * 序错了加密帧它永远解不开（表现为页面永远没读数）。 */
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "host synced, %s  PID=0x%04X  MAC %02X:%02X:%02X:%02X:%02X:%02X  bound=%d",
             DEV_LABEL, DEV_PID,
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0], s_auth.bound);

    s_idx = 0;
    build_frame(s_idx);
    adv_start();
    gatt_miot_dump_handles();
    s_was_bound = s_auth.bound;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    sel_load();            /* 先定档案，ensure_pid_match 才知道该比哪个 PID */
    ensure_pid_match();

    /* BOOT(GPIO9) 的按键处理挪到 btn_task：开机检测只管「按住不放」，
     * 运行中短按要能触发称重，所以做成常驻任务（见 btn_task）。 */

    /* ★每个档案一个独立 MAC。11 个型号共用同一地址时，米家/MIUI 会把
     * 「反复绑定失败过的地址」写进 DeviceBlackList，之后无论换哪个档案
     * 都扫不到（22:32 实测：onScanResult 收到了，但
     *  "device is in the DeviceBlackList ... do not deal with it"）。
     * 只在基础 MAC 的第 5 字节上按档案序号派生：
     *   ESP_MAC_BT = 基础 MAC 末字节 +1，所以基础写 0x01 → 广播出去是 0x02。
     * 同一档案重启后地址不变（s_sel 存在 NVS），绑定关系不会因重启失效。 */
    {
        uint8_t base[6] = { 0x10, 0x00, 0x3B, 0xCE,
                            (uint8_t)(0x40 + (s_sel & 0x0F)), 0x01 };
        esp_base_mac_addr_set(base);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    ESP_LOGI(TAG, "MAC for [%d] %s: %02X:%02X:%02X:%02X:%02X:%02X",
             s_sel, DEV_LABEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    miot_port_esp32_crypto_init();
    fcnt_load();

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    if (DEV_KIND == 0) {
        D->init(&s_dev, &s_port, &s_tbl, &s_tbl_cnt);
        ESP_LOGI(TAG, "profile loaded: %s (%u props)", DEV_LABEL, (unsigned)s_tbl_cnt);
    } else {
        /* 广播型：不注册属性表，spec 通道只用于认证/绑定，数据面全在 MiBeacon */
        miot_device_init(&s_dev, &s_port);
        miot_device_attach(&s_dev, NULL, 0, NULL, NULL, NULL);
        s_tbl = NULL;
        s_tbl_cnt = 0;
        ESP_LOGI(TAG, "profile loaded: %s (beacon-only, %u adv slots)",
                 DEV_LABEL, (unsigned)DEV_SLOTS);
    }

    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.save        = id_save;
    ap.load        = id_load;
    ap.random      = port_random;
    miot_auth_init(&s_auth, &ap);

    uint32_t t0 = now_ms();
    miot_auth_prepare(&s_auth);
    ESP_LOGI(TAG, "ECDH prepare %lums", (unsigned long)(now_ms() - t0));

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(D->name);
    gatt_miot_set_pid(DEV_PID);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);
    xTaskCreate(cli_task, "cli", 4096, NULL, 4, NULL);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);

    uint32_t last_report = 0;
    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            s_login_ms = t;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** session established: spec channel ready ***");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame(s_idx);
            adv_refresh();
        }

        if (!gatt_miot_is_connected()) {
            if (t - last_adv >= ADV_PERIOD_MS) {
                last_adv = t;
                s_idx = (s_idx + 1) % DEV_SLOTS;
                s_fcnt++;
                sim_tick();
                if ((s_fcnt % 10) == 0) fcnt_save();
                if (build_frame(s_idx) >= 0) {
                    adv_refresh();
                    ESP_LOGD(TAG, "adv#%d %-13s frame=%dB cnt=%lu",
                             s_idx + 1, slot_name(s_idx), s_flen, (unsigned long)s_fcnt);
                }
            }
        } else if (s_logged_in && t - last_report >= SPEC_PERIOD_MS) {
            last_report = t;
            if (DEV_KIND == 0) {
                if (s_sel == 0) night_light_tick(t);
                do_report(t);
            }
        }

        /* BOOT 短按 -> 称一次。必须等 App 登录（会话密钥就绪），
         * 否则事件加密发不出去，App 那边就是「没反应」。 */
        if (s_weigh_req) {
            s_weigh_req = 0;
            if (s_sel != 2) {
                ESP_LOGW(TAG, "BOOT 称重只对档案 [2] S400 有效（当前 sel=%d）", s_sel);
            } else if (!s_auth.logged_in) {
                ESP_LOGW(TAG, "App 未登录（login=0）：先在米家里打开秤页面，再按 BOOT");
            } else if (s_wstep < 0) {
                int from_click = s_click_pending;
                s_click_pending = 0;
                s_wstep = 0;
                /* 点击触发要留出 App 的界面过渡时间；BOOT 是「人踩上去」的等价
                 * 事件，此刻 App 已停在「请上秤」，可以立刻开口。 */
                if (from_click) { s_wdelay = 4000; s_wgap_p = k_wgap_slow; }
                else            { s_wdelay = 0;    s_wgap_p = k_wgap; }
                s_wnext = t + s_wdelay;
                /* 每按一次换一组数据：第 1 次 64.5kg、第 2 次 63.8kg、第 3 次 65.2kg，
                 * 之后循环。这样连按三次能在历史里落三条不同记录，便于确认
                 * 「是不是每次都是独立测量」，而不是同一条被覆盖。 */
                s_ww   = k_wcycle[s_widx][0];
                s_wimp = k_wcycle[s_widx][1];
                s_widx = (s_widx + 1) % (int)(sizeof(k_wcycle) / sizeof(k_wcycle[0]));
                s_wts = scale_now();
                s_last_weigh = t;
                ESP_LOGW(TAG, "weigh-in start %u.%03ukg imp=%u ts=%u",
                         (unsigned)(s_ww / 1000u), (unsigned)(s_ww % 1000u),
                         (unsigned)s_wimp, (unsigned)s_wts);
            }
        }

        /* 称重序列：一帧一步，按当前节奏表节流，绝不连发 */
        if (s_wstep >= 0 && (int32_t)(t - s_wnext) >= 0) {
            int st = s_wstep;
            int rc = scale_s400_report_step(&s_dev, st, s_ww, s_wimp, s_wts);
            ESP_LOGI(TAG, "weigh step %d/7 rc=%d", st, rc);
            if (++s_wstep >= SCALE_S400_STEPS) s_wstep = -1;
            else s_wnext = t + s_wgap_p[st];
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
