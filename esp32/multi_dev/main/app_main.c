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
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_profiles.h"

#include "gatt_miot.h"
#include "mesh_miot.h"

/* 事件池记账（实现在 patch/npl_os_freertos.c）：判断是突发打穿还是泄漏 */
extern int ble_npl_ev_live_count(void);
extern int ble_npl_ev_total_count(void);
extern int ble_npl_ev_hist(void **ras, int *cnts, int max);

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
    /* ★PID 必须是 0x03DD：2026-10-31 实测（LIGHT_SHELL）App 认这个号，
     * 显示「米家夜灯2」并可进设备页。早前误填 0x07F6（那是只走网关的
     * 执行器壳），App 绑完只给一个通用「夜灯」页、无控件且要求蓝牙网关。 */
    { "NIGHTL",   "MUE4094RT",  "夜灯2 (MUE4094RT)",          0x03DD, 0, 0, 1, sn_batt,   night_light_init },
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

/* BondAbility（广播帧最后一个字节的 bit3~4）：
 *   1 = 旧版绑定（App 下发 12B token，走 RC4 挑战应答）
 *   2 = 标准认证（ECDH P-256 + HKDF）
 * ★夜灯2（0x03DD）必须发 1。实测对比：
 *   发 2 时 App 认为「已按标准认证绑过」，于是跳过 INIT/token 协商，
 *   冷启写一条 GETRAND，1 秒后判失败、退订 CCCD 主动断开
 *   （旧工程 serial_rebind2.log 成功那次广播的正是 0x08，即 BondAbility=1）。
 *   而发 1 时 App 会走完整流程：INIT(静默) → 下发 12B token → GETRAND →
 *   LOGIN，token 由 App 指定，不存在两边派生不一致的问题。 */
#define DEV_BOND_ABILITY ((DEV_PID == 0x03DD) ? 1 : 2)

/* 未绑定广播的 FC 高字节（bit8 registered | bit9 solicited | bit10~11 authMode | bit12~15 version）
 *   0x22 = v2 | solicited=1 | authMode=0   ← 旧工程跑通那次用的就是它
 *   0x58 = v5 | solicited=0 | authMode=2   ← 我们原来
 * 串口 `fc 58` / `fc 22` 可运行时切换，避免为这一个字节反复烧录。 */
static uint8_t s_fc_hi;   /* 0 = 按档案自动（旧版绑定 0x22，其余 0x58） */
/* FC 低字节（bit6 Object | bit7 Mesh | bit4 加密 | bit3 MAC）。
 * 0 = 自动（0x58）。网关是 Mesh 网关，是否只接管「自报 mesh」的设备未知，
 * 串口 `fc 59 D8` 可在运行时把 bit7 置 1 验证，不必反复烧录。 */
static uint8_t s_fc_lo;
/* 强制以「待绑定」形态广播（即使 NVS 里 bound=1）。
 * 现场唯一的全链路成功轮（AUTH_INIT → token → BOND → DeviceID → GETRAND →
 * LOGIN 全过）就发生在设备广播待绑定帧的时候；广播已绑定帧的每一次连接，
 * App 都在订阅完 0x0001 后 1 秒主动放弃。串口 `advu 1` 打开。 */
static uint8_t s_adv_unbound;

#define ADV_PERIOD_MS  1500
/* 未绑定时不要每 1.5s 停一次广播：每次 adv_refresh 都是 stop+start，
 * 空档里手机的连接请求会落空（夜灯2 表现为「扫得到、点了没反应」）。
 * 未绑定 hello 帧的 frame counter 对 App 无意义，拉长周期即可。 */
#define ADV_PERIOD_UNBOUND_MS 5000
/* 真机夜灯2 的「蓝牙设定按钮」按一下 = 唤醒广播一段时间。BOOT 短按等价：
 * 30s 内用 20~30ms 的快广播，手机更容易连上，App 也会看到广播变密。 */
#define ADV_FAST_MS    30000
#define ADV_FAST_ITVL_MIN 32
#define ADV_FAST_ITVL_MAX 48
#define SPEC_PERIOD_MS 5000
#define NVS_NS         "miot"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"
#define NVS_KEY_PID    "devpid"
#define NVS_KEY_SEL    "devsel"
/* MAC 版本号：每 +1 末字节 +2，换一个全新 BLE 地址。
 * 米家/MIUI 会把「反复绑定失败过的地址」写进 DeviceBlackList（仓库里已有该结论），
 * 同一地址反复失败后就再也不会走完整绑定流程——App 只做「连上→订阅→1 秒后断开」
 * 的试探循环，且写的是 GETRAND（登录）而不是 INIT（绑定）。串口 `newmac` 递增。 */
#define NVS_KEY_MACV   "macv"

/* ★每个档案一个独立命名空间（2026-11-11 22:xx）：
 *   sel=0 -> "miot"（沿用旧数据），sel>0 -> "miot<sel>"。
 * 动机：以前 `dev N` 会 nvs_flash_erase() 把身份清空，设备变成"全新未绑定"，
 * 重新绑定时米家云端就多建一条设备记录（实测出现两台「飞利浦蓝牙夜灯」）。
 * 改成每档案独立后，各档案的 token/bind_key/fcnt/macv 互不覆盖，
 * 切换档案不再丢绑定、也不会再诱导米家重复添加。
 * devsel 本身必须放在全局命名空间，否则"先有鸡还是先有蛋"。 */
#define NVS_NS_GLOBAL  "miotcfg"   /* 只放 devsel */
static char s_ns[16] = "miot";     /* 当前档案的命名空间 */

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
static volatile int s_adv_req;      /* BOOT 短按：请求一次「配对唤醒」广播 */
static uint32_t s_fast_until;       /* 快广播截止时刻（ms） */

/* 夜灯2 的传感器上报（米家自动化唯一的数据来源）。
 * 这个型号是「条件设备」：只能当触发源，不能被小爱控制（官方 FAQ 明确写不支持）。
 * 网关把它当人体/光线传感器用，靠的正是 MiBeacon 的 0x000F 对象；
 * 没有网关、或广播里不带这个对象，米家「智能场景」里就不会出现以它为条件的选项。 */
static uint8_t  s_nl_motion;            /* 0x000F b1：0=无人 1=有人 */
static uint8_t  s_nl_light = 1;         /* 0x000F b2：0=暗 1=亮 */
static uint32_t s_nl_motion_until;      /* 有人状态保持到何时（ms），到点自动回落 */
static volatile int s_nl_motion_req;    /* CLI 置位，主循环消费（跨任务不直接碰广播） */

/* 无线开关（XMWXKG01YL / XMWXKG01CM）的按键事件。
 * 这个型号是纯「事件源」：设备页没有任何控件，米家自动化完全依赖 MiBeacon 的
 * 0x4E0C(单击) / 0x4E0D(双击) / 0x4E0E(长按) 三个对象。
 * ★之前的固件里这三个对象只在 miot_beacon.h 定义过、代码一次都没发过
 *   （build_frame 对 BTN 档案走 default 分支只发电量），所以按键事件永远上不了云。
 * 事件必须重复广播一段时间：网关按「同一对象连续若干帧」确认一次事件，
 * 只发一帧大概率被当成广播抖动丢掉。 */
static uint16_t s_btn_obj;              /* 0=空闲；否则 0x4E0C/0x4E0D/0x4E0E */
static uint8_t  s_btn_key = 1;          /* 载荷 = 按键号：1=左键 2=右键（双键版语义） */
static uint32_t s_btn_until;            /* 事件保持到何时（ms），到点回落电量帧 */
static volatile int s_btn_req;          /* 1=单击 2=双击 3=长按，主循环消费 */
/* 「叫网关来连」的事件窗口（`near` 命令）：
 * 秤没有能装体重的 Property 对象，第三方网关无从解析，也就不会主动连；
 * 唯一的招是发通用事件 0x0001/0x0003 —— 网关见到「设备请求被连接」才会
 * 作为 central 连上来，之后走我们已经打通的标准登录 + spec 会话推数据。 */
static uint16_t s_ev_obj;               /* 0=空闲；否则 0x0001/0x0003 */
static uint32_t s_ev_until;             /* 窗口截止（ms） */
static uint32_t s_ev_next;              /* 下一帧重发时刻（ms） */
/* 称重序列的分帧状态机（见 scale_s400_report_step：一帧一步，必须节流） */
static int      s_wstep = -1;
static uint32_t s_wnext, s_ww, s_wimp, s_wts;
/* 离线称重后的「请求被连接」窗口（ms 截止时刻）。
 * 真机称完、手机不在场时会把广播的 solicited 位置 1，等于对外喊「我有数据要交」；
 * 网关正是靠这一位决定要不要主动建链。窗口结束后自动回落到默认形态。 */
