/**
 * @file app_main.c
 * @brief 米家体脂秤 S400（MJTZC01YM / PID 0x30D9）—— 可绑定版最小固件
 *
 * 目标：一根 Type-C 线烧进去，米家 App 能添加、能打开设备页、能"称一次"并出
 * 完整体重/体脂报告（含云端历史）。
 *
 * 与 btn1_adv / t3pro_adv 同一套骨架（认证 + GATT + 广播），差异在这里：
 *   1) 秤是**连接型**设备：App 靠 GATT 会话拿数据，广播只用来发电量和"请求被连接"
 *   2) 一次称重 = 7 帧事件，必须**逐帧节流**（App 的 ag2 窗口同一毫秒只吃一帧）：
 *        8.1 过程流 ×3 → 8.2 结果 → 8.1 done → 11.1022 → 11.2/11.3 属性变更
 *      节流表见 k_wgap；点「测量」按钮触发时用更慢的 k_wgap_slow（App 要 4~5s
 *      才把界面切到"请上秤"，太早发等于全丢）
 *   3) 13.1 sync 必须回 5 个 out 参数（离线消息数/SN/MAC/固件/电量），
 *      否则 App 开屏卡住不动 —— 见 miot_device_action_out 的强符号实现
 *
 * 三种触发称重的方式：
 *   · 板载 BOOT 短按 —— 等价真机"人踩上去"，手机在连着就立刻推序列；
 *     手机不在场（无会话）则走"离线称重"：数值写进属性表 + 广播置 solicited，
 *     等网关/手机连上来 GET 3.1005/3.1006
 *   · App 里点「测量」—— 插件不给设备发指令，唯一痕迹是 ACTION 10.1，
 *     收到后等 4s 用慢节奏推（miot_demo_app_action_hook）
 *   · 串口没有额外命令：就这两种，够用
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

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

static const char *TAG = "SCALE";

/* ★改参数只动这一块 */
#define SCALE_PID      0x30D9      /* 米家体脂秤 S400（国区） */
#define SCALE_NAME     "MJJ"       /* 扫描响应名；真机也是这个短名 */
#define ADV_PERIOD_MS  3000
#define NVS_NS         "scale"     /* 与其他工程隔离，避免串 did/bind_key；换身份用 erase-flash */
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"
#define NVS_KEY_OFFQ   "offq"      /* 离线称重队列（官方说明真机可存 160 条，我们存 8 条） */

/* MAC 开关：1 = 用派生 MAC（推荐），0 = 用芯片出厂物理 MAC。
 * 米家按 MAC 认设备：同一个 MAC 只要在账号里绑过（这颗芯片跑过 t3pro_adv，
 * 已被绑成温湿度计 3 Pro），「附近设备」就会把它当"已添加的设备"过滤掉 → 扫不到。
 * 换一颗从未绑定过的板子时可以置 0。 */
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4    0x5C        /* 第 5 字节：体脂秤专属（btn2_adv 用 0x5B） */
#define BASE_MAC_B5    0x01        /* 末字节 0x01 → 空中 BT MAC 末字节 0x02 */
#endif

#define BOOT_PIN       9
#define SOLICIT_MS     (10u * 60u * 1000u)   /* 离线称重后"请求被连接"窗口：10 分钟 */

/* 离线补传时 8.2 的 weightDoneStatus（下标 5）——「待认领数据」的开关：
 *   0 = normalData：App 把这条直接算到当前用户，进历史。BLE 直连唯一可用路径。
 *   2 = userError ：App 弹「是你吗/选择用户」，不认领就整条丢弃（15:45 实测：
 *       弹窗关不掉，强杀 App 后记录消失，待认领仍空）。官方「待认领数据」实际靠
 *       云端/网关上传通道产生，见 docs/known-issues.md §2.2.1。
 * 所以：接上真网关/云端取数、且 App 侧能落「待认领」时，才把这里改成 2。 */
#define OFFQ_REPLAY_STATUS  0

/* 7 步节流的帧间隔：过程流 250ms 一跳，结果前 400ms，收尾各 900ms */
static const uint16_t k_wgap[SCALE_S400_STEPS] = { 250, 250, 250, 400, 900, 900, 0 };
/* 点「测量」触发的慢节奏：App 要 4~5s 才切到"请上秤"，等待期发出去的全丢 */
static const uint16_t k_wgap_slow[SCALE_S400_STEPS] = { 1800, 1800, 1200, 900, 900, 300, 0 };

