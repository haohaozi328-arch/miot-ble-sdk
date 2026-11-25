/**
 * @file app_main.c
 * @brief 米家八电极体脂秤 S800（MJTZC04YM / xiaomi.scales.ms116 / PID 0x51E2）—— 小白版最小固件
 *
 * ─────────────────────────────────────────────────────────────────────
 * 为什么这只能做「广播型」：S800 的称重数据走 **MiBeacon 事件 3.e.1022**，
 * 不是 GATT 会话里推的。证据是 XiaomiGateway3 给这台设备配了专属解码器
 * `BLEScaleS800`（converters/devices.py 20962 条目 → mibeacon.py）：
 *
 *   MapConv("battery_low", mi="4.e.1001.p.1")
 *   BLEScaleS800("p2", mi="3.e.1022.p.2")   ← 阶段帧
 *   BLEScaleS800("p3", mi="3.e.1022.p.3")   ← 数据帧
 *
 * 对象号按 MiBeacon2 公式（与 pir2s / occ 同一套）：
 *   eid = 0x4000 | (0x200 事件) | (siid << 10) | (iid - 1000)
 *   3.e.1022 → 0x4000|0x200|0xC00|22 = 0x4E16
 *
 * 事件载荷 = 参数按 spec 顺序打包，每个参数 4B（uint32）：
 *   p.2 = (kind<<24)|(stage<<16)|data   —— 阶段：0x05 称重 / 0x15 心率
 *   p.3 = 该阶段的数据
 *
 * 解码规则（GW3 实测；p.3 的语义由「上一个 p.2 阶段」决定）：
 *   上一个 stage==0x05 → weight = ((p3>>16)&0xFFFF)/100 kg
 *   上一个 stage==0x15 → heart_rate = ((p3>>24)&0xFF) + 50
 *
 * spec（home.miot-spec.com/spec/xiaomi.scales.ms116）：
 *   2 秤   2.p.1 单位 / 2.p.2 心率检测 / 2.p.3 当前时间
 *   3 BLE 私有 3.p.1~3 / 3.e.1022
 *   4 电池 4.p.1 电量 / 4.p.1004 状态 / 4.e.1001 低电量
 *   9 离线数据 / 10 在线数据 / 13 sync-info / 14 分享用户
 *
 * 操作（板载 BOOT 键）：
 *   ① 短按 —— 模拟一次上秤称重（阶段帧 + 数据帧，含心率）
 *   ② 双击 —— 切换模拟体重档位（50.00 / 64.50 / 80.00 kg）
 *   ③ 长按 5 秒 = 恢复出厂
 * ─────────────────────────────────────────────────────────────────────
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

#include "gatt_miot.h"

void miot_port_esp32_crypto_init(void);

static const char *TAG = "S800";

/* ★★★ 改参数只动这一块 ★★★ */
#define S800_PID        0x51E2          /* pdid 20962 = xiaomi.scales.ms116 */
#define S800_NAME       "S800"

#define NVS_NS          "s800"
#define NVS_KEY_ID      "ident"
#define NVS_KEY_FCNT    "fcnt"

/* 派生 MAC 后缀（在用：5D door/5E motion/5F leak/61 hcho/70 nightl2/71 bulb/
 * 72 v5kwm/73 pir2s/74 occ → 本工程 0x75）*/
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4     0x75
#define BASE_MAC_B5     0x01
#endif

#define BOOT_PIN        9
#define ADV_PERIOD_MS   3000            /* 空闲轮换间隔（秤是低频设备）*/
#define EVENT_GAP_MS    250             /* 事件帧间隔 */
#define EVENT_REPEAT    3
#define SOLICIT_MS      (10u * 60u * 1000u)
#define WAKE_WINDOW_MS  120000
#define ADV_CONN_ALWAYS 1

/* ── 对象号 ── */
#define S800_OBJ_MEAS_EV   0x4E16       /* 事件 3.e.1022：8B = p2(u32)+p3(u32) */
#define S800_OBJ_BATTERY   0x100A       /* 电量标准对象 1B u8 %（继承 pir2s 教训）*/