static uint32_t s_solicit_until;
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

/* action 的 out 参数：只有体脂秤的 13.1 sync 需要（离线消息数/SN/MAC/固件/电量）。
 * ★App 开屏第一件事就是发这条 267B 的动作，应答缺 out 时同步流程不推进。 */
int miot_device_action_out(uint8_t siid, uint16_t aiid,
                           miot_spec_value_t *outs, int max)
{
    return scale_s400_action_out(siid, (uint8_t)aiid, outs, max);
}

/* 体脂秤记录时间戳：板上没有 RTC，也没连 WiFi，只能自己造一个 wall clock。 * 做法是拿**编译时刻**（__DATE__/__TIME__）当锚点，再加运行时长 —— 误差等于
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
    if (nvs_open(s_ns, NVS_READWRITE, &h) != ESP_OK) return;
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
    if (nvs_open(s_ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*id);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_ID, id, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*id)) return false;
    ESP_LOGI(TAG, "identity loaded, did[0..3]=%02X %02X %02X %02X",
             id->did[0], id->did[1], id->did[2], id->did[3]);
    return true;
}

/* ---------------- legacy（App 下发 token）绑定完成回调 ----------------
 * 由 gatt_miot.c 在解出 BOND（cipher(token, 0xFA54AB92)）后调用。
 *
 * 旧工程 mi_beacon v121 的实机日志里，BOND 一确认广播立刻切成
 * 「bound kind=2 cnt=49 len=24」；不切的话 App 这边认证虽然过了，但回到
 * 设备列表仍然找不到已绑定的设备，加设备流程最终仍报「连接超时」。
 * token 是 App 指定的（不是 ECDH 派生的），所以直接当本机身份用；
 * bind_key 必须与广播加密用的是同一份，首次随机生成后就不再变。 */
/* 收到 12B token 立刻落盘（gatt_miot.c 在 ACK 之后调用）。
 * 只更新 token，不动 bound、不切广播 —— 绑定判定仍由 BOND 那一帧决定。 */
void miot_legacy_token_hook(const uint8_t token[12])
{
    if (memcmp(s_auth.id.token, token, 12) == 0) return;
    memcpy(s_auth.id.token, token, 12);
    id_save(NULL, &s_auth.id);
    ESP_LOGW(TAG, "*** legacy token 落盘: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X ***",
             token[0], token[1], token[2], token[3], token[4], token[5],
             token[6], token[7], token[8], token[9], token[10], token[11]);
}

void miot_legacy_bound_hook(const uint8_t token[12])
{
    memcpy(s_auth.id.token, token, 12);
    /* ★bind_key 的初始化必须和 token 是否变化解耦。
     * 12B token 一到就先落盘（miot_legacy_token_hook），等 BOND 走到这里时
     * memcmp 已经相等，原来嵌在「token 变了」分支里的 bkey 初始化就被跳过，
     * 留下全 0 的 bind_key —— 广播帧和 0x001A 数据帧全按全 0 加密，跨设备
     * 复用同一把空密钥，页面要么解不开、要么直接判定不匹配。 */
    bool zero = true;
    for (int i = 0; i < 16; i++) {
        if (s_auth.id.bind_key[i]) { zero = false; break; }
    }
    if (zero) esp_fill_random(s_auth.id.bind_key, 16);
    s_auth.bound = 1;
    id_save(NULL, &s_auth.id);
    ESP_LOGW(TAG, "*** legacy BOND: token 入库 -> 切已绑定广播 (bkey %02X%02X%02X%02X) ***",
             s_auth.id.bind_key[0], s_auth.id.bind_key[1],
             s_auth.id.bind_key[2], s_auth.id.bind_key[3]);
}

/* ---------------- 页面模式：0x0001 主动推帧 ----------------
 *
 * logcat 实证（12:27 那轮）：App 打开详情页后 订阅 0x001B/001A/0001，
 * 之后整 1000ms 零收包，随即**主动退订三个通道再干净断开**，隔 4~6s 重来。
 * 这不是异常超时，是「订阅后等设备推帧」的页面模式：真机订阅完立刻推一帧
 * MiBeacon（v5 加密），之后每 2s 心跳。不推 → 界面一直「连接超时」。
 * 参考工程 mi_beacon 的注释也写明：「页面模式 App 在登录前就等设备推帧，
 * 缺了它 App 1s 超时断开（页面连接超时根因之一）」。 */
static volatile uint8_t s_push_req;
/* 数据推送总开关（`push 0/1` 现场切）。0=连上后不推任何 MiBeacon 帧，
 * 用于判定「订阅后推帧」这一步是不是把 App 的认证/会话状态机顶坏了：
 * 页面重连时我们在认证应答后 190ms 就往同一条 0x0001 塞 16B 数据帧，
 * App 有可能把它当成认证应答吃掉，随后 600ms 就退订走人。 */
volatile uint8_t s_push_on = 0;

/** 从属性表里读一个 uint8（找不到时返回 dflt） */
static uint8_t prop_u8(uint8_t siid, uint16_t piid, uint8_t dflt)
{
    if (!s_tbl) return dflt;
    miot_prop_t *p = miot_props_find(s_tbl, s_tbl_cnt, siid, piid);
    if (!p || !p->ptr) return dflt;
    return (p->size == 1) ? *(uint8_t *)p->ptr : (uint8_t)(*(uint32_t *)p->ptr);
}

static volatile uint32_t s_sub_ms;

void miot_token_subscribed_hook(void)
{
    s_sub_ms = now_ms();
    s_push_req = 1;          /* 不在 NimBLE host 栈里做加密，交给 push_task */
}

static void push_token_frame(void)
{
    if (!s_push_on) return;                 /* `push 1` 才推，见 s_push_on 注释 */
    if (!s_auth.bound || !gatt_miot_is_connected()) return;

    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = DEV_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);
    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    /* ★2026-11-10 19:45 定论（对照 mi_beacon v121 夜灯壳）：
     * 数据帧只发「电量」一个对象。0x000F「有人移动」每 2s 推一次会让云端
     * 每 2s 刷一条移动事件；参考工程专门为此只推电量。
     * 无 MAC 形态帧长 = 12 + 对象长，电量对象 2+1+1=4 → 16B，稳过 ATT 20B。 */
    uint8_t objs[8];
    size_t n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, prop_u8(3, 1, 80));

    uint8_t out[32];
    /* ★计数器必须与广播共用同一个递增序列：单独从 0 开始会被 App 的
     * 重放保护当成回退帧直接丢弃（实测收到帧后 25ms 就断开）。 */
    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt++, objs, n, out, sizeof(out));
    if (fl <= 0) {
        ESP_LOGW(TAG, "push v5 build rc=%d (crypto backend?)", fl);
        return;
    }
    /* ★只发 0x0001（Token 通道）。这是唯一被真机/参考工程证实的数据通道：
     *   0x001B 是 spec/ag2 通道，App 把所有 notify 当 ag2 帧解析，
     *          裸 v5 帧会被判 invalid 并污染状态机（实测订阅后 20ms 即断）；
     *   0x001A 是标准认证 v2 的 sa_notify 通道，同样不吃裸 v5 帧。 */
    int rc = gatt_miot_notify_token(out, (size_t)fl);
    ESP_LOGI(TAG, "push v5 %dB -> 0x0001 rc=%d", fl, rc);
}

