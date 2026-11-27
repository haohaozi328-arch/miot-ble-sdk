/**
 * @file app_main.c
 * @brief HHGPK智能加湿器H10P（zh001.humidifier.h10p / PID 0x805F）—— humidifier 全新品类
 *
 * 从 thermo_m76_adv 派生：GATT/ECDH/MiBeacon2 骨架沿用。
 *
 * 产品库实证：pd_id=32863 (0x805F) model=zh001.humidifier.h10p name=HHGPK智能加湿器H10P
 *   bt_bind_style=2（BLE 直连）、resolve_type=3
 * spec: urn:miot-spec-v2:device:humidifier:0000A00E:zh001-h10p:1
 *   siid2 function：2.1 workmode u8(0关/1-4档/5开) / 2.2 level u8(1-100) /
 *                   2.3 color uint32 RGB / 2.4 light u8 / 2.5 auto-power-off u16
 *   对象号 0x4000|(siid<<10)|piid = 0x4801..0x4805
 *
 * GATT 层与 sdkconfig 来自 kettle_adv  lineage（12 特征值真机对齐、栈 8192）。
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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
#include "miot_ble/miot_props.h"
#include "miot_ble/miot_spec.h"

#include "gatt_miot.h"

void miot_port_esp32_crypto_init(void);

static const char *TAG = "KETTLE";

#define KETTLE_PID     0x805F          /* HHGPK智能加湿器H10P (zh001.humidifier.h10p, pdid 32863) */
#define KETTLE_NAME    "H10P"

#define NVS_NS         "humidh10p"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* 每工程独立 MAC（米家按 MAC 去重），humid 用 0xE8 */
#define USE_CUSTOM_MAC 1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4    0xE8
#define BASE_MAC_B5    0x01
#endif

#define BOOT_PIN       9
#define ADV_PERIOD_MS  4000
#define EVENT_REPEAT   3
#define EVENT_GAP_MS   400
#define WAKE_WINDOW_MS 120000
#define ADV_CONN_ALWAYS 1

/* ---------------- 本地状态 ----------------
 * humidifier spec siid=2 piid1-9，对象号 0x4801..0x4809。
 * 只实现 5 个数值属性（其余 string/int64 由 spec 兜底 GET 处理）。
 */
static uint32_t s_fcnt;
static uint8_t  v_workmode = 0;      /* 2.1 workmode 0关/1-4档/5开 */
static uint8_t  v_level    = 50;     /* 2.2 level 1-100 */
static uint32_t v_color    = 0xFFFFFF;/* 2.3 color RGB u32 */
static uint8_t  v_light    = 1;      /* 2.4 light 0关/1彩/2黄/3自定义 */
static uint16_t v_autoff   = 0;      /* 2.5 auto-power-off 0-720 分 */

static volatile uint8_t  s_dirty_siid;
static volatile uint16_t s_dirty_piid;

static void kettle_on_set(const struct miot_prop *p)
{
    s_dirty_siid = p->siid;
    s_dirty_piid = p->piid;
    ESP_LOGI(TAG, "<<< App set %u.%u 已落值", p->siid, p->piid);
}

/* 属性表：humidifier spec siid=2 的 5 个数值属性 */
static miot_prop_t k_props[] = {
    { 2, 1, MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &v_workmode, 0, 5,       kettle_on_set, "workmode"        },
    { 2, 2, MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &v_level,    1, 100,     kettle_on_set, "level"           },
    { 2, 3, MIOT_TYPE_U32, MIOT_PROP_RW, 4, &v_color,    0, 16777215, kettle_on_set, "color"           },
    { 2, 4, MIOT_TYPE_U8,  MIOT_PROP_RW, 1, &v_light,    0, 3,       kettle_on_set, "light"           },
    { 2, 5, MIOT_TYPE_U16, MIOT_PROP_RW, 2, &v_autoff,   0, 720,     kettle_on_set, "auto-power-off"  },
};

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static uint8_t  s_flen;
static int      s_logged_in;
static int      s_was_bound;
static uint32_t s_wake_until;
static int      s_ev_left;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool win_active(void) { return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0; }
static uint32_t port_time_now(void *user) { (void)user; return now_ms(); }
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
    ESP_LOGI(TAG, "读到已绑定身份（did %02X %02X %02X %02X）",
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

/* ---------------- 广播 ----------------
 * ★本地控制型设备的云端值来自网关收 MiBeacon 广播，必须**多帧轮播所有属性**，
 * 只发一帧 2.1 会导致其它属性云端恒「有定义无值」(-704010000)。
 * 对象号按 MiBeacon2 公式 eid=0x4000|(siid<<10)|(piid-1000)：
 *   2.1→0x4801  3.2→0x4C02  3.5→0x4C05(u16)  3.6→0x4C06 */
#define FRAME_SLOTS 5
static int s_idx;
static const char *const k_names[FRAME_SLOTS] = { "workmode", "level", "color", "light", "autoff" };

static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = KETTLE_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2, s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);
    uint8_t objs[16];
    size_t n;
    switch (s_idx) {
    case 0:  n = miot_beacon_obj_u8 (objs, 0x4801, v_workmode); break;  /* 2.1 workmode */
    case 1:  n = miot_beacon_obj_u8 (objs, 0x4802, v_level);    break;  /* 2.2 level */
    case 2:  n = miot_beacon_obj_u32(objs, 0x4803, v_color);    break;  /* 2.3 color */
    case 3:  n = miot_beacon_obj_u8 (objs, 0x4804, v_light);    break;  /* 2.4 light */
    default: n = miot_beacon_obj_u16(objs, 0x4805, v_autoff);   break;  /* 2.5 auto-power-off */
    }

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) { ESP_LOGE(TAG, "加密帧失败 rc=%d", fl); return fl; }
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
    if (rc != 0) { ESP_LOGE(TAG, "广播字段失败 rc=%d", rc); return; }

    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(KETTLE_PID & 0xFF);
    mfr[m++] = (uint8_t)(KETTLE_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)KETTLE_NAME;
    rsp.name_len = sizeof(KETTLE_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "扫描响应设置失败");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