/* 每按一次换一组数据，方便在历史里看到多条独立记录（体重=克，阻抗=0.1Ω） */
static const uint32_t k_wcycle[][2] = {
    { 64500u, 500u },   /* 64.5kg */
    { 63800u, 460u },   /* 63.8kg */
    { 65200u, 540u },   /* 65.2kg */
};
static int s_widx;

/* 离线称重队列：官方说明真机最多存 160 条，这里 8 条环形队列 + NVS 持久化 */
#define OFFQ_N 8
typedef struct { uint32_t w, imp, ts; uint8_t st; } off_rec_t;   /* st=8.2 weightDoneStatus */
typedef struct {
    uint8_t   n;                /* 有效条数 0..OFFQ_N */
    uint8_t   head;             /* 最老一条的下标（满时新记录覆盖 head，等效丢最老） */
    off_rec_t rec[OFFQ_N];
} off_q_t;
static off_q_t s_offq;

static uint32_t s_fcnt;
static uint8_t  v_batt = 92;
static uint32_t s_solicit_until;        /* 离线称重后的 solicited 窗口截止（ms） */

/* 称重状态机 */
static int      s_wstep = -1;
static uint32_t s_wnext, s_ww, s_wimp, s_wts;
static uint16_t s_wdelay;
static const uint16_t *s_wgap_p = k_wgap;
static volatile int s_weigh_req;
static volatile int s_click_pending;

/* 离线补传状态机：手机下次连上来时，把离线那次称重按 8.2 → done → 11.1022 → 11.2/11.3
 * 重放一遍。真机就是这么"自动上传"的（说明书 §3），不重放的话 App 只是读到 3.1005
 * 一个静态值，历史里不会多出一条记录。 */
static int      s_rep_step = -1;
static uint32_t s_rep_next, s_rep_ww, s_rep_imp, s_rep_ts;
static uint8_t  s_rep_st;           /* 该条的 weightDoneStatus：0=历史 / 2=待认领 */

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static uint8_t  s_flen;
static int      s_was_bound;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/** 板上没有 RTC：用编译时刻当锚点 + 运行时长，误差 = 编译到上电的间隔。
 *  填 0 云端判非法（报告页 1970 + 服务器异常）；__TIME__ 本身是 UTC，别再减 8h。 */
static uint32_t build_epoch(void)
{
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    const char *t = __TIME__;
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    int day = (d[4] == ' ') ? (d[5] - '0') : ((d[4] - '0') * 10 + (d[5] - '0'));
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

/** 手机上电后 13.1 sync 会给一次真实时间，用它的偏移把编译锚点掰正。
 *  编译锚点本身只保证"量级对"（编译到上电的间隔恒等于误差），
 *  实测误差就是 26 分钟（14:09 编译、14:35 上电），App 记录串全飘。 */
static int32_t s_ts_off;

static uint32_t scale_now(void)
{
    return (uint32_t)((int64_t)build_epoch() + now_ms() / 1000 + s_ts_off);
}

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

/* ---------------- 离线称重队列（NVS 持久化，断电不丢） ---------------- */
static void offq_sync_count(void) { scale_s400_offline_set_count(s_offq.n); }

static void offq_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_OFFQ, &s_offq, sizeof(s_offq));
    nvs_commit(h);
    nvs_close(h);
}

static void offq_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_offq);
    memset(&s_offq, 0, sizeof(s_offq));
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, NVS_KEY_OFFQ, &s_offq, &len) != ESP_OK || len != sizeof(s_offq)) {
        memset(&s_offq, 0, sizeof(s_offq));
    } else if (s_offq.n > OFFQ_N || s_offq.head >= OFFQ_N) {
        memset(&s_offq, 0, sizeof(s_offq));     /* 旧版本/损坏 blob，别带脏数据 */
    }
    nvs_close(h);
    if (s_offq.n)
        ESP_LOGI(TAG, "offline queue restored: %u record(s)", (unsigned)s_offq.n);
    offq_sync_count();
}

/** 入队（满时丢最老一条），并同步 13.6 计数。
 *  st = 8.2 的 weightDoneStatus：0=已识别（进历史）/ 2=未识别（进「待认领」） */
static void offq_push(uint32_t w, uint32_t imp, uint32_t ts, uint8_t st)
{
    uint8_t tail;
    if (s_offq.n == OFFQ_N) {               /* 满：丢最老 */
        s_offq.head = (uint8_t)((s_offq.head + 1) % OFFQ_N);
        s_offq.n--;
        ESP_LOGW(TAG, "offline queue full -> dropped oldest");
    }
    tail = (uint8_t)((s_offq.head + s_offq.n) % OFFQ_N);
    s_offq.rec[tail].w = w;
    s_offq.rec[tail].imp = imp;
    s_offq.rec[tail].ts = ts;
    s_offq.rec[tail].st = st;
    s_offq.n++;
    offq_save();
    offq_sync_count();
}