/* 阶段码（GW3 解码器里写死的那两个）*/
#define STAGE_WEIGHT    0x05
#define STAGE_HEART     0x15

/* 模拟体重档位（0.01kg）*/
static const uint32_t k_weight_tab[] = { 5000u, 6450u, 8000u };   /* 50 / 64.5 / 80 kg */
#define WEIGHT_TAB_N  (sizeof(k_weight_tab) / sizeof(k_weight_tab[0]))

static uint32_t s_fcnt;
static uint8_t  v_batt   = 92;
static uint8_t  v_batt_lo = 0;
static uint8_t  v_unit   = 0;           /* 2.p.1 0=kg 1=lb */
static uint8_t  v_hr_detect = 1;        /* 2.p.2 */
static uint32_t v_time_s = 0;           /* 2.p.3 current-time（秒）*/
static uint8_t  v_cust5  = 0;           /* 3.p.1 */
static uint32_t v_cust4  = 0;           /* 3.p.2 */
static uint32_t v_cust3  = 0;           /* 3.p.3 */
static uint8_t  v_work_mode = 0;        /* 10.p.1 */
static uint8_t  v_work_status = 0;      /* 10.p.2 */
static uint32_t v_work_u16 = 0;         /* 10.p.3 */
static uint8_t  v_off_cnt = 0;          /* 9.p.3 / 13.p.6 */
static uint8_t  v_plugin_ver = 1;       /* 13.p.2 */
static uint8_t  v_sn_ver = 1;           /* 13.p.7 */
static uint32_t s_weight = 6450;        /* 当前模拟体重（0.01kg）*/
static uint8_t  s_weight_idx = 1;
static uint8_t  s_hr = 72;

/* ── 事件帧队列：一次称重 = 阶段帧 + 数据帧 交替，必须严格按序播 ── */
#define Q_MAX 8
static struct {
    uint8_t len;
    uint8_t data[8];
    uint8_t repeat;
} s_q[Q_MAX];
static int s_q_n, s_q_head, s_q_left;

static uint32_t s_wake_until;

/* App 里点「测量」只会发 ACTION 10.1（set-mode），插件不给设备别的指令；
 * 记下时刻，4s 后由主循环推一次称重序列（App 要 4~5s 才把界面切到「请上秤」）。*/
static uint32_t s_measure_at;

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static int      s_idx;
static uint8_t  s_flen;
static int      s_logged_in;
static int      s_was_bound;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool win_active(void)
{
    return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0;
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
    ESP_LOGI(TAG, "绑定身份已保存（did %02X %02X %02X %02X）",
             id->did[0], id->did[1], id->did[2], id->did[3]);
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
    ESP_LOGI(TAG, "读到已保存的绑定身份（did %02X %02X %02X %02X）",
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

/* ---------------- 广播帧 ----------------
 * 未绑定：只发身份帧（hello）。已绑定：加密帧，一次一个对象。
 * ★用「无 MAC 形态」：真机 27B 心跳帧就是因为带 MAC 才超 26B 上限。 */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = S800_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                        s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;      /* bit9：请求被连接，App 才会发起连接 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;

    if (s_q_left > 0 && s_q_n > 0) {
        n = miot_beacon_obj_bytes(objs, S800_OBJ_MEAS_EV,
                                  s_q[s_q_head].data, s_q[s_q_head].len);
    } else {
        switch (idx % 2) {
        case 0:                                     /* 电量 */
            n = miot_beacon_obj_u8(objs, S800_OBJ_BATTERY, v_batt);
            break;
        default: {                                  /* 空闲心跳：全 0 阶段帧（待上秤）*/
            uint8_t d[8] = { 0 };
            n = miot_beacon_obj_bytes(objs, S800_OBJ_MEAS_EV, d, 8);
            break;
        }
        }
    }

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "加密帧失败 rc=%d（加密后端没初始化？）", fl);
        return fl;
    }
    if (win_active()) s_frame[1] = 0x5B;
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
        ESP_LOGE(TAG, "广播字段设置失败 rc=%d（帧 %dB，超 26B 上限？）", rc, n);
        return;
    }

    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(S800_PID & 0xFF);
    mfr[m++] = (uint8_t)(S800_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)S800_NAME;
    rsp.name_len = sizeof(S800_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "扫描响应设置失败");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    /* ★-28 修复（继承 pir2s）：连接期间必须保持「请求配对」形态广播，
     *   连接态 NimBLE 发不了可连接广播，改发同帧内容的非连接 beacon。 */
    if (gatt_miot_is_connected()) {
        p.conn_mode = BLE_GAP_CONN_MODE_NON;
    } else {
        p.conn_mode = BLE_GAP_CONN_MODE_UND;
    }
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (win_active()) {
        p.itvl_min = 48;
        p.itvl_max = 80;
    } else {
        p.itvl_min = s_auth.bound ? 800 : 160;
        p.itvl_max = s_auth.bound ? 800 : 320;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
}