#if ADV_CONN_ALWAYS
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
#else
    p.conn_mode = win_active() ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
#endif
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (s_ev_left > 0) { p.itvl_min = 48; p.itvl_max = 80; }
    else { p.itvl_min = s_auth.bound ? 800 : 160; p.itvl_max = s_auth.bound ? 800 : 320; }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "广播启动失败 rc=%d", rc);
}
static void adv_refresh(void)
{
    if (gatt_miot_is_connected()) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    adv_start();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            gatt_miot_on_connect(event->connect.conn_handle, BLE_ATT_MTU_DFLT);
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "手机连上来了");
        } else { adv_start(); }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        gatt_miot_on_disconnect();
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) { s_was_bound = s_auth.bound; build_frame(); }
        adv_start();
        ESP_LOGI(TAG, "手机断开 reason=0x%02x，恢复广播", event->disconnect.reason);
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
        gatt_miot_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);
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
    ESP_LOGI(TAG, "协议栈就绪 MAC %02X:%02X:%02X:%02X:%02X:%02X %s",
             s_mac_air[5], s_mac_air[4], s_mac_air[3], s_mac_air[2], s_mac_air[1], s_mac_air[0],
             s_auth.bound ? "已绑定" : "未绑定");
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

/* ---------------- 模拟：按键切换系统状态（加热/待机）---------------- */
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
                    ESP_LOGW(TAG, "按住 5s → 恢复出厂");
                    nvs_flash_erase(); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
                }
            }
            if (ms >= 60) {
                /* 短按：工作档位 0↔5 切换 */
                v_workmode = v_workmode ? 0 : 5;
                s_dirty_siid = 2; s_dirty_piid = 1;
                s_wake_until = now_ms() + WAKE_WINDOW_MS;
                s_ev_left = EVENT_REPEAT; s_fcnt++;
                build_frame(); adv_refresh();
                ESP_LOGW(TAG, ">>> 按键：workmode=%u", v_workmode);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* 串口 CLI：work N / level N / color RRGGBB / light N / autoff N / info / factory */
static void cli_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        for (char *p = line; *p; p++) { if (*p == '\r' || *p == '\n') { *p = 0; break; } }
        if (!strncmp(line, "work", 4)) {
            const char *a = line + 4; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_workmode = (uint8_t)(n < 0 ? 0 : (n > 5 ? 5 : n));
                      s_dirty_siid = 2; s_dirty_piid = 1; s_ev_left = EVENT_REPEAT; s_fcnt++;
                      build_frame(); adv_refresh(); ESP_LOGW(TAG, ">>> 串口 workmode=%u", v_workmode); }
        } else if (!strncmp(line, "level", 5)) {
            const char *a = line + 5; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_level = (uint8_t)(n < 1 ? 1 : (n > 100 ? 100 : n));
                      s_dirty_siid = 2; s_dirty_piid = 2; ESP_LOGW(TAG, ">>> 串口 level=%u", v_level); }
        } else if (!strncmp(line, "color", 5)) {
            const char *a = line + 5; while (*a == ' ') a++;
            if (*a) { v_color = (uint32_t)strtoul(a, NULL, 16);
                      s_dirty_siid = 2; s_dirty_piid = 3; ESP_LOGW(TAG, ">>> 串口 color=0x%06X", v_color); }
        } else if (!strncmp(line, "light", 5)) {
            const char *a = line + 5; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_light = (uint8_t)(n < 0 ? 0 : (n > 3 ? 3 : n));
                      s_dirty_siid = 2; s_dirty_piid = 4; ESP_LOGW(TAG, ">>> 串口 light=%u", v_light); }
        } else if (!strncmp(line, "autoff", 6)) {
            const char *a = line + 6; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_autoff = (uint16_t)(n < 0 ? 0 : (n > 720 ? 720 : n));
                      s_dirty_siid = 2; s_dirty_piid = 5; ESP_LOGW(TAG, ">>> 串口 autoff=%u", v_autoff); }
        } else if (!strncmp(line, "info", 4)) {
            ESP_LOGI(TAG, "PID=0x%04X bound=%d work=%u level=%u color=0x%06X light=%u autoff=%u",
                     KETTLE_PID, s_auth.bound, v_workmode, v_level, v_color, v_light, v_autoff);
        } else if (!strncmp(line, "factory", 7)) {
            ESP_LOGW(TAG, "串口 factory → 擦身份重启");
            nvs_flash_erase(); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
        }
    }
}

