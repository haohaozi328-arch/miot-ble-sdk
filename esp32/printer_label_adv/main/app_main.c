/**
 * @file app_main.c
 * @brief 米家标签打印机（xiaomi.printer.label / PID 0x34FE）—— 本地控制型
 *
 * 目标：被米家 App 绑定后设备页完整渲染，打印状态/份数/纸张/故障/对时/电量
 *       逐项 SET/GET/回推；source-material(string) 抓 App 下发的打印内容。
 *
 * 产品库实证：pd_id=13502 (0x34FE) model=xiaomi.printer.label
 *   name=米家标签打印机 / resolve_type=3（广播可扫描识别）/ 米家自家
 * spec: urn:miot-spec-v2:device:printer:0000A060:xiaomi-label:1
 *   siid2 printer : 2.1 state u8(0待机/1工作/2完成/3挂起) /
 *                   2.2 source-material string / 2.3 copies u8(1-99) /
 *                   2.4 paper-status bool / 2.5 cover-status bool /
 *                   2.6 fault u8(0正常/1缺纸/2未合盖/3高温) / 2.7 time-set u16
 *                   action 2.1 start / 2.2 continue / 2.3 end
 *   siid3 battery : 3.1 battery-level u8(1-100)
 *
 * ★string 属性无法进数值属性表，走 on_get/on_set 兜底回调。
 *   source-material 是 App→设备下发打印内容的入口（本次重点抓包验证）。
 * GATT/sdkconfig 来自 carlight_adv 完整修复版。
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

static const char *TAG = "PRINTER";

#define PRINTER_PID   0x34FE
#define PRINTER_NAME  "MI-PRINTER"

#define NVS_NS        "printerlabel"
#define NVS_KEY_ID    "ident"
#define NVS_KEY_FCNT  "fcnt"

#define USE_CUSTOM_MAC 1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4   0x03
#define BASE_MAC_B5   0x03
#endif

#define BOOT_PIN       9
#define ADV_PERIOD_MS  4000
#define EVENT_REPEAT   3
#define EVENT_GAP_MS   400
#define WAKE_WINDOW_MS 120000
#define ADV_CONN_ALWAYS 1

/* 本地状态 */
static uint32_t s_fcnt;
static uint8_t  v_state     = 0;       /* 2.1 0待机/1工作/2完成/3挂起 */
static char     v_material[MIOT_SPEC_MAX_STR + 1] = "";  /* 2.2 string（兜底回调） */
static uint8_t  v_copies    = 1;       /* 2.3 1-99 */
static uint8_t  v_paper     = 1;       /* 2.4 有纸=1 */
static uint8_t  v_cover     = 1;       /* 2.5 合盖=1 */
static uint8_t  v_fault     = 0;       /* 2.6 0正常/1缺纸/2未合盖/3高温 */
static uint16_t v_timeset   = 0;       /* 2.7 */
static uint8_t  v_battery   = 85;      /* 3.1 1-100 */

static volatile uint8_t  s_dirty_siid;
static volatile uint16_t s_dirty_piid;

static void printer_on_set(const struct miot_prop *p)
{
    s_dirty_siid = p->siid;
    s_dirty_piid = p->piid;
    ESP_LOGI(TAG, "<<< App set %u.%u 已落值", p->siid, p->piid);
}

static miot_prop_t c_props[] = {
    { 2, 1, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_state,   0, 3,         printer_on_set, "state"        },
    /* 2.2 source-material 是 string，走 on_set/on_get 兜底回调，不进数值表 */
    { 2, 3, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_copies,  1, 99,        printer_on_set, "copies"       },
    { 2, 4, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &v_paper,   0, 1,         printer_on_set, "paper-status" },
    { 2, 5, MIOT_TYPE_BOOL, MIOT_PROP_RW, 1, &v_cover,   0, 1,         printer_on_set, "cover-status" },
    { 2, 6, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_fault,   0, 3,         printer_on_set, "fault"        },
    { 2, 7, MIOT_TYPE_U16,  MIOT_PROP_RW, 2, &v_timeset, 0, 65535,     printer_on_set, "time-set"     },
    { 3, 1, MIOT_TYPE_U8,   MIOT_PROP_RW, 1, &v_battery, 1, 100,       printer_on_set, "battery"      },
};

/* 2.2 string 属性兜底：GET 回当前内容，SET 抓 App 下发的打印数据 */
static bool printer_get(void *user, uint8_t siid, uint16_t piid, miot_spec_value_t *out)
{
    (void)user;
    if (siid == 2 && piid == 2) {
        out->type = MIOT_TYPE_STRING;
        size_t n = strlen(v_material);
        if (n > MIOT_SPEC_MAX_STR) n = MIOT_SPEC_MAX_STR;
        memcpy(out->val.str, v_material, n);
        out->val.str[n] = 0;
        ESP_LOGI(TAG, "GET 2.2 source-material len=%u", (unsigned)n);
        return true;
    }
    return false;
}
static bool printer_set(void *user, uint8_t siid, uint16_t piid, const miot_spec_value_t *in)
{
    (void)user;
    if (siid == 2 && piid == 2) {
        if (in->type != MIOT_TYPE_STRING) return false;
        size_t n = strlen(in->val.str);
        if (n > MIOT_SPEC_MAX_STR) n = MIOT_SPEC_MAX_STR;
        memcpy(v_material, in->val.str, n);
        v_material[n] = 0;
        ESP_LOGW(TAG, "<<< App set 2.2 source-material len=%u data=%s", (unsigned)n, v_material);
        return true;
    }
    return false;
}