static void adv_refresh(void)
{
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
            build_frame(s_idx);
            adv_refresh();
            ESP_LOGI(TAG, "手机连上来了（切非连接 beacon 继续广播）");
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
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
        adv_refresh();
        ESP_LOGI(TAG, "手机断开（reason=0x%02x），恢复广播", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "App 订阅 attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        /* ★必须告诉 GATT 层 App 订阅的是哪条：应答要投回它写入的那条特征，
         * 否则 App 收不到 ACK，每 6s 重发同一帧、spec 请求一条都不发。 */
        gatt_miot_on_subscribe(event->subscribe.attr_handle,
                               event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

/* ---------------- 启动 ---------------- */
static void on_sync(void)
{
    uint8_t addr[6];
    if (ble_hs_util_ensure_addr(0) == 0 &&
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "协议栈就绪  MAC %02X:%02X:%02X:%02X:%02X:%02X  %s",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0],
             s_auth.bound ? "已绑定" : "未绑定（等待米家添加）");

    s_idx = 0;
    build_frame(s_idx);
    adv_start();
    s_was_bound = s_auth.bound;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- 事件队列 ---------------- */
static void q_push(const void *data, uint8_t len, uint8_t repeat)
{
    if (s_q_n >= Q_MAX) return;
    int i = (s_q_head + s_q_n) % Q_MAX;
    s_q[i].len = len;
    memcpy(s_q[i].data, data, len);
    s_q[i].repeat = repeat;
    s_q_n++;
}

static void kick_queue(void)
{
    if (s_q_n == 0) return;
    s_q_left = s_q[s_q_head].repeat;
    s_fcnt++;
    fcnt_save();
    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    build_frame(s_idx);
    adv_refresh();
}

/* 单字符串事件：ms116 的 10.e.3 / 10.e.4 参数都是 10.4 那条 string */
static int send_ev_str(uint8_t siid, uint16_t eiid, uint16_t piid, const char *s)
{
    miot_spec_value_t a;
    memset(&a, 0, sizeof(a));
    a.piid = piid;
    a.type = MIOT_TYPE_STRING;
    snprintf(a.val.str, sizeof(a.val.str), "%s", s);
    return miot_device_send_event(&s_dev, siid, eiid, &a, 1);
}

/* 一次完整称重：阶段帧(称重) → 数据帧(体重) → 阶段帧(心率) → 数据帧(心率) */
static void fire_weigh_in(void)
{
    uint8_t f[8];

    uint32_t p2 = ((uint32_t)STAGE_WEIGHT << 16);
    memcpy(f, &p2, 4); memset(f + 4, 0, 4);
    q_push(f, 8, EVENT_REPEAT);

    uint32_t p3 = (s_weight & 0xFFFFu) << 16;
    memset(f, 0, 4); memcpy(f + 4, &p3, 4);
    q_push(f, 8, EVENT_REPEAT);

    p2 = ((uint32_t)STAGE_HEART << 16);
    memcpy(f, &p2, 4); memset(f + 4, 0, 4);
    q_push(f, 8, EVENT_REPEAT);

    uint32_t hr = ((uint32_t)(s_hr - 50)) << 24;
    memset(f, 0, 4); memcpy(f + 4, &hr, 4);
    q_push(f, 8, EVENT_REPEAT);

    ESP_LOGW(TAG, ">>> 上秤：%.2f kg / %u bpm（0x4E16 阶段 %02X→%02X）",
             s_weight / 100.0, (unsigned)s_hr, STAGE_WEIGHT, STAGE_HEART);
    kick_queue();

    /* ★双通道：广播之外，已建立 spec 会话时再用 GATT 推一遍。
     *
     * ms116 的 App 通道是 **10 online-data**（不是 3.e.1022 —— 那是给网关解广播的）：
     *   10.e.3 realtime-data-report  实时体重流（参数 10.4 string，"kg×10,状态"）
     *   10.e.4 result-data-report    结果帧（28 字段 CSV，S400/8.2 同构）
     *   10.4  string                 数据帧载体属性（props_changed 同步一份）
     * 3.e.1022 保留：网关（XiaomiGateway3 的 BLEScaleS800）就是解这一条。
     *
     * ★必须逐帧节流：App 的 ag2 接收窗口同一毫秒只吃一帧，连发整帧丢（S400 实证）。 */
    if (s_dev.session_ok) {
        char realtime[48], result[120];
        uint32_t w10 = s_weight / 10;                  /* 0.01kg → kg×10 */
        uint32_t ts  = v_time_s;

        /* 格式沿用 S400/ms103 已实证的 "体重(kg×10),状态"：1=bodyWeight 稳定 */
        snprintf(realtime, sizeof(realtime), "%u,1", (unsigned)w10);
        /* 结果帧 28 字段：S400 插件按 [3]=weight [4]=hr [5]=doneStatus [6]=时间戳
         * [7]=体脂率 [8]=骨骼肌 取；S800 的八电极字段先按 0 占位，App 能出体重。 */
        snprintf(result, sizeof(result),
                 "0,0,0,%u,0,0,%u,185,275,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
                 (unsigned)w10, (unsigned)ts);

        int n1 = send_ev_str(10, 3, 4, realtime);
        ESP_LOGW(TAG, "GATT 10.e.3 实时：\"%s\" (rc=%d)", realtime, n1);
        vTaskDelay(pdMS_TO_TICKS(300));

        miot_spec_value_t pc;
        memset(&pc, 0, sizeof(pc));
        pc.siid = 10; pc.piid = 4; pc.type = MIOT_TYPE_STRING;
        snprintf(pc.val.str, sizeof(pc.val.str), "%s", result);
        miot_device_send_props_changed(&s_dev, &pc, 1);
        vTaskDelay(pdMS_TO_TICKS(250));

        int n2 = send_ev_str(10, 4, 4, result);
        ESP_LOGW(TAG, "GATT 10.e.4 结果：\"%s\" (rc=%d)", result, n2);
        vTaskDelay(pdMS_TO_TICKS(250));

        /* 网关路线：3.e.1022 两个 u32（p.2 阶段 / p.3 数据） */
        miot_spec_value_t a[2];
        memset(a, 0, sizeof(a));
        a[0].type = MIOT_TYPE_U32; a[1].type = MIOT_TYPE_U32;
        a[0].piid = 2; a[1].piid = 3;
        a[0].val.u32 = ((uint32_t)STAGE_WEIGHT << 16);
        a[1].val.u32 = (s_weight & 0xFFFFu) << 16;
        int n3 = miot_device_send_event(&s_dev, 3, 1022, a, 2);
        vTaskDelay(pdMS_TO_TICKS(250));
        a[0].val.u32 = ((uint32_t)STAGE_HEART << 16);
        a[1].val.u32 = ((uint32_t)(s_hr - 50)) << 24;
        int n4 = miot_device_send_event(&s_dev, 3, 1022, a, 2);
        ESP_LOGW(TAG, "GATT 3.e.1022 已推（%d/%d）", n3, n4);

        /* 测量结束：10.2 回 0，页面从「测量中」退出 */
        vTaskDelay(pdMS_TO_TICKS(250));
        v_work_status = 0;
        miot_spec_value_t st;
        memset(&st, 0, sizeof(st));
        st.siid = 10; st.piid = 2; st.type = MIOT_TYPE_U8; st.val.u8 = 0;
        miot_device_send_props_changed(&s_dev, &st, 1);
    }
}

/* ---------------- 按键（GPIO9 板载 BOOT）
 * 短按    —— 模拟一次称重
 * 双击    —— 切换体重档位
 * 长按 5s —— 恢复出厂 */
static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (gpio_get_level(BOOT_PIN) == 0) {
            int ms = 0;
            while (gpio_get_level(BOOT_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                ms += 50;
                if (ms >= 5000) {
                    ESP_LOGW(TAG, "长按 5 秒 → 恢复出厂（擦掉绑定身份，重启后可重新添加）");
                    nvs_flash_erase();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            if (ms < 60) continue;

            if (ms >= 1000) continue;           /* 1~5s 长按不定义动作 */

            bool dbl = false;
            for (int t0 = 0; t0 < 400; t0 += 20) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (gpio_get_level(BOOT_PIN) == 0) { dbl = true; break; }
            }
            if (dbl) {
                while (gpio_get_level(BOOT_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(30));
                s_weight_idx = (s_weight_idx + 1) % WEIGHT_TAB_N;
                s_weight = k_weight_tab[s_weight_idx];
                ESP_LOGW(TAG, ">>> 双击：模拟体重切到 %.2f kg", s_weight / 100.0);
            } else {
                fire_weigh_in();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- spec 属性表（ms116）----------------
 * 秤的插件开屏会读一串属性；未注册的回 result=1（不认识），页面直接空。
 * 这里按 ms116 spec 把能读的先铺上，剩下的靠日志看 App 到底要什么。 */
static miot_prop_t s800_props[] = {
    { 2, 1,    MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_unit,        0, 1,     NULL, "unit"        },
    { 2, 2,    MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &v_hr_detect,   0, 1,     NULL, "hr-detect"   },
    { 2, 3,    MIOT_TYPE_U32,  MIOT_PROP_RW, 4, &v_time_s,      0, 0,     NULL, "current-time"},
    { 3, 1,    MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_cust5,       0, 255,   NULL, "arg5"        },
    { 3, 2,    MIOT_TYPE_U32,  MIOT_PROP_RW, 4, &v_cust4,       0, 0,     NULL, "arg4"        },
    { 3, 3,    MIOT_TYPE_U32,  MIOT_PROP_RW, 4, &v_cust3,       0, 0,     NULL, "arg3"        },
    { 4, 1,    MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_batt,        0, 100,   NULL, "battery"     },
    { 4, 1004, MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_batt_lo,     0, 4,     NULL, "batt-status" },
    { 9, 3,    MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_off_cnt,     0, 255,   NULL, "offline-cnt" },
    { 10, 1,   MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_work_mode,   0, 4,     NULL, "work-mode"   },
    { 10, 2,   MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_work_status, 0, 255,   NULL, "work-status" },
    { 10, 3,   MIOT_TYPE_U16,  MIOT_PROP_R,  4, &v_work_u16,    0, 0xFFFF,NULL, "work-u16"    },
    { 13, 2,   MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_plugin_ver,  0, 255,   NULL, "plugin-ver"  },
    { 13, 6,   MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_off_cnt,     0, 255,   NULL, "offline-cnt" },
    { 13, 7,   MIOT_TYPE_U8,   MIOT_PROP_R,  1, &v_sn_ver,      0, 255,   NULL, "sn-ver"      },
};

/* 未入表的字符串属性（插件按名字读身份信息）*/
static bool s800_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0]);

    if (siid == 1) {                        /* Device Information */
        out->type = MIOT_TYPE_STRING;
        switch (piid) {
        case 1: snprintf(out->val.str, sizeof(out->val.str), "Xiaomi");     return true;
        case 2: snprintf(out->val.str, sizeof(out->val.str), "MJTZC04YM");  return true;
        case 3: snprintf(out->val.str, sizeof(out->val.str), "%s", mac);    return true;
        case 4: snprintf(out->val.str, sizeof(out->val.str), "1.0.0");      return true;
        case 5: snprintf(out->val.str, sizeof(out->val.str), "S800-0001");  return true;
        default: return false;
        }
    }
    if (siid == 13) {                       /* sync-info 的字符串项 */
        out->type = MIOT_TYPE_STRING;
        switch (piid) {
        case 1: snprintf(out->val.str, sizeof(out->val.str), "1");          return true;
        case 3: snprintf(out->val.str, sizeof(out->val.str), "S800-0001");  return true;
        case 4: snprintf(out->val.str, sizeof(out->val.str), "%s", mac);    return true;
        case 5: snprintf(out->val.str, sizeof(out->val.str), "1.0.0");      return true;
        default: return false;
        }
    }
    if (siid == 9) {                        /* offline-data：空串 = 没有离线数据 */
        out->type = MIOT_TYPE_STRING;
        return true;
    }
    if (siid == 14 && piid == 1) {
        out->type = MIOT_TYPE_STRING;
        return true;
    }
    return false;
}

static bool s800_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user;
    if (siid == 10 && piid == 1) {
        v_work_mode = in->val.u8;
        /* ★App 点「测量」发的是 **SET 10.1**（不是 ACTION）—— 日志实证。
         * 收到后把 10.2 置 1（测量中）让页面切到「请上秤」，4s 后再推数据：
         * 插件要先看到测量态才肯接 10.e.3/10.e.4，太早发等于全丢。 */
        if (v_work_mode) {
            s_measure_at = now_ms() + 4000;
            v_work_status = 1;
            miot_spec_value_t pc;
            memset(&pc, 0, sizeof(pc));
            pc.siid = 10; pc.piid = 2; pc.type = MIOT_TYPE_U8; pc.val.u8 = 1;
            miot_device_send_props_changed(&s_dev, &pc, 1);
            ESP_LOGW(TAG, "App 点「测量」= SET 10.1=%u → 10.2=1，4s 后推数据",
                     (unsigned)v_work_mode);
        }
        return true;
    }
    return false;
}

/* 13.1 sync 之类动作：ms116 的 sync out 是空的，回空即可（App 再按需读属性）*/
int miot_device_action_out(uint8_t siid, uint16_t aiid,
                           miot_spec_value_t *outs, int max)
{
    (void)siid; (void)aiid; (void)outs; (void)max;
    return 0;
}

/* 覆盖 SDK 的 weak 钩子：App 点「测量」= ACTION 10.1（set-mode） */
void miot_demo_app_action_hook(uint8_t siid, uint16_t aiid)
{
    if (siid == 10 && aiid == 1) {
        s_measure_at = now_ms() + 4000;
        ESP_LOGW(TAG, "App 点了「测量」（ACTION 10.1）→ 4s 后推称重序列");
    }
}

/* 会话建立后主动吐一帧「我还活着」：电量 + 当前时间 + 工状态 */
static void push_status(void)
{
    miot_spec_value_t p[3];
    memset(p, 0, sizeof(p));
    p[0].siid = 4;  p[0].piid = 1; p[0].type = MIOT_TYPE_U8;  p[0].val.u8  = v_batt;
    p[1].siid = 2;  p[1].piid = 3; p[1].type = MIOT_TYPE_U32; p[1].val.u32 = v_time_s;
    p[2].siid = 10; p[2].piid = 2; p[2].type = MIOT_TYPE_U8;  p[2].val.u8  = v_work_status;
    int n = miot_device_send_props_changed(&s_dev, p, 3);
    ESP_LOGI(TAG, "状态帧已推 4.1 电量/2.3 时间/10.2 工状态 -> %d", n);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

#if USE_CUSTOM_MAC
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
    /* 5120：按键任务里要组 spec 事件（pt 256B + ag2 加密 300B 的栈缓冲） */
    xTaskCreate(btn_task, "btn", 5120, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    v_time_s = 1794700000u;                 /* 占位时钟，App 会用 2.3 校对 */

    miot_port_esp32_crypto_init();
    fcnt_load();

    ESP_LOGI(TAG, "===== 体脂秤 S800 小白版（MJTZC04YM / 0x%04X）=====", S800_PID);
    ESP_LOGI(TAG, "事件 0x4E16 = 3.e.1022：p2 阶段帧 + p3 数据帧（各 4B u32）");
    ESP_LOGI(TAG, "  stage 0x05 → p3 高16位 = 体重×100（kg）");
    ESP_LOGI(TAG, "  stage 0x15 → p3 高 8 位 = 心率-50");
    ESP_LOGI(TAG, "用法：米家 App → + → 添加设备 → 附近设备 → 绑定");
    ESP_LOGI(TAG, "      短按 BOOT = 上秤称重   双击 = 换体重档   长按 5 秒 = 恢复出厂");

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_device_init(&s_dev, &s_port);
    miot_device_attach(&s_dev, s800_props,
                       sizeof(s800_props) / sizeof(s800_props[0]),
                       s800_get, s800_set, NULL);

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
    ESP_LOGI(TAG, "ECDH 预生成 %lums", (unsigned long)(now_ms() - t0));

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙协议栈初始化失败");
        return;
    }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(S800_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    /* ★0x0002 PID + 0x0004 固件版本（"x.y.z_zzzz" 形态）：App 在 LOGIN 后 read
     * 这两条做设备确认，读失败/格式不对就直接放弃初始化 —— 症状是「登录成功，
     * 之后一条 spec 请求都没有，页面永远空着」（S400 2026-11-14 实测，本题同型）。*/
    gatt_miot_set_pid(S800_PID);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        /* App 点「测量」后排期的称重（4s 后） */
        if (s_measure_at && (int32_t)(t - s_measure_at) >= 0) {
            s_measure_at = 0;
            fire_weigh_in();
        }

        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** 会话建立（spec 通道就绪）***");
            /* ★连上就吐一帧状态（沿用 S400 的经验）：App 订阅完 spec 通道后
             * 1s 内收不到任何东西就退订重连，页面永远停在初始化。 */
            push_status();
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** %s ***", s_was_bound ? "绑定成功，广播切到已绑定形态"
                                                     : "绑定状态被清除");
            build_frame(s_idx);
            adv_refresh();
        }

        {
            static bool win_was;
            bool win_now = win_active();
            if (win_was && !win_now) {
                build_frame(s_idx);
                adv_refresh();
            }
            win_was = win_now;
        }

        /* 事件队列：每帧重复 EVENT_REPEAT 次，按「帧→重复→下一帧」推进 */
        /* ★称重事件在「已连接」时也必须继续发：真机 S800 的体重就在广播事件
         * 3.e.1022 里，App 开着设备页时手机是连着 GATT 的。旧代码 `if (!connected)`
         * 把队列整段冻住 —— 按 BOOT 一点反应都没有（2026-11-15「无法称重」元凶）。*/
        {
            bool has_ev = (s_q_left > 0 && s_q_n > 0);
            if (has_ev || !gatt_miot_is_connected()) {
                uint32_t period = has_ev ? EVENT_GAP_MS : ADV_PERIOD_MS;
                if (t - last_adv >= period) {
                    last_adv = t;
                    if (s_q_left > 0) {
                        s_q_left--;
                        s_fcnt++;
                        ESP_LOGI(TAG, "事件帧 0x4E16（本帧剩 %d 次，队列剩 %d 帧）",
                                 s_q_left, s_q_n);
                        if (s_q_left == 0) {
                            s_q_head = (s_q_head + 1) % Q_MAX;
                            s_q_n--;
                            s_q_left = s_q_n > 0 ? s_q[s_q_head].repeat : 0;
                        }
                    } else {
                        s_idx = (s_idx + 1) % 2;
                        s_fcnt++;
                        if ((s_fcnt % 10) == 0) fcnt_save();
                    }
                    if (build_frame(s_idx) >= 0) adv_refresh();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