/** 弹出最老一条（补传完成后调用），并同步 13.6 计数 */
static void offq_pop(void)
{
    if (!s_offq.n) return;
    s_offq.head = (uint8_t)((s_offq.head + 1) % OFFQ_N);
    s_offq.n--;
    offq_save();
    offq_sync_count();
}

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
 * 秤没有能装体重的 Property 对象，第三方网关无从解析，所以广播只有两种用途：
 *   1) 未绑定：hello + solicited，让 App 能发起连接
 *   2) 已绑定：电量心跳；离线称重后 10 分钟内额外置 solicited（FC 0x5B），
 *      等于对外喊"我有数据要交"，网关靠这一位决定要不要主动建链 */
static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = SCALE_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;      /* solicited=1：App 就等这一位 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);
    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
        return fl;
    }
    if (s_solicit_until && (int32_t)(s_solicit_until - now_ms()) > 0) s_frame[1] = 0x5B;
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
    mfr[m++] = (uint8_t)(SCALE_PID & 0xFF);
    mfr[m++] = (uint8_t)(SCALE_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)SCALE_NAME;
    rsp.name_len = sizeof(SCALE_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    /* 秤必须随时可被连：添加、开屏同步、上秤都靠手机主动建链 */
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
        gatt_miot_on_subscribe(event->subscribe.attr_handle,
                               event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    uint8_t addr[6];
    if (ble_hs_util_ensure_addr(0) == 0 &&
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "host synced, MAC %02X:%02X:%02X:%02X:%02X:%02X  bound=%d",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0], s_auth.bound);

    build_frame();
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

/* ---------------- 设备层接管钩子（SDK 里的弱符号） ---------------- */

/** App 发 ACTION siid=10 aiid=1 —— 唯一能观测到的"用户点了测量"的信号。
 *  在 NimBLE 任务里回调，只置标志，真正的发送交给主循环。 */
void miot_demo_app_action_hook(uint8_t siid, uint16_t aiid)
{
    if (siid != 10 || aiid != 1) return;
    if (!s_auth.logged_in) return;
    if (s_wstep >= 0 || s_weigh_req) return;
    ESP_LOGW(TAG, "App ACTION 10.1 = 用户点了「测量」-> 4s 后用慢节奏推序列");
    s_click_pending = 1;
    s_weigh_req = 1;
}

/** 13.1 sync 的入参里第 3 项（piid=4）是手机当前时间 —— 板子没有 RTC，
 *  这是唯一能拿到真实时间的入口。拿到就把编译锚点的偏差整段掰掉。 */
void miot_demo_action_args_hook(uint8_t siid, uint16_t aiid,
                                const miot_spec_value_t *args, int argc)
{
    if (siid != 13 || aiid != 1) return;
    for (int i = 0; i < argc; i++) {
        const miot_spec_value_t *a = &args[i];
        if (a->piid != 4) continue;
        uint32_t phone = a->val.u32;
        if (phone < 1600000000u) continue;      /* 明显不是 unix 秒，别把时钟带跑 */
        uint32_t local = build_epoch() + now_ms() / 1000u;
        s_ts_off = (int32_t)(phone - local);
        scale_s400_set_now(phone);
        ESP_LOGW(TAG, "time sync from phone: %u (local est %u, off %+lds)",
                 (unsigned)phone, (unsigned)local, (long)s_ts_off);
        return;
    }
}

/** 13.1 sync 的 5 个 out 参数：离线消息数 / SN / MAC / 固件 / 电量。
 *  ★App 开屏第一件事就是发这条动作，应答缺 out 时同步流程不推进。 */
int miot_device_action_out(uint8_t siid, uint16_t aiid,
                           miot_spec_value_t *outs, int max)
{
    return scale_s400_action_out(siid, (uint8_t)aiid, outs, max);
}