/** 回推属性给 App */
static void push_prop(uint8_t siid, uint16_t piid)
{
    if (!s_auth.logged_in || !gatt_miot_is_connected()) return;
    if (siid != 2) return;
    static miot_spec_value_t v;
    memset(&v, 0, sizeof(v));
    v.siid = 2; v.piid = piid;
    switch (piid) {
    case 1: v.type = MIOT_TYPE_U8;    v.val.u8  = v_workmode; break;
    case 2: v.type = MIOT_TYPE_U8;    v.val.u8  = v_level;    break;
    case 3: v.type = MIOT_TYPE_U32;   v.val.u32 = v_color;    break;
    case 4: v.type = MIOT_TYPE_U8;    v.val.u8  = v_light;    break;
    case 5: v.type = MIOT_TYPE_U16;   v.val.u16 = v_autoff;   break;
    default: return;
    }
    miot_device_send_props_changed(&s_dev, &v, 1);
    ESP_LOGI(TAG, "properties_changed %u.%u", siid, piid);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
#if USE_CUSTOM_MAC
    { uint8_t base[6] = { 0x10, 0x00, 0x3B, 0xCE, BASE_MAC_B4, BASE_MAC_B5 }; esp_base_mac_addr_set(base); }
#endif

    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_PIN, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);
    xTaskCreate(cli_task, "cli", 4096, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    ESP_LOGI(TAG, "===== HHGPK智能加湿器H10P（zh001.humidifier.h10p / 0x%04X）=====", KETTLE_PID);
    ESP_LOGI(TAG, "humidifier spec siid2: 0x4801 workmode / 0x4802 level / 0x4803 color / "
                  "0x4804 light / 0x4805 autoff");
    ESP_LOGI(TAG, "串口 work 0-5 / level 1-100 / color RRGGBB / light 0-3 / autoff 0-720 / info");

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_device_init(&s_dev, &s_port);
    miot_device_attach(&s_dev, k_props, sizeof(k_props) / sizeof(k_props[0]), NULL, NULL, NULL);
    ESP_LOGI(TAG, "spec 属性表 %u 条", (unsigned)(sizeof(k_props) / sizeof(k_props[0])));

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

    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "蓝牙初始化失败"); return; }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(KETTLE_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    gatt_miot_set_pid(KETTLE_PID);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0;
    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** 会话建立 ***");
            miot_device_send_version_reply(&s_dev, s_dev.tid++);
            /* 推全部属性初值 */
            static miot_spec_value_t v[5];
            memset(v, 0, sizeof(v));
            int i = 0;
            v[i].siid=2; v[i].piid=1; v[i].type=MIOT_TYPE_U8;  v[i++].val.u8=v_workmode;
            v[i].siid=2; v[i].piid=2; v[i].type=MIOT_TYPE_U8;  v[i++].val.u8=v_level;
            v[i].siid=2; v[i].piid=3; v[i].type=MIOT_TYPE_U32; v[i++].val.u32=v_color;
            v[i].siid=2; v[i].piid=4; v[i].type=MIOT_TYPE_U8;  v[i++].val.u8=v_light;
            v[i].siid=2; v[i].piid=5; v[i].type=MIOT_TYPE_U16; v[i++].val.u16=v_autoff;
            miot_device_send_props_changed(&s_dev, v, (size_t)i);
            ESP_LOGI(TAG, "initial props pushed (%d)", i);
        }

        {
            uint8_t siid = s_dirty_siid;
            uint16_t piid = s_dirty_piid;
            if (piid) { s_dirty_siid = 0; s_dirty_piid = 0; push_prop(siid, piid); }
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** %s ***", s_was_bound ? "绑定成功" : "绑定清除");
            build_frame(); adv_refresh();
        }

        if (s_ev_left > 0 && (int32_t)(t - last_adv) >= EVENT_GAP_MS) {
            last_adv = t; s_ev_left--; s_fcnt++;
            if (build_frame() >= 0) adv_refresh();
        } else if (!gatt_miot_is_connected() && s_ev_left <= 0 && t - last_adv >= ADV_PERIOD_MS) {
            last_adv = t; s_fcnt++;
            if ((s_fcnt % 10) == 0) fcnt_save();
            /* 常态心跳轮播四帧，让网关收齐所有属性（云端才有值） */
            s_idx = (s_idx + 1) % FRAME_SLOTS;
            if (build_frame() >= 0) adv_refresh();
            ESP_LOGI(TAG, "broadcast %s (idx=%d)", k_names[s_idx], s_idx);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