/* App 点打印/继续/结束：抓 3 个 action 作为状态机信号 */
void miot_demo_app_action_hook(uint8_t siid, uint16_t aiid)
{
    if (siid != 2) return;
    ESP_LOGW(TAG, ">>> App ACTION 2.%u %s", aiid,
             aiid == 1 ? "start" : (aiid == 2 ? "continue" : (aiid == 3 ? "end" : "?")));
    if (aiid == 1)      v_state = 1;   /* 工作 */
    else if (aiid == 3) v_state = 2;   /* 完成 */
    s_dirty_siid = 2; s_dirty_piid = 1;
}

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

static void port_log(void *user, int level, const char *tag, const char *fmt, ...)
{
    (void)user;
    va_list ap; va_start(ap, fmt);
    char buf[192]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (level == MIOT_LOG_ERROR)      ESP_LOGE(tag, "%s", buf);
    else if (level == MIOT_LOG_WARN)  ESP_LOGW(tag, "%s", buf);
    else                              ESP_LOGI(tag, "%s", buf);
}
static int port_random(void *user, uint8_t *out, size_t len)
{
    (void)user; esp_fill_random(out, len); return 0;
}

/* 广播轮播：state / paper-status / fault / battery */
#define FRAME_SLOTS 4
static int s_idx;
static const char *const k_names[FRAME_SLOTS] = { "state", "paper", "fault", "battery" };

static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = PRINTER_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2, s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        s_frame[1] = 0x2A; s_flen = (uint8_t)fl; return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);
    uint8_t objs[16];
    size_t n;
    switch (s_idx) {
    case 0:  n = miot_beacon_obj_u8 (objs, 0x4001, v_state);   break;  /* 2.1 state */
    case 1:  n = miot_beacon_obj_u8 (objs, 0x4004, v_paper);   break;  /* 2.4 paper */
    case 2:  n = miot_beacon_obj_u8 (objs, 0x4006, v_fault);   break;  /* 2.6 fault */
    default: n = miot_beacon_obj_u8 (objs, 0x4801, v_battery); break;  /* 3.1 battery */
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
    f.uuids16 = &uuid_fe95; f.num_uuids16 = 1; f.uuids16_is_complete = 1;
    f.svc_data_uuid16 = sd; f.svc_data_uuid16_len = (uint8_t)n;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "广播字段失败 rc=%d", rc); return; }

    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03; mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(PRINTER_PID & 0xFF);
    mfr[m++] = (uint8_t)(PRINTER_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)PRINTER_NAME; rsp.name_len = sizeof(PRINTER_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr; rsp.mfg_data_len = (uint8_t)m;
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
            ble_gap_adv_stop(); ESP_LOGI(TAG, "手机连上来了");
        } else { adv_start(); }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        gatt_miot_on_disconnect();
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) { s_was_bound = s_auth.bound; build_frame(); }
        adv_start(); ESP_LOGI(TAG, "手机断开 reason=0x%02x", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_MTU: gatt_miot_on_mtu(event->mtu.value); return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE: adv_start(); return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "App 订阅 attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        gatt_miot_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;
    default: return 0;
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
    build_frame(); adv_start(); gatt_miot_dump_handles();
    s_was_bound = s_auth.bound;
}
static void host_task(void *param)
{
    (void)param; nimble_port_run(); nimble_port_freertos_deinit();
}