/* ---------------- BOOT 键：等价"人踩上去" ---------------- */
static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (gpio_get_level(BOOT_PIN) == 0) {
            int ms = 0;
            while (gpio_get_level(BOOT_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                ms += 100;
                if (ms >= 5000) {
                    ESP_LOGW(TAG, "BOOT held 5s -> erase NVS (factory reset)");
                    nvs_flash_erase();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            if (ms >= 60) s_weigh_req = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

#if USE_CUSTOM_MAC
    /* ★必须在任何 BLE/协议栈初始化之前落定 MAC：米家按 MAC 认设备 */
    {
        uint8_t base[6] = { 0x10, 0x00, 0x3B, 0xCE, BASE_MAC_B4, BASE_MAC_B5 };
        esp_base_mac_addr_set(base);
    }
#endif

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    /* 13.1 应答里的身份：真机从 flash 读，这里由 MAC 派生 */
    {
        char mstr[18], snstr[24];
        snprintf(mstr, sizeof(mstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(snstr, sizeof(snstr), "S400%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
        scale_s400_set_identity(mstr, snstr, "1.0.0_0031");
    }

    miot_port_esp32_crypto_init();
    fcnt_load();
    offq_load();        /* 恢复离线称重队列 + 13.6 计数（断电不丢，官方说明真机 160 条） */

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    /* ★scale_s400_init 用的是真机验证过的属性表：
     *   3.1003 电量 / 3.1004 单位 / 3.1005 体重(kg×10) / 3.1006 阻抗
     *   10.1 work-mode / 13.x syncinfo（开屏 GET 的那些）  */
    miot_prop_t *tbl; size_t cnt;
    scale_s400_init(&s_dev, &s_port, &tbl, &cnt);
    ESP_LOGI(TAG, "spec table: %u props (3.1005 weight / 3.1006 impedance / 13.x sync)", (unsigned)cnt);

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
    ble_svc_gap_device_name_set(SCALE_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    gatt_miot_set_pid(SCALE_PID);   /* 0x0002 只读特征，App 靠它认型号 */
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0, last_clock = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        /* ★登录完成后必须把会话密钥交给 spec 层。
         * miot_device_send_event() 开头是 `if (!d->session_ok) return -1;`，
         * 不调这一步 → 7 帧称重数据全是 rc=-1：日志里"连上了、也在称"，
         * App 端却一片空白（2026-11-14 实测踩到）。断开时 session_reset 会清回去，
         * 所以每次重连都要重新交一次。 */
        if (s_auth.logged_in && !s_dev.session_ok) {
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGW(TAG, "*** session established: spec channel ready (mtu=%u) ***",
                     (unsigned)gatt_miot_mtu());
            /* ★立刻吐一帧：App 订阅完 spec 通道后最多等 1s，收不到任何东西就
             * 退订重连，页面永远停在"设备初始化中"。 */
            scale_s400_push_status(&s_dev);
            ESP_LOGI(TAG, "initial status pushed (weight/time/battery)");
            /* ★离线补传：说明书 §3「数据将在下一次连接手机时自动上传」。
             * 只写属性表是不够的 —— App 要走事件流程才会生成历史记录。 */
            if (s_offq.n && s_rep_step < 0) {
                const off_rec_t *r = &s_offq.rec[s_offq.head];
                s_rep_ww = r->w; s_rep_imp = r->imp; s_rep_ts = r->ts; s_rep_st = r->st;
                s_rep_step = 0;             /* ★整段 7 帧重放：8.1 过程流三轮 → 8.2 → done
                                             *   → 11.1022 → 11.2/3。只发 8.2 时 App 只
                                             *   ACK 但不出记录（15:05 实测），插件要靠
                                             *   8.1 才能进"称重中"状态。 */
                s_rep_next = t + 5000;      /* 等插件把订阅/13.1 sync 做完（实测 13.1 会发两次） */
                ESP_LOGW(TAG, "offline pending: %u.%03ukg imp=%u ts=%u status=%u (queued=%u) -> replay after 5s",
                         (unsigned)(s_rep_ww / 1000u), (unsigned)(s_rep_ww % 1000u),
                         (unsigned)s_rep_imp, (unsigned)s_rep_ts, (unsigned)s_rep_st,
                         (unsigned)s_offq.n);
            }
        }

        /* 13.4 current-time：每秒刷一次，App 用它给记录打时间戳 */
        if (t - last_clock >= 1000) {
            last_clock = t;
            scale_s400_set_now(scale_now());
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame();
            adv_refresh();
        }

        /* solicited 窗口到期：广播回落普通电量帧 */
        if (s_solicit_until && (int32_t)(t - s_solicit_until) >= 0) {
            s_solicit_until = 0;
            ESP_LOGI(TAG, "solicit window closed -> adv back to 0x59");
            s_fcnt++;
            if (build_frame() >= 0) adv_refresh();
        }

        /* 称重请求 */
        if (s_weigh_req) {
            s_weigh_req = 0;
            if (!s_auth.logged_in) {
                /* 手机不在场 = 说明书里的"离线称重"：入队（NVS 持久化），
                 * 广播置 solicited 10 分钟，等手机下次连上来整段补传。 */
                s_ww   = k_wcycle[s_widx][0];
                s_wimp = k_wcycle[s_widx][1];
                s_widx = (s_widx + 1) % (int)(sizeof(k_wcycle) / sizeof(k_wcycle[0]));
                s_wts = scale_now();
                scale_s400_store_offline(s_ww, s_wimp, s_wts);
                /* 默认 normalData：App 直接归到当前用户历史。想走「待认领」就把
                 * OFFQ_REPLAY_STATUS 改成 2（需云端/网关取数通道，BLE 直连会丢数据）。 */
                offq_push(s_ww, s_wimp, s_wts, OFFQ_REPLAY_STATUS);
                s_solicit_until = t + SOLICIT_MS;
                s_fcnt++;
                adv_refresh();
                ESP_LOGW(TAG, "offline weigh-in %u.%03ukg imp=%u queued=%u -> solicited=1 10min",
                         (unsigned)(s_ww / 1000u), (unsigned)(s_ww % 1000u), (unsigned)s_wimp,
                         (unsigned)s_offq.n);
            } else if (s_wstep < 0) {
                int from_click = s_click_pending;
                s_click_pending = 0;
                s_wstep  = 0;
                s_wdelay = from_click ? 4000 : 0;
                s_wgap_p = from_click ? k_wgap_slow : k_wgap;
                s_wnext  = t + s_wdelay;
                s_ww   = k_wcycle[s_widx][0];
                s_wimp = k_wcycle[s_widx][1];
                s_widx = (s_widx + 1) % (int)(sizeof(k_wcycle) / sizeof(k_wcycle[0]));
                s_wts  = scale_now();
                ESP_LOGW(TAG, "weigh-in start %u.%03ukg imp=%u ts=%u (%s)",
                         (unsigned)(s_ww / 1000u), (unsigned)(s_ww % 1000u), (unsigned)s_wimp,
                         (unsigned)s_wts, from_click ? "click" : "boot");
            }
        }

        /* 7 步序列：一帧一步，按节奏表节流，绝不连发 */
        if (s_wstep >= 0 && (int32_t)(t - s_wnext) >= 0) {
            int st = s_wstep;
            int rc = scale_s400_report_step(&s_dev, st, s_ww, s_wimp, s_wts);
            ESP_LOGI(TAG, "weigh step %d/%d rc=%d", st + 1, SCALE_S400_STEPS, rc);
            if (++s_wstep >= SCALE_S400_STEPS) {
                s_wstep = -1;
                ESP_LOGW(TAG, "weigh-in done (7/7 frames)");
            } else {
                s_wnext = t + s_wgap_p[st];
            }
        }

        /* 离线补传：整段 7 帧，节奏与在线称重完全一致；一条传完接着下一条 */
        if (s_rep_step >= 0 && s_wstep < 0 && (int32_t)(t - s_rep_next) >= 0) {
            int st = s_rep_step;
            int rc = scale_s400_report_step_ex(&s_dev, st, s_rep_ww, s_rep_imp, s_rep_ts, s_rep_st);
            ESP_LOGW(TAG, "offline replay step %d/%d rc=%d", st + 1, SCALE_S400_STEPS, rc);
            if (++s_rep_step >= SCALE_S400_STEPS) {
                offq_pop();
                if (s_offq.n) {
                    const off_rec_t *r = &s_offq.rec[s_offq.head];
                    s_rep_ww = r->w; s_rep_imp = r->imp; s_rep_ts = r->ts; s_rep_st = r->st;
                    s_rep_step = 0;
                    s_rep_next = t + 900;
                    ESP_LOGW(TAG, "offline replay next: %u.%03ukg status=%u (queued=%u)",
                             (unsigned)(s_rep_ww / 1000u), (unsigned)(s_rep_ww % 1000u),
                             (unsigned)s_rep_st, (unsigned)s_offq.n);
                } else {
                    s_rep_step = -1;
                    ESP_LOGW(TAG, "offline replay all done -> queue empty, 13.6=0");
                }
            } else {
                s_rep_next = t + k_wgap[st];
            }
        }

        /* 广播心跳：断连期间按 ADV_PERIOD_MS 刷新，fcnt 递增防重放过滤 */
        if (!gatt_miot_is_connected() && t - last_adv >= ADV_PERIOD_MS) {
            last_adv = t;
            s_fcnt++;
            if ((s_fcnt % 10) == 0) fcnt_save();
            if (build_frame() >= 0) adv_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