static void push_task(void *arg)
{
    (void)arg;
    uint32_t next_hb = 0;
    uint32_t first_at = 0;
    for (;;) {
        uint32_t now = now_ms();

        if (s_push_req) {
            s_push_req = 0;
            /* ★真机行为（mi_beacon: 「App 订阅数据通道后设备立即推一帧」）：
             * 订阅完 0x0001 后 App 立刻写 GETRAND，我们不推数据它 1.02s 就
             * 退订断开（实测每一轮都是 subscribe+1020ms，与 GETRAND 无关）。
             * 但 GETRAND 的 4B 应答必须先到 —— 那是认证状态机在等同一条通道
             * 上的包。所以：认证应答 180ms 发，数据帧排在订阅后 400ms。 */
            first_at = now + 400;
            next_hb = 0;
        }
        if (gatt_miot_is_connected() && first_at &&
            (int32_t)(now - first_at) >= 0 &&
            (int32_t)(now - gatt_miot_rx_ms()) >= 300) {
            first_at = 0;
            push_token_frame();
            next_hb = now_ms() + 2000;
        }
        /* 连接期间每 2s 心跳一帧，保持页面「在线」。next_hb=0 表示还没推过，
         * 不能当成「到期」（旧写法 0 会被判为已到期，导致一连上就野推一帧）。
         * 同样要求「认证静默 300ms」：App 正在写 GETRAND/LOGIN 时数据帧会
         * 抢同一条 0x0001 通道，被它的认证状态机当成应答吃掉。 */
        if (gatt_miot_is_connected() && next_hb &&
            (int32_t)(now - next_hb) >= 0 &&
            (int32_t)(now - gatt_miot_rx_ms()) >= 300) {
            next_hb = now + 2000;
            push_token_frame();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void fcnt_load(void)
{    nvs_handle_t h;
    if (nvs_open(s_ns, NVS_READONLY, &h) != ESP_OK) { s_fcnt = 1; return; }
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_FCNT, &v) != ESP_OK) v = 1;
    nvs_close(h);
    s_fcnt = v;
}

static void fcnt_save(void)
{
    nvs_handle_t h;
    if (nvs_open(s_ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_FCNT, s_fcnt);
    nvs_commit(h);
    nvs_close(h);
}

/** 型号切换守卫：当前命名空间里的身份属于别的 PID 时擦掉它。
 *  否则换型号后 App 会拿旧 did 当「已绑定设备」，永远进不了添加流程。
 *  devpid 没记录过但存在身份 blob（旧固件留下）同样视为不匹配。
 *  只擦当前档案的命名空间，不再整片 erase —— 其它档案的绑定要保留。 */
static void ensure_pid_match(void)
{
    uint16_t stored = 0;
    bool has_pid = false, has_ident = false;
    nvs_handle_t h;
    if (nvs_open(s_ns, NVS_READONLY, &h) == ESP_OK) {
        has_pid = (nvs_get_u16(h, NVS_KEY_PID, &stored) == ESP_OK);
        size_t sz = 0;
        has_ident = (nvs_get_blob(h, NVS_KEY_ID, NULL, &sz) == ESP_OK && sz > 0);
        nvs_close(h);
    }
    if ((has_pid && stored != DEV_PID) || (!has_pid && has_ident)) {
        ESP_LOGW(TAG, "NVS 身份与本型号不符 (devpid %s, 存在身份=%d, 当前 PID=0x%04X)"
                      " -> 只擦当前档案 [%s]", has_pid ? "已记录且不同" : "未记录",
                 has_ident, DEV_PID, s_ns);
        if (nvs_open(s_ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    if (nvs_open(s_ns, NVS_READWRITE, &h) == ESP_OK) {
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

    if (!s_auth.bound || s_adv_unbound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, (uint8_t)DEV_BOND_ABILITY,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        /* ★配对唤醒窗口内置 FC bit9(solicited)：官方位定义里这一位表示
         * 「设备请求被连接」。夜灯2 配网时 App 会停在「请按设备按钮」，
         * 真机按下按钮后广播才会带上这个「我准备好了」的信号，App 据此发起
         * 连接。只改广播间隔/帧计数（我们之前做的）App 完全看不见，
         * 所以按了十几次 BOOT 都没反应。窗口结束后再清掉这一位。 */
        /* ★2026-11-10 20:0x 根因：待绑定帧的 FC 高字节被按 BondAbility 写成 0x22，
         * 而 0x22 解出来是 version=2 / authMode=0 —— App 把这台设备当成「二代老设备」，
         * 于是**永远走 legacy p21 挑战应答**（0x0010 AUTH_INIT + 0x0001 12B token），
         * 根本不进标准认证。这正是文档里「重绑走 legacy PSK」那条死路：
         * 绑定能过、登录能过，但页面会话没有 spec 通道，订阅完 1s 就断（界面「连接超时」）。
         * FC 位定义：bit12..15=version，bit10..11=authMode，bit8=registered，bit9=待连接。
         * 正确形态是 0x58（version5 | authMode2 标准认证 | registered0），BondAbility
         * 只应出现在 capability 字节里（bit3..4），不能影响 FC。 */
        /* ★2026-11-10 21:0x 再次纠正（这次有参考工程真机实测做依据）：
         *   V2 明文未绑定帧的 FC 高字节 = 0x20（平时）/ 0x22（bit9=设备请求被连接）。
         *   0x58 是「v5 + registered=0」的畸形组合，App 直接无视；
         *   0x59 才是真机**已绑定**设备发的（v5|registered=1|authMode=2）。
         *   我们上一步把未绑定帧写成 0x58，等于既不是待绑定、也不是已注册，
         *   App 连上后什么都不写、1 秒退订——现场就是 -55。
         *   现在未绑定一律发 0x22（一直请求连接，真机按键后同款）。 */
        /* ★2026-11-12 13:1x 铁证修正：原来这里写 0x22，解出来 authMode=0，
         * 于是 App 把设备当「二代 legacy 设备」，**绑定全程走 p21 挑战应答**
         * （串口实证：`legacy token 落盘` + `legacy BOND 确认`，两次绑定都是），
         * 拿到的 12B token 是 legacy 兜底值，不是云端 token —— 之后的
         * 标准 LOGIN 用这把假钥匙算 devConfirm，App 必然不认，页面永久转圈。
         * 真机形态：v2|registered=0|authMode=2 → 0x28；配对窗口加 bit9 → 0x2A。 */
        s_frame[1] = s_fc_hi ? s_fc_hi : 0x2A;
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;
    if (s_ev_obj) {
        /* 事件帧优先：一帧只装一个对象，长度 12+3+4=19B，远低于 31B 预算 */
        n = miot_beacon_obj_u8(objs, s_ev_obj, 1);
    } else if (DEV_KIND == 1) {
        n = build_objs_beacon(idx, objs, sizeof(objs));
    } else if (s_sel == 0) {
        /* 夜灯2：移动 / 光照 / 电量 三个 1B 对象轮换。
         *
         * ★为什么不能照抄真机的 3B 0x000F：主广播包 = uuids16 列表(AD头2+UUID2=4)
         *   + svc_data(AD头2 + UUID2 + 帧 N)，合计 8+N ≤ 31 → 帧长上限 23B。
         *   帧 = 11(FC2+PID2+cnt1+MAC6) + 对象(2+1+值) + ctr 3 + MIC 4 = 21+值，
         *   所以值最多 2B。真机 3B 形态是 24B，实测直接 rc=4(EMSGSIZE)，整帧发不出去
         *   （之前只发电量帧 22B，恰好卡在上限内，所以一直没暴露这个约束）。
         *
         *   因此 0x000F 用 2B（b1=移动 b2=光照；真机第 3 字节是保留位，去掉不影响解析）。
         *   另两帧轮换 0x1018(光照明暗) / 0x100A(电量)，都是 22~23B。 */
        int nl_kind = (int)(s_fcnt % 3);
        if (s_nl_motion) nl_kind = 0;   /* 有人移动期间优先发移动帧，别被轮换吃掉这次事件 */
        switch (nl_kind) {
        case 0: {
            uint8_t v[2] = { s_nl_motion ? 1 : 0, s_nl_light ? 1 : 0 };
            n = miot_beacon_obj_bytes(objs, MB_OBJ_MOTION_LIGHT, v, 2);
            break;
        }
        case 1: {
            uint8_t v[1] = { s_nl_light ? 1 : 0 };
            n = miot_beacon_obj_bytes(objs, MB_OBJ_LIGHT, v, 1);
            break;
        }
        default:
            n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
            break;
        }
    } else if (s_sel == 3 || s_sel == 4) {
        /* 无线开关：有按键事件时发事件对象（载荷恒 1，与真机一致），
         * 空闲时发心跳电量。两者的帧长都是 22B，落在 23B 上限内。 */
        if (s_btn_obj) n = miot_beacon_obj_u8(objs, s_btn_obj, s_btn_key);
        else           n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
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
    /* 已绑定形态的 FC 高字节也允许现场覆盖（`fc 58` / `fc 59`）：
     *  0x59 = v5|registered=1|authMode=2 —— 真机已注册设备的形态；
     *  0x58 = v5|registered=0|authMode=2 —— 实测 App 会退回去走 legacy GETRAND 登录。
     * 夜灯2 是 legacy p21 绑的（0x0010 上 AUTH_INIT/BOND/GETRAND），
     * 页面会话能不能起来，就看 App 认哪种。 */
    if (s_fc_hi) s_frame[1] = s_fc_hi;
    if (s_fc_lo) s_frame[0] = s_fc_lo;
    /* 离线称重窗口内强制 solicited=1：0x5B = v5|registered=1|solicited=1|authMode=2。
     * 真机称完、手机不在场时就是这个形态 —— 「我有数据要交，来连我」。
     * 网关的扫描策略正是按这一位决定要不要主动建链，所以窗口结束后必须回落，
     * 否则长期 solicited 会让网关以为一直有待传数据。 */
    if (s_solicit_until && (int32_t)(s_solicit_until - now_ms()) > 0) {
        s_frame[1] = 0x5B;
    }
    s_flen = (uint8_t)fl;
    return fl;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;
    /* ★Mesh 门卫：未配网 beacon 广播期间 Mesh 层持有 legacy 广播实例，
     * 这里必须让位，否则两边互相 stop/start 打架，谁也发不成完整帧。 */
    if (mesh_miot_adv_owns_radio()) return;

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
    /* ★2026-11-12 13:2x 这才是 App 真正读的那一帧：扫描应答里的 MiBeacon
     * 制造商数据（0xFE95 的 service data 只是辅助）。原写死 `bound?0x29:0x20`，
     * 未绑定恒为 0x20 → authMode=0 → App 判定「二代 legacy 设备」，
     * 绑定走 p21 挑战应答（串口实证 legacy token 落盘），token 是假的。
     * 已绑定 0x29 = v2|registered|authMode=2；未绑定对齐成 0x28。 */
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x29 : 0x28);
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
    /* ★连接态只能发「非连接」广播：NimBLE 在 MAX_CONNECTIONS=1 时对
     * connectable adv 直接返回 ENOMEM（ble_gap_ext_adv_params_validate:
     * `params->connectable && !ble_hs_conn_can_alloc()`）。真机连接期间发的
     * 本来也只是非连接 beacon 帧（让 App 扫描管线知道设备「在身边」），
     * 不影响已建立的 GATT 连接。断开后走 DISCONNECT 分支重启为可连接。 */
    p.conn_mode = gatt_miot_is_connected() ? BLE_GAP_CONN_MODE_NON
                                           : BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 配对唤醒窗口内用快广播（20~30ms），其余按绑定状态。
     * 已绑定固定 160=100ms，与真机一致（参考工程实测真机恒定 160）。
     * 之前用 800=500ms：App 详情页只有约 1s 判定窗口，500ms 间隔下扫描管线
     * 大概率一帧都收不到，设备就被判成「不在身边」。 */
    if (s_fast_until && (int32_t)(now_ms() - s_fast_until) < 0) {
        p.itvl_min = ADV_FAST_ITVL_MIN;
        p.itvl_max = ADV_FAST_ITVL_MAX;
    } else if (s_auth.bound) {
        p.itvl_min = 160;
        p.itvl_max = 160;
    } else {
        p.itvl_min = 160;
        p.itvl_max = 320;
    }

    /* 回调传 NULL：事件统一由 gap_listener_install() 注册的全局监听器分发，
     * 否则我们自己的广播和监听器会各收一份，CONNECT 被处理两次。 */
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           NULL, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void adv_refresh(void)
{
    if (gatt_miot_is_connected()) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    adv_start();
}

/* ---------------- GAP 事件 ---------------- */
/* 设备侧主动发起的 ATT MTU 交换回调（Android App 不会主动 requestMtu） */
static int nl_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                     uint16_t mtu, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error && error->status == 0) {
        ESP_LOGI(TAG, "MTU exchange done -> %u", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed status=%d", error ? error->status : -1);
    }
    return 0;
}

/* 最近一次连接的 central 地址（排查手机/网关混连时用） */
static uint8_t s_peer[6];
static int     s_peer_ok;
/* 诊断计数（每 30s 汇总一次，判断「对方有没有来找我们」） */
static uint32_t s_disc_cnt;      /* 收到的广播帧数 */
static uint32_t s_prov_pdu_cnt;  /* 其中 AD type 0x29 = Mesh 配网 PDU */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* ★2026-11-12 加：打印 central 身份。混合环境里手机、青萍网关、小爱都能连，
             * 只靠 "phone connected" 无法区分是谁，排查「网关到底连没连」必须看 MAC
             * 与连接参数（网关通常是固定长间隔 + 大 latency，手机是 7.5~15ms）。 */
            struct ble_gap_conn_desc cd;
            if (ble_gap_conn_find(event->connect.conn_handle, &cd) == 0) {
                memcpy(s_peer, cd.peer_id_addr.val, 6);
                s_peer_ok = 1;
                ESP_LOGW(TAG, "CENTRAL %02X:%02X:%02X:%02X:%02X:%02X itvl=%u lat=%u to=%u",
                         s_peer[5], s_peer[4], s_peer[3], s_peer[2], s_peer[1], s_peer[0],
                         (unsigned)cd.conn_itvl, (unsigned)cd.conn_latency,
                         (unsigned)cd.supervision_timeout);
            }
            /* ★多连接：主会话已占用时，第二条链路只旁听（不重置认证状态、
             * 不改连接参数），抓「网关连上来之后干什么」。 */
            if (gatt_miot_is_connected()) {
                ESP_LOGW(TAG, "2nd link conn=%u accepted as LISTENER (primary=%u alive)",
                         (unsigned)event->connect.conn_handle,
                         (unsigned)gatt_miot_conn_handle());
                return 0;
            }
            gatt_miot_on_connect(event->connect.conn_handle, BLE_ATT_MTU_DFLT);
            /* ★连接态必须继续广播（真机行为，参考工程实测结论）：
             * App 的扫描管线靠 MiBeacon 帧把设备标记为「在身边/在线」，
             * 详情页那句「正在连接…」等的就是这个信号。我们在 connect 时
             * 停播 → App 扫不到设备 → 约 1s 后自己 cancelOpen 并断开(0x213)，
             * 表现为「连接超时」。停播是之前一直连不上的直接原因之一。 */
            if (s_auth.bound) {
                if (ble_gap_adv_active()) ble_gap_adv_stop();
                adv_start();
                ESP_LOGI(TAG, "phone connected -> adv KEPT (bound, beacon alive)");
            } else {
                ble_gap_adv_stop();
                ESP_LOGI(TAG, "phone connected -> adv stopped (unbound, 握手期间静默)");
            }
            /* ★连接间隔压到 7.5~15ms。logcat 实证：App 从 connect 到 cancelOpen
             * 只有约 1 秒，而服务发现（12 特征 + 描述符，约 40 个 ATT 请求）
             * 在 interval=24(30ms) 下要 600~900ms，订阅完预算就没了 —— 表现为
             * 「订阅成功后 20ms 主动断开」。压到 7.5ms 可把这段降到 300ms 内。 */
            struct ble_gap_upd_params up;
            memset(&up, 0, sizeof(up));
            up.itvl_min = 6;              /* 7.5ms */
            up.itvl_max = 12;             /* 15ms  */
            up.latency = 0;
            up.supervision_timeout = 400; /* 4s */
            int urc = ble_gap_update_params(event->connect.conn_handle, &up);
            ESP_LOGI(TAG, "conn itvl -> 7.5~15ms, req rc=%d", urc);
            /* ★设备主动发起 MTU 交换（参考工程实测：Android App 从不自己
             * requestMtu，默认 23 会把 spec/插件命令截断成 20B；真机在连接
             * 建立后由设备以 GATT client 角色先发 Exchange MTU Request）。
             * 之前我们一直停在 mtu=23，App 侧可能因预算不足以推进认证。 */
            int mrc = ble_gattc_exchange_mtu(event->connect.conn_handle, nl_mtu_cb, NULL);
            ESP_LOGI(TAG, "MTU exchange initiated rc=%d", mrc);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (s_peer_ok) {
            ESP_LOGW(TAG, "DISCONNECT from %02X:%02X:%02X:%02X:%02X:%02X reason=0x%x",
                     s_peer[5], s_peer[4], s_peer[3], s_peer[2], s_peer[1], s_peer[0],
                     (unsigned)event->disconnect.reason);
            s_peer_ok = 0;
        }
        /* 旁听链路断开：主会话还在，什么都别动 */
        if (gatt_miot_is_connected() &&
            event->disconnect.conn.conn_handle != gatt_miot_conn_handle()) {
            ESP_LOGW(TAG, "listener conn=%u gone (reason=0x%x), primary keeps running",
                     (unsigned)event->disconnect.conn.conn_handle,
                     (unsigned)event->disconnect.reason);
            return 0;
        }
        gatt_miot_on_disconnect();
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            build_frame(s_idx);
        }
        /* 连接态是「非连接」广播，这里必须强行重启一次才能换回可连接形态，
         * 否则 adv_start 会因「已在广播」直接 return，手机再也连不上。 */
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        adv_start();
        ESP_LOGI(TAG, "disconnected reason=0x%02x -> adv resumed (connectable)", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d indicate=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        gatt_miot_on_subscribe(event->subscribe.attr_handle,
                               event->subscribe.cur_notify,
                               event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_DISC:
        /* ★Mesh 栈为配网开了常开扫描（prov_enable → pb_adv.link_accept），
         * 周围每一帧广播都会以 DISC 进来，全打会刷屏（实测 15s 三千多条）。
         * 只挑真正有用的：AD type 0x29 = Mesh Provisioning PDU。
         * 手机/网关一旦发 Provisioning Invite，这里就会打出来——这是判断
         * 「对方到底有没有来找我们配网」最直接的证据。 */
        {
            const uint8_t *p = event->disc.data;
            int left = event->disc.length_data;
            s_disc_cnt++;
            while (left >= 2 && p[0] >= 1 && p[0] + 1 <= left) {
                uint8_t t = p[1];
                if (t == 0x29) {   /* BT_DATA_MESH_PROV */
                    s_prov_pdu_cnt++;
                    ESP_LOGW(TAG, "PB-ADV PDU from %02X:%02X:%02X:%02X:%02X:%02X rssi=%d len=%d",
                             event->disc.addr.val[5], event->disc.addr.val[4],
                             event->disc.addr.val[3], event->disc.addr.val[2],
                             event->disc.addr.val[1], event->disc.addr.val[0],
                             event->disc.rssi, p[0] - 1);
                    ESP_LOG_BUFFER_HEX_LEVEL(TAG, p + 2, p[0] - 1, ESP_LOG_WARN);
                }
                left -= p[0] + 1;
                p += p[0] + 1;
            }
        }
        return 0;

    default:
        /* 排查期：把我们没显式处理的 GAP 事件也打出来（加密变更/参数更新等），
         * 只看类型号，频率低，不影响时序。 */
        ESP_LOGW(TAG, "GAP ev type=%d (unhandled)", (int)event->type);
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

static uint8_t s_macv;     /* MAC 版本号（NVS） */

static void macv_load(void)
{
    nvs_handle_t h;
    if (nvs_open(s_ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_MACV, &s_macv);
        nvs_close(h);
    }
}

static void macv_set(uint8_t v)
{
    s_macv = (uint8_t)(v & 0x3F);
    nvs_handle_t h;
    if (nvs_open(s_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_MACV, s_macv);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void macv_bump(void)
{
    macv_set((uint8_t)(s_macv + 1));
}

static void sel_load(void)
{
    int sel = DEV_DEFAULT_INDEX;
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(NVS_NS_GLOBAL, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_SEL, &v) == ESP_OK) { sel = v; got = true; }
        nvs_close(h);
    }
    if (!got && nvs_open("miot", NVS_READONLY, &h) == ESP_OK) {
        /* 旧固件把 devsel 存在 "miot" 里，读一次做向后兼容 */
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_SEL, &v) == ESP_OK) sel = v;
        nvs_close(h);
    }
    if (sel < 0 || sel >= DEV_COUNT) sel = DEV_DEFAULT_INDEX;
    s_sel = sel;
    D = &k_devs[s_sel];
}

/** 按当前档案构造命名空间名（必须在 sel_load 之后、任何身份读写之前调用）。 */
static void ns_build(void)
{
    if (s_sel == 0) snprintf(s_ns, sizeof(s_ns), "%s", NVS_NS);
    else            snprintf(s_ns, sizeof(s_ns), "miot%d", s_sel);
}

static void sel_store(int sel)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_GLOBAL, NVS_READWRITE, &h) == ESP_OK) {
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
                    /* 不再擦 NVS：每个档案有独立命名空间，切回去还是原来的绑定。
                     * 以前这里整片 erase，导致每次切档案都变成"新设备"，
                     * 米家云端会因此多建一条设备记录。 */
                    ESP_LOGW(TAG, "switch -> [%d] %s (keep identity, restart)", n, k_devs[n].label);
                    vTaskDelay(pdMS_TO_TICKS(50));
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
        } else if (!strcmp(line, "tok")) {
            /* 现场核对 token：我们手里的 vs App 下发的。GETRAND 应答用的是它，
             * 两边不一致时 App 会静默丢弃回来的一秒就断（界面上就是「连接超时」）。 */
            gatt_miot_dump_token();
        } else if (!strncmp(line, "btn", 3)) {
            /* btn [1|2|3]  1=单击 2=双击 3=长按（默认单击）
             * 对应 MiBeacon 0x4E0C / 0x4E0D / 0x4E0E，仅对档案 [3][4] 有意义。
             * 打开串口会复位芯片，所以量产验证请用 BOOT 短按。 */
            const char *a = line + 3;
            while (*a == ' ') a++;
            int k = (*a) ? atoi(a) : 1;
            int key = 1;
            const char *sp = strchr(a, ' ');
            if (sp) { key = atoi(sp + 1); }
            if (key < 1 || key > 2) key = 1;
            if (k < 1 || k > 3) {
                ESP_LOGW(TAG, "用法: btn [1=单击 2=双击 3=长按] [1=左 2=右]");
            } else {
                s_btn_key = (uint8_t)key;
                s_btn_req = k;
                ESP_LOGW(TAG, "btn: 已请求 %s %s键",
                         (k == 2) ? "双击" : (k == 3) ? "长按" : "单击",
                         (key == 2) ? "右" : "左");
            }
        } else if (!strcmp(line, "motion")) {
            /* 模拟「有人移动」。这是米家自动化里唯一能用的触发条件，
             * 前提：设备已绑定（bound=1，广播为已注册形态）且账号下有蓝牙网关。
             * 没有网关时米家根本不会出现这个条件，串口照样会打出广播帧。 */
            s_nl_motion_req = 1;
            ESP_LOGW(TAG, "sensor: 已请求一次「有人移动」，下一轮广播 0x000F");
        } else if (!strncmp(line, "lux", 3)) {
            /* lux 0 -> 环境光暗（自动化条件「环境光较暗时」成立）
             * lux 1 -> 环境光亮 */
            const char *a = line + 3;
            while (*a == ' ') a++;
            s_nl_light = (*a == '0') ? 0 : 1;
            ESP_LOGW(TAG, "sensor: light=%u (0=暗 1=亮)", s_nl_light);
        } else if (!strncmp(line, "nlstat", 6)) {
            /* 打印 nlight 文本协议的设备侧状态（亮度/时长/场景/已收命令数） */
            gatt_miot_nl_dump();
        } else if (!strncmp(line, "nl ", 3)) {
            /* nl <text>  —— 本机模拟一条插件命令，用来在没有手机时自测解析与应答：
             *   nl get_prop power bright lux snm dur
             *   nl set_bright 58
             *   nl apply_fixed_scene 2
             * 应答仍从 NUS TX 发出（没连接时会打印 no conn）。 */
            gatt_miot_nl_input(line + 3);
        } else if (!strncmp(line, "grd", 3)) {
            /* grd         -> 查看 GETRAND 回包首帧延时
             * grd <ms>    -> 设置（默认 40ms）。无 hook 时靠它抢在 App 断连前送达，
             *                现场可 20/40/80 扫一轮，看哪档 App 能接住。 */
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (*a) {
                gatt_miot_set_gr_delay(atoi(a));
            }
            ESP_LOGW(TAG, "GETRAND 回包首帧延时 = %d ms", gatt_miot_get_gr_delay());
        } else if (!strncmp(line, "nus", 3)) {
            /* nus             -> 打印说明
             * nus <hex>       -> 从 NUS TX(6E400003) 回一帧，给 philips 插件做实验
             * 2026-11-11 现场：插件页面只调 service=6E400001/char=6E400003，
             * 补齐该服务后先用这条命令试回包，确认插件是否认账。 */
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (!*a) {
                ESP_LOGI(TAG, "用法: nus <hex>   从 6E400003 回一帧（例: nus 0102）");
            } else {
                uint8_t buf[64];
                size_t n = 0;
                while (a[0] && a[1] && n < sizeof(buf)) {
                    char b[3] = { a[0], a[1], 0 };
                    buf[n++] = (uint8_t)strtoul(b, NULL, 16);
                    a += 2;
                    while (*a == ' ') a++;
                }
                int rc = gatt_miot_nus_notify(buf, n);
                ESP_LOGW(TAG, "nus 手工回包 len=%u rc=%d", (unsigned)n, rc);
            }
        } else if (!strncmp(line, "key", 3)) {
            /* key             -> 查询现有 token
             * key <24hex>     -> 手工写入 12B token 并立刻切「已绑定」形态广播。
             *
             * ★2026-11-10 20:0x 现场取证（设备端日志 + 手机 logcat 对照）：
             *   bound=1 的会话：App 依次 subscribe 001B / 001A，然后 **往 0x0010
             *   写 4B AUTH_INIT**，再 subscribe 0001，我们回 12B token ACK……
             *   bound=0 的会话：App 订阅完 001B/001A/0001 后**一个字节都不写**，
             *   1.02 秒退订断开（每 3.5s 重来）。手机侧同一时刻报 BLE -55。
             * 也就是说：广播必须是「已绑定」形态，App 才肯发起 legacy 认证。
             * NVS 被清后广播回到 hello 形态 → 认证永远不开始。
             * 这条命令就是把这个状态临时顶起来：先随便给个 token 让广播变成
             * 已绑定形态，App 连上后会在 AUTH_INIT 之后下发**真正的** token，
             * 固件照单采纳（legacy_adopt_token），密钥就对齐了。 */
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (!*a) {
                gatt_miot_dump_token();
            } else if (strlen(a) >= 24) {
                uint8_t t[12];
                for (int i = 0; i < 12; i++) {
                    char b[3] = { a[i * 2], a[i * 2 + 1], 0 };
                    t[i] = (uint8_t)strtoul(b, NULL, 16);
                }
                miot_legacy_token_hook(t);
                miot_legacy_bound_hook(t);
                ESP_LOGW(TAG, "手工 token 入库，bound=%d -> 已绑定形态广播", s_auth.bound);
                build_frame(s_idx);
                adv_refresh();
            } else {
                ESP_LOGW(TAG, "用法: key <24 位十六进制 token>");
            }
        } else if (!strcmp(line, "log")) {
            /* 事后再取证：不用串口监听当时活着，事件都记在 RAM 里 */
            gatt_miot_dump_evt();
        } else if (!strcmp(line, "handles")) {
            gatt_miot_dump_handles();
        } else if (!strncmp(line, "push", 4)) {
            /* push      -> 查询
             * push 1/0  -> 开/关「订阅后主动推 MiBeacon 帧」 */
            const char *a = line + 4;
            while (*a == ' ') a++;
            if (*a) s_push_on = (uint8_t)atoi(a);
            ESP_LOGW(TAG, "数据推送 = %d（0=不推，1=订阅后推 + 每 2s 心跳）", s_push_on);
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
        } else if (!strncmp(line, "ow", 2)) {
            /* ow                -> 离线称重，默认 64.5kg / 阻抗 500
             * ow 70 / ow 70250 480
             * 模拟「手机不在场时上秤」这个真实场景：体重写进属性表，
             * 广播 solicited 置 1 保持 10 分钟，等网关主动建链来读 3.1005。 */
            const char *a1 = line + 2;
            while (*a1 == ' ') a1++;
            uint32_t g = *a1 ? (uint32_t)strtoul(a1, NULL, 10) : 64500u;
            if (g < 2000u) g *= 1000u;
            uint32_t imp = 500u;
            const char *a2 = strchr(a1, ' ');
            if (a2) {
                while (*a2 == ' ') a2++;
                if (*a2) imp = (uint32_t)strtoul(a2, NULL, 10);
            }
            scale_s400_store_offline(g, imp);
            s_solicit_until = now_ms() + 10u * 60u * 1000u;
            adv_refresh();
            ESP_LOGW(TAG, "offline weigh-in %u.%03ukg imp=%u -> solicited adv 10min",
                     (unsigned)(g / 1000u), (unsigned)(g % 1000u), (unsigned)imp);
        } else if (!strncmp(line, "near", 4)) {
            /* near              -> 查询
             * near 0            -> 关闭
             * near 1 [秒]       -> 发 0x0001「连接」事件（默认 12s）
             * near 3 [秒]       -> 发 0x0003「靠近」事件
             * 目的：验证第三方网关（青萍）会不会因为事件而主动连上来。
             * 连上来之后能否读到数据，取决于网关有没有这个型号的读取表。 */
            const char *a = line + 4;
            while (*a == ' ') a++;
            if (!*a) {
                ESP_LOGW(TAG, "near: obj=0x%04X 剩余=%dms（0=关闭）", s_ev_obj,
                         s_ev_obj ? (int)(s_ev_until - now_ms()) : 0);
            } else {
                int obj = atoi(a);
                if (obj == 0) {
                    s_ev_obj = 0;
                    s_fast_until = 0;
                    s_fcnt++;
                    build_frame(s_idx);
                    adv_refresh();
                    ESP_LOGW(TAG, "near: 已停止，回到电量帧");
                } else if (obj == 1 || obj == 3) {
                    uint32_t sec = 12;
                    const char *a2 = strchr(a, ' ');
                    if (a2) { while (*a2 == ' ') a2++; if (*a2) sec = (uint32_t)strtoul(a2, NULL, 10); }
                    if (sec < 3) sec = 3;
                    if (sec > 300) sec = 300;
                    uint32_t t = now_ms();
                    s_ev_obj = (uint16_t)(obj == 1 ? MB_OBJ_EV_CONNECT : MB_OBJ_EV_NEAR);
                    s_ev_until = t + sec * 1000u;
                    s_ev_next = t;
                    /* 同时开快广播窗口：100ms 太稀，网关 20~30ms 一帧才容易抓到边沿 */
                    s_fast_until = s_ev_until;
                    s_adv_req = 1;
                    ESP_LOGW(TAG, "near: 0x%04X %s事件，持续 %us（快广播 + 220ms 重发）",
                             s_ev_obj, obj == 1 ? "连接" : "靠近", (unsigned)sec);
                } else {
                    ESP_LOGW(TAG, "用法: near 0 | near 1 [秒] | near 3 [秒]");
                }
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
            /* 只擦当前档案的身份，其它档案的绑定保留（要全清用 `factory all`） */
            ESP_LOGW(TAG, "factory reset -> 擦除档案 [%s] 身份, restart", s_ns);
            nvs_handle_t h;
            if (nvs_open(s_ns, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (!strncmp(line, "gr", 2)) {
            /* gr       -> 查询
             * gr 0     -> 静默（判定 App 是否在等 GETRAND 回包）
             * gr 1     -> 单帧（旧工程有效形态）
             * gr 18    -> 多假设循环（默认） */
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            if (*arg) gatt_miot_set_gr_reply(atoi(arg));
            ESP_LOGW(TAG, "GETRAND 应答帧数 = %d（0=静默 1=单帧 18=多假设）",
                     gatt_miot_get_gr_reply());
        } else if (!strncmp(line, "strat", 5)) {
            /* strat      -> 查询
             * strat -1   -> 自动轮换（每次重连换一种 通道+延时）
             * strat 0..5 -> 固定：0=200ms@0001 1=60ms@0001 2=110ms@0001
             *                      3=60ms@001B  4=60ms@001A  5=300ms@0001 */
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            if (*arg) gatt_miot_set_strat(atoi(arg));
            ESP_LOGW(TAG, "登录路径应答策略 = %d（-1=自动轮换）", gatt_miot_get_strat());
        } else if (!strncmp(line, "spec", 4)) {
            /* spec 1 -> App 订阅 001B/001A 时设备主动发 ag2 START 控制帧
             * spec 0 -> 关（默认） */
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            gatt_miot_set_spec_probe(atoi(arg));
            ESP_LOGW(TAG, "spec 探针 = %d（0=关 1=ag2 START 2=mng 建链）",
                     gatt_miot_get_spec_probe());
        } else if (!strcmp(line, "mstat")) {
            char st[160]; mesh_miot_status(st, sizeof(st));
            ESP_LOGW(TAG, "%s", st);
        } else if (!strncmp(line, "mprov", 5)) {
            /* mprov 0/1  -> 关/开 Mesh 配网承载（未配网 beacon + 0x1827）。
             * 关掉后 MiBeacon 广播恢复（`factory` 后默认是开的）。 */
            const char *arg = line + 5;
            while (*arg == ' ') arg++;
            if (*arg) {
                mesh_miot_set_prov(atoi(arg) ? true : false);
                adv_refresh();
            }
            char st[160]; mesh_miot_status(st, sizeof(st));
            ESP_LOGW(TAG, "%s", st);
        } else if (!strncmp(line, "muuid ", 6)) {
            /* muuid 8F03...  -> 整帧替换 16 字节 Mesh Device UUID（32 个 hex） */
            const char *arg = line + 6;
            uint8_t u[16]; int n = 0;
            while (n < 16 && arg[0] && arg[1]) {
                char b[3] = { arg[0], arg[1], 0 };
                u[n++] = (uint8_t)strtoul(b, NULL, 16);
                arg += 2;
            }
            if (n == 16) mesh_miot_set_uuid(u);
            else ESP_LOGW(TAG, "需要 32 个 hex 字符（16 字节），收到 %d 字节", n);
        } else if (!strncmp(line, "fc", 2)) {
            /* fc          -> 查询
             * fc 22       -> 只改高字节（旧形态）
             * fc 58       -> v5|solicited=0|auth=2
             * fc 59 D8    -> 高字节 0x59 + 低字节 0xD8（bit7 Mesh=1）
             * fc 59 58    -> 复位低字节（0x58 自动值） */
            const char *arg = line + 2;
            while (*arg == ' ') arg++;
            if (*arg) {
                char *end = NULL;
                s_fc_hi = (uint8_t)strtoul(arg, &end, 16);
                s_fc_lo = 0;
                if (end) {
                    while (*end == ' ') end++;
                    if (*end) s_fc_lo = (uint8_t)strtoul(end, NULL, 16);
                }
            }
            ESP_LOGW(TAG, "广播 FC = %02X %02X (hi=%02X%s lo=%02X%s)",
                     s_fc_lo ? s_fc_lo : 0x58, s_fc_hi ? s_fc_hi : 0x59,
                     s_fc_hi ? s_fc_hi : 0x59, s_fc_hi ? "" : "自动",
                     s_fc_lo ? s_fc_lo : 0x58, s_fc_lo ? "" : "自动");
            build_frame(s_idx);
            adv_refresh();
        } else if (!strncmp(line, "advu", 4)) {
            /* advu       -> 查询
             * advu 1/0   -> 强制/取消「待绑定」形态广播（token 与 bind_key 不动） */
            const char *arg = line + 4;
            while (*arg == ' ') arg++;
            if (*arg) s_adv_unbound = (uint8_t)(atoi(arg) ? 1 : 0);
            ESP_LOGW(TAG, "强制待绑定广播 = %d（bound=%d）", s_adv_unbound, s_auth.bound);
            build_frame(s_idx);
            adv_refresh();
        } else if (!strncmp(line, "setmacv", 7)) {
            /* 指定 MAC 版本号（末字节 = 0x01 + 2*n），重启生效。
             * 现场要「回到某台已配对过的地址」时用它，比连点 newmac 精确。 */
            const char *a = line + 7;
            while (*a == ' ') a++;
            if (*a) {
                uint8_t v = (uint8_t)(strtoul(a, NULL, 10) & 0x3F);
                macv_set(v);
                ESP_LOGW(TAG, "MAC 版本号 -> %u（末字节 %02X），重启生效",
                         s_macv, (unsigned)(0x01 + (s_macv & 0x3F) * 2));
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else {
                ESP_LOGW(TAG, "用法: setmacv N （0..63）");
            }
        } else if (!strcmp(line, "newmac")) {
            /* 换一个全新 BLE 地址（末字节 +2）并重启。用于绕开
             * 米家/MIUI 对「反复失败地址」的 DeviceBlackList，
             * 同时迫使手机丢弃 GATT 缓存、重新做服务发现。 */
            macv_bump();
            ESP_LOGW(TAG, "MAC 版本号 -> %u（末字节 %02X），重启生效",
                     s_macv, (unsigned)(0x02 + (s_macv & 0x3F) * 2));
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (!strcmp(line, "reboot")) {
            esp_restart();
        } else if (*line) {
            ESP_LOGW(TAG, "命令: dev [N] | info | tok | key <24hex> | log | handles | frames | gr [N] | fc [22|58] | advu [0|1] | setmacv N | newmac | weight [值] [阻抗] | factory | reboot");
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
                /* 任何档案都先当作「按一下设备上的蓝牙设定按钮」：真机夜灯2 配网时
                 * App 会提示按这个按钮，设备按下去会唤醒/加密广播一段时间。固件若对
                 * 这一步毫无响应，App 就一直停在那个提示/按钮上（实测卡住）。
                 * 顺带保留 S400 的称重语义，两个动作互不冲突。 */
                s_adv_req = 1;
                ESP_LOGW(TAG, "BOOT press %dms -> pairing adv + weigh-in", ms);
                /* 别再给开关档案塞称重请求：只会每按一次就打一行「只对 S400 有效」，
                 * 真正的动作是它自己的按键事件。 */
                if (s_sel == 2)                    s_weigh_req = 1;
                if (s_sel == 3 || s_sel == 4) {
                    /* BOOT 固定代表「左键单击」，不受上一次 `btn x 2` 的影响
                     * （否则现场按 BOOT 会莫名其妙报成右键，本轮实测踩到过）。 */
                    s_btn_key = 1;
                    s_btn_req = 1;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- 启动 ---------------- */
/* ★2026-11-13 「-55」解药：Mesh 栈自己起广播时把 GAP 回调传成 NULL
 * （glue.c bt_le_adv_start → ble_gap_adv_start(..., NULL, NULL)），
 * 于是「Mesh 广播引来的那条连接」的 CONNECT/DISCONNECT/SUBSCRIBE 事件
 * 一个都进不到 gap_event_cb：
 *   - gatt_miot_on_connect() 不被调用 → s_conn 停在 BLE_HS_CONN_HANDLE_NONE
 *   - notify_chr() 第一句就 return -1 → 认证/注册阶段每条回包全部发不出去
 *   - 手机侧干等 notify → 超时 → 米家错误码 -55（官方码表：绑定时 notify 超时）
 * 解法：注册 NimBLE 全局事件监听器，它在每条事件上都会调用（与 per-adv 回调
 * 并行，不是替代），此后无论谁起的广播，连接都能落到我们的状态机里。
 * 为避免同一事件被处理两次，adv_start 的 per-adv 回调改传 NULL。 */
static struct ble_gap_event_listener s_gap_listener;
static bool s_gap_listener_ok;

static void gap_listener_install(void)
{
    if (s_gap_listener_ok) return;
    int rc = ble_gap_event_listener_register(&s_gap_listener, gap_event_cb, NULL);
    s_gap_listener_ok = (rc == 0);
    ESP_LOGW(TAG, "gap listener register rc=%d (0=OK)", rc);
}

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
    /* 全局 GAP 监听器必须在任何广播起来之前装好（含 Mesh 的 beacon） */
    gap_listener_install();
    /* ★顺序关键（2026-11-13）：MiBeacon 必须**先**占住 legacy adv，Mesh 后初始化。
     * 反过来的话，Mesh 的 bt_le_adv_start 会先调 ble_gap_adv_set_data（全局写入）
     * 把 MiBeacon 的 service data / 制造商数据覆盖成 mesh beacon；即便它随后的
     * start 失败（实测 err 2），广播内容也已经废了，设备整个从空中消失。
     * glue.c 的门卫（g_mesh_adv_owned）再兜一层：宿主占着广播时 Mesh 直接返回。 */
    adv_start();
    mesh_miot_on_sync();
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
    ns_build();            /* 再定该档案的命名空间（身份/fcnt/macv 都按档案隔离） */
    macv_load();
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
                            (uint8_t)(0x40 + (s_sel & 0x0F)),
                            (uint8_t)(0x01 + (s_macv & 0x3F) * 2) };
        esp_base_mac_addr_set(base);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    ESP_LOGI(TAG, "MAC for [%d] %s: %02X:%02X:%02X:%02X:%02X:%02X",
             s_sel, DEV_LABEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    {
        /* 13.1 sync 的 out 参数要回 MAC/SN/固件：真机从 flash 读，这里用空中地址派生。
         * ★不填的话 App 拿到的设备页会缺「序列号/固件版本/电量」。 */
        char mstr[18], snstr[24];
        snprintf(mstr, sizeof(mstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(snstr, sizeof(snstr), "S400%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
        scale_s400_set_identity(mstr, snstr, "1.0.0_0031");
    }

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
    gatt_miot_set_mac(s_mac_air);
    {
        /* mixA/mixB 的正态输入是人读序 MAC（s_mac_air 是它的反转） */
        uint8_t mac_human[6];
        for (int i = 0; i < 6; i++) mac_human[i] = s_mac_air[5 - i];
        gatt_miot_set_mac_human(mac_human);
        ESP_LOGW(TAG, "mix MAC human=%02X%02X%02X%02X%02X%02X",
                 mac_human[0], mac_human[1], mac_human[2],
                 mac_human[3], mac_human[4], mac_human[5]);
    }
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    /* ★Mesh 栈早期挂载：bt_mesh_init 注册 Mesh Proxy/Prov GATT 服务，
     * 必须赶在 host 启动（ble_gatts_start 建表）之前，晚了会断言崩溃。 */
    {
        uint8_t mac_human[6];
        for (int i = 0; i < 6; i++) mac_human[i] = s_mac_air[5 - i];
        mesh_miot_early_init(DEV_PID, mac_human);
    }

    nimble_port_freertos_init(host_task);
    xTaskCreate(push_task, "push", 4096, NULL, 3, NULL);
    xTaskCreate(cli_task, "cli", 4096, NULL, 4, NULL);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);

    uint32_t last_report = 0;
    uint32_t last_adv = 0;
    int fast_window = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        /* 夜灯2 传感器上报：CLI 触发一次「有人移动」，1.5s 后自动回落。
         * 必须当场把帧刷出去 —— 网关按边沿判定事件，等下一轮心跳（100~300ms 也可能
         * 刚好落在电量帧上）会漏掉这次移动，自动化就不触发。 */
        if (s_nl_motion_req) {
            s_nl_motion_req = 0;
            s_nl_motion = 1;
            s_nl_motion_until = t + 1500;
            s_fcnt++;
            build_frame(s_idx);
            adv_refresh();
            ESP_LOGW(TAG, "sensor: 0x000F motion=1 (1.5s 后回落) light=%u", s_nl_light);
        } else if (s_nl_motion && (int32_t)(t - s_nl_motion_until) >= 0) {
            s_nl_motion = 0;
            /* 对齐到 3 的倍数，保证这次回落发的也是 0x000F（0=无人），
             * 否则网关只看到最后一帧是电量，这次「移动结束」就丢了。 */
            do { s_fcnt++; } while (s_fcnt % 3);
            if (build_frame(s_idx) >= 0) adv_refresh();
            ESP_LOGI(TAG, "sensor: 0x000F motion=0");
        }

        /* 无线开关按键：立刻把事件帧刷出去（网关按边沿判定，
         * 等下一轮心跳会落在别的帧上直接漏掉），并保持 2.5s 重复广播。 */
        if (s_btn_req) {
            int k = s_btn_req;
            s_btn_req = 0;
            s_btn_obj = (k == 2) ? MB_OBJ_BTN_DOUBLE
                      : (k == 3) ? MB_OBJ_BTN_HOLD
                                 : MB_OBJ_BTN_CLICK;
            s_btn_until = t + 2500;
            s_fcnt++;
            if (build_frame(s_idx) >= 0) adv_refresh();
            ESP_LOGW(TAG, "btn: 0x%04X %s(%s键)，重复广播 2.5s",
                     s_btn_obj, (k == 2) ? "双击" : (k == 3) ? "长按" : "单击",
                     (s_btn_key == 2) ? "右" : "左");
        } else if (s_btn_obj && (int32_t)(t - s_btn_until) >= 0) {
            s_btn_obj = 0;
            s_fcnt++;
            if (build_frame(s_idx) >= 0) adv_refresh();
            ESP_LOGI(TAG, "btn: 事件结束，回到电量帧");
        }

        /* near 事件窗口：每 220ms 换一帧计数重发（网关去重，内容必须变），
         * 到点回落电量帧。整段期间 adv 都是 connectable，网关随时能连。 */
        if (s_ev_obj && (int32_t)(t - s_ev_next) >= 0) {
            if ((int32_t)(t - s_ev_until) >= 0) {
                s_ev_obj = 0;
                s_fcnt++;
                if (build_frame(s_idx) >= 0) adv_refresh();
                ESP_LOGW(TAG, "near: 窗口结束，回到电量帧");
            } else {
                s_ev_next = t + 220;
                s_fcnt++;
                if (build_frame(s_idx) >= 0) adv_refresh();
            }
        }

        int fast_now = s_fast_until && (int32_t)(t - s_fast_until) < 0;
        if (fast_window && !fast_now) {
            /* 唤醒窗口结束：重建一帧把 solicited 位清掉，再按常规间隔重开广播 */
            fast_window = 0;
            if (!gatt_miot_is_connected()) {
                s_fcnt++;
                build_frame(s_idx);
                adv_refresh();
            }
        }
        if (s_adv_req) {
            s_adv_req = 0;
            if (!gatt_miot_is_connected()) {
                s_fast_until = t + ADV_FAST_MS;
                fast_now = 1;
                fast_window = 1;
                last_adv = t;
                /* ★必须让空中帧真的变：只改间隔不改内容，App 看不出「按钮被按过」。
                 * 真机按下蓝牙按钮会刷新帧计数，App 靠广播内容变化确认设备已唤醒。 */
                s_fcnt++;
                sim_tick();
                if (build_frame(s_idx) >= 0) {
                    char hex[3 * 16 + 4];
                    size_t o = 0;
                    for (int k = 0; k < s_flen && k < 16; k++)
                        o += (size_t)snprintf(hex + o, sizeof(hex) - o, "%02X ", s_frame[k]);
                    ESP_LOGW(TAG, "pair adv: 快广播 %dms, cnt=%lu, frame=%s",
                             ADV_FAST_MS, (unsigned long)s_fcnt, hex);
                } else {
                    ESP_LOGW(TAG, "pair adv: frame build failed");
                }
                adv_refresh();
            }
        }

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
            /* 快广播窗口内保持广播连续，不做 stop/start —— 空档就是连接失败的窗口 */
            uint32_t per = fast_now ? UINT32_MAX
                                    : (s_auth.bound ? ADV_PERIOD_MS : ADV_PERIOD_UNBOUND_MS);
            if (t - last_adv >= per) {
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

        /* 诊断看门狗：每 30s 一行，回答三个问题——
         * 1) Mesh 扫描有没有在跑（disc 计数持续增长 = 在扫）
         * 2) 手机/网关有没有发配网 PDU（prov=0 就是人家没来找我们）
         * 3) 未配网 beacon 有没有真的发出去（adv 是 mesh 持有还是我们持有） */
        {
            static uint32_t last_stat_ms;
            if (t - last_stat_ms >= 30000) {
                last_stat_ms = t;
                ESP_LOGW(TAG, "STAT disc=%lu prov_pdu=%lu mesh_prov_on=%d provisioned=%d adv_mine=%d adv_act=%d conn=%d ev=%d/%d heap=%lu",
                         (unsigned long)s_disc_cnt, (unsigned long)s_prov_pdu_cnt,
                         (int)mesh_miot_prov_active(), (int)mesh_miot_provisioned(),
                         (int)(!mesh_miot_adv_owns_radio()), (int)ble_gap_adv_active(),
                         (int)gatt_miot_is_connected(),
                         ble_npl_ev_live_count(), ble_npl_ev_total_count(),
                         (unsigned long)esp_get_free_heap_size());
                {
                    void *ras[24]; int cns[24];
                    int hn = ble_npl_ev_hist(ras, cns, 24);
                    for (int i = 0; i < hn; i++) {
                        if (cns[i] >= 4) {
                            ESP_LOGW(TAG, "  ev-alloc from %p x%d", ras[i], cns[i]);
                        }
                    }
                }
            }
        }

        /* BOOT 短按 -> 称一次。必须等 App 登录（会话密钥就绪），
         * 否则事件加密发不出去，App 那边就是「没反应」。 */
        if (s_weigh_req) {
            s_weigh_req = 0;
            if (s_sel != 2) {
                ESP_LOGW(TAG, "BOOT 称重只对档案 [2] S400 有效（当前 sel=%d）", s_sel);
            } else if (!s_auth.logged_in) {
                /* ★未登录 = 手机不在场，这正是说明书里的「离线称重模式」。
                 * 真机此时照样完成称重、把数据存在秤里，并让广播进入「请求被连接」
                 * 形态，等网关/手机来取（说明书绑定步骤④：手机与秤未连接时，
                 * 秤会自动连接蓝牙网关）。我们做同样两件事：
                 *   1) 体重/阻抗写进属性表 → 谁连上来 GET 3.1005 / 3.1006 就能读到；
                 *   2) 广播 solicited 置 1，窗口 10 分钟（见 build_frame 里的 0x5B）。
                 * 事件帧（8.1/8.2/11.1022）此刻发不出去 —— 没有会话密钥，
                 * 这与真机「数据先存秤里、等下次连接再上传」是同一个状态。 */
                s_ww   = k_wcycle[s_widx][0];
                s_wimp = k_wcycle[s_widx][1];
                s_widx = (s_widx + 1) % (int)(sizeof(k_wcycle) / sizeof(k_wcycle[0]));
                s_wts  = scale_now();
                s_last_weigh = t;
                scale_s400_store_offline(s_ww, s_wimp);
                s_solicit_until = t + 10u * 60u * 1000u;
                adv_refresh();
                ESP_LOGW(TAG, "offline weigh-in %u.%03ukg imp=%u (no session) -> "
                              "adv solicited=1, gateway may read 3.1005",
                         (unsigned)(s_ww / 1000u), (unsigned)(s_ww % 1000u),
                         (unsigned)s_wimp);
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