static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (gpio_get_level(BOOT_PIN) == 0) {
            int ms = 0;
            while (gpio_get_level(BOOT_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100)); ms += 100;
                if (ms >= 5000) {
                    ESP_LOGW(TAG, "按住 5s → 恢复出厂");
                    nvs_flash_erase(); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
                }
            }
            if (ms >= 60) {
                v_state = v_state ? 0 : 1;
                s_dirty_siid = 2; s_dirty_piid = 1;
                ESP_LOGW(TAG, ">>> 按键：state=%u", v_state);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        for (char *p = line; *p; p++) { if (*p == '\r' || *p == '\n') { *p = 0; break; } }
        if (!strncmp(line, "state", 5)) {
            const char *a = line + 5; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_state = (uint8_t)(n < 0 ? 0 : (n > 3 ? 3 : n));
                      s_dirty_siid = 2; s_dirty_piid = 1;
                      ESP_LOGW(TAG, ">>> 串口 state=%u", v_state); }
        } else if (!strncmp(line, "fault", 5)) {
            const char *a = line + 5; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_fault = (uint8_t)(n < 0 ? 0 : (n > 3 ? 3 : n));
                      s_dirty_siid = 2; s_dirty_piid = 6;
                      ESP_LOGW(TAG, ">>> 串口 fault=%u", v_fault); }
        } else if (!strncmp(line, "batt", 4)) {
            const char *a = line + 4; while (*a == ' ') a++;
            if (*a) { int n = atoi(a); v_battery = (uint8_t)(n < 1 ? 1 : (n > 100 ? 100 : n));
                      s_dirty_siid = 3; s_dirty_piid = 1;
                      ESP_LOGW(TAG, ">>> 串口 battery=%u", v_battery); }
        } else if (!strncmp(line, "info", 4)) {
            ESP_LOGI(TAG, "PID=0x%04X bound=%d state=%u copies=%u paper=%u cover=%u fault=%u batt=%u mat=%s",
                     PRINTER_PID, s_auth.bound, v_state, v_copies, v_paper, v_cover, v_fault,
                     v_battery, v_material);
        } else if (!strncmp(line, "factory", 7)) {
            ESP_LOGW(TAG, "串口 factory → 擦身份重启");
            nvs_flash_erase(); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
        }
    }
}

static void push_prop(uint8_t siid, uint16_t piid)
{
    if (!s_auth.logged_in || !gatt_miot_is_connected()) return;
    static miot_spec_value_t v;
    memset(&v, 0, sizeof(v));
    v.siid = siid; v.piid = piid;
    switch (siid) {
    case 2:
        switch (piid) {
        case 1: v.type = MIOT_TYPE_U8;   v.val.u8  = v_state;   break;
        case 2: v.type = MIOT_TYPE_STRING; strncpy(v.val.str, v_material, MIOT_SPEC_MAX_STR); break;
        case 3: v.type = MIOT_TYPE_U8;   v.val.u8  = v_copies;  break;
        case 4: v.type = MIOT_TYPE_BOOL; v.val.u8  = v_paper;   break;
        case 5: v.type = MIOT_TYPE_BOOL; v.val.u8  = v_cover;   break;
        case 6: v.type = MIOT_TYPE_U8;   v.val.u8  = v_fault;   break;
        case 7: v.type = MIOT_TYPE_U16;  v.val.u16 = v_timeset; break;
        default: return;
        }
        break;
    case 3:
        if (piid == 1) { v.type = MIOT_TYPE_U8; v.val.u8 = v_battery; }
        else return;
        break;
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

    ESP_LOGI(TAG, "===== 米家标签打印机（xiaomi.printer.label / 0x%04X）=====", PRINTER_PID);
    ESP_LOGI(TAG, "串口 state 0-3 / fault 0-3 / batt N / info / factory");

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_device_init(&s_dev, &s_port);
    miot_device_attach(&s_dev, c_props, sizeof(c_props) / sizeof(c_props[0]),
                       printer_get, printer_set, NULL);
    ESP_LOGI(TAG, "spec 属性 %u 条", (unsigned)(sizeof(c_props) / sizeof(c_props[0])));

    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.save = id_save; ap.load = id_load; ap.random = port_random;
    miot_auth_init(&s_auth, &ap);

    uint32_t t0 = now_ms();
    miot_auth_prepare(&s_auth);
    ESP_LOGI(TAG, "ECDH 预生成 %lums", (unsigned long)(now_ms() - t0));

    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "蓝牙初始化失败"); return; }
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = NULL;
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set(PRINTER_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    gatt_miot_set_pid(PRINTER_PID);
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
            /* 全属性初值（2.2 string 单独 push，避免整帧因 STRING 编码超长失败） */
            static miot_spec_value_t v[7];
            memset(v, 0, sizeof(v));
            int i = 0;
            v[i].siid=2; v[i].piid=1; v[i].type=MIOT_TYPE_U8;   v[i++].val.u8=v_state;
            v[i].siid=2; v[i].piid=3; v[i].type=MIOT_TYPE_U8;   v[i++].val.u8=v_copies;
            v[i].siid=2; v[i].piid=4; v[i].type=MIOT_TYPE_BOOL; v[i++].val.u8=v_paper;
            v[i].siid=2; v[i].piid=5; v[i].type=MIOT_TYPE_BOOL; v[i++].val.u8=v_cover;
            v[i].siid=2; v[i].piid=6; v[i].type=MIOT_TYPE_U8;   v[i++].val.u8=v_fault;
            v[i].siid=2; v[i].piid=7; v[i].type=MIOT_TYPE_U16;  v[i++].val.u16=v_timeset;
            v[i].siid=3; v[i].piid=1; v[i].type=MIOT_TYPE_U8;   v[i++].val.u8=v_battery;
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
            s_idx = (s_idx + 1) % FRAME_SLOTS;
            if (build_frame() >= 0) adv_refresh();
            ESP_LOGI(TAG, "broadcast %s", k_names[s_idx]);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
