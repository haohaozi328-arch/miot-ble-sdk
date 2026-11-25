/**
 * @file app_main.c
 * @brief 乐天派人在传感器POP2（rhj.sensor_occupy.l730d / PID 0x5BBA）
 *
 * ─────────────────────────────────────────────────────────────────────
 * 从 occ_adv（小米人体存在传感器）派生，骨架相同（GATT spec + ECDH + MiBeacon2），
 * 换 PID、持续时长对象号、存在状态枚举值。
 *
 * POP2 spec（miot-spec.org 实证）：
 *   urn:miot-spec-v2:device:occupancy-sensor:0000A0BF:rhj-l730d:1
 *   服务 2 存在检测：2.p.1078 存在状态 u8 枚举(0无人/2微动/3远离/4接近/5静态/6移动/7进入)
 *                     2.p.1005 光照 float lux
 *                     2.p.1079 持续无人 u8 / 2.p.1080 持续有人 u8
 *   对象号 eid=0x4000|(siid<<10)|(piid-1000)：
 *     0x484E 存在状态 / 0x4805 光照 / 0x484F 持续无人 / 0x4850 持续有人
 *     0x100A 电量（标准对象）
 *
 * 操作（板载 BOOT 键）：短按 有人(6)/无人(0) 切换；双击 光照档位；长按 5s 恢复出厂。
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

static const char *TAG = "OCC";

/* ★★★ 改参数只动这一块 ★★★ */
#define OCC_PID         0x5BBA          /* 乐天派人在传感器POP2 (rhj.sensor_occupy.l730d, pdid 23418) */
#define OCC_NAME        "L730D"

/* ★NVS 命名空间必须与其它工程不同（米家按 MAC 去重、NVS 存绑定身份）*/
#define NVS_NS          "occpop2"
#define NVS_KEY_ID      "ident"
#define NVS_KEY_FCNT    "fcnt"

/* ★派生 MAC：独立后缀（occ_adv=74 pir2s=73，本工程用 0x75）*/
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4     0x75
#define BASE_MAC_B5     0x01            /* 末字节 0x01 → 空中 BT MAC 末字节 0x02 */
#endif

#define BOOT_PIN        9
#define ADV_PERIOD_MS   6000            /* 空闲轮播间隔 */
#define EVENT_GAP_MS    400             /* 事件帧补发间隔 */
#define EVENT_REPEAT    3
#define NO_ONE_MS       120000          /* 120 秒没动作 → 自动回「无人」*/
#define WAKE_WINDOW_MS  120000
#define ROT_SLOTS       5

/* ── 对象号（公式 eid=0x4000|(siid<<10)|(piid-1000)，POP2 spec 实证）── */
#define OCC_OBJ_STATE        0x484E     /* 2.p.1078 存在状态 u8 枚举(0无人/2微动/3远离/4接近/5静态/6移动/7进入) */
#define OCC_OBJ_ILLUM        0x4805     /* 2.p.1005 光照 f32 lux   */
#define OCC_OBJ_NO_ONE_DUR   0x484F     /* 2.p.1079 持续无人 u8 分钟 */
#define OCC_OBJ_HAS_ONE_DUR  0x4850     /* 2.p.1080 持续有人 u8 分钟 */
#define OCC_OBJ_BATTERY      0x100A     /* 电量标准对象 u8 %        */

/* POP2 存在状态取值（spec value_list，非 occ_adv 的 0/1/2）*/
#define OCC_NO_ONE      0
#define OCC_HAS_ONE     6               /* 6 = 有人移动（spec: Moving）*/
#define OCC_QUICK_CHECK 5               /* 5 = 有人静态（spec: Static Motion） */

#define NO_ONE_DUR_VAL  2               /* 「2 分钟持续无人」条件值 */
#define HAS_ONE_DUR_VAL 2               /* 「2 分钟持续有人」条件值 */

static const float k_lux_tab[] = { 5.0f, 50.0f, 300.0f };
#define LUX_TAB_N  (sizeof(k_lux_tab) / sizeof(k_lux_tab[0]))

static uint32_t s_fcnt;
static uint8_t  v_state = OCC_NO_ONE;   /* 当前存在状态 */
static float    v_lux = 50.0f;
static uint8_t  v_lux_idx = 1;
static uint8_t  v_batt  = 88;
static uint32_t s_last_one_ms;          /* 上次「有人」时刻 */
static uint16_t s_ev_obj;               /* 事件期要播的对象号（0 = 无）*/
static uint8_t  s_ev_val;               /* 事件帧的 u8 载荷 */
static int      s_ev_left;
static uint32_t s_wake_until;

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
 * ★用「无 MAC 形态」：真机心跳帧带 MAC 会超 26B 上限。 */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = OCC_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                        s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;      /* v2 | registered=0 | authMode=2 | solicited=1 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;

    if (s_ev_left > 0 && s_ev_obj) {
        if (s_ev_obj == OCC_OBJ_ILLUM) {
            n = miot_beacon_obj_f32(objs, s_ev_obj, v_lux);
        } else {
            n = miot_beacon_obj_u8(objs, s_ev_obj, s_ev_val);
        }
    } else {
        switch (idx % ROT_SLOTS) {
        case 0:                                         /* 存在状态 */
            n = miot_beacon_obj_u8(objs, OCC_OBJ_STATE, v_state);
            break;
        case 1:                                         /* 光照度 */
            n = miot_beacon_obj_f32(objs, OCC_OBJ_ILLUM, v_lux);
            break;
        case 2:                                         /* 持续无人（条件）*/
            n = miot_beacon_obj_u8(objs, OCC_OBJ_NO_ONE_DUR, NO_ONE_DUR_VAL);
            break;
        case 3:                                         /* 持续有人（条件）*/
            n = miot_beacon_obj_u8(objs, OCC_OBJ_HAS_ONE_DUR, HAS_ONE_DUR_VAL);
            break;
        default:                                        /* 电量 */
            n = miot_beacon_obj_u8(objs, OCC_OBJ_BATTERY, v_batt);
            break;
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
    mfr[m++] = (uint8_t)(OCC_PID & 0xFF);
    mfr[m++] = (uint8_t)(OCC_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)OCC_NAME;
    rsp.name_len = sizeof(OCC_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "扫描响应设置失败");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    /* 连接态发不了可连接广播（MAX_CONNECTIONS=1）→ 发非连接 beacon，
     * 保证「确认配对」类产品在直连后 1~2s 仍能看到请求配对形态（-28 教训）。*/
    p.conn_mode = gatt_miot_is_connected() ? BLE_GAP_CONN_MODE_NON
                                           : BLE_GAP_CONN_MODE_UND;
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
            /* 连接期间保持同一帧的广播（-28 修复，见 troubleshooting Q12）*/
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
        adv_refresh();   /* 连接态一直开着广播，必须 stop/start 换回可连接形态 */
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
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];   /* LSB 序 */
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

/* ---------------- 事件触发 ---------------- */
static void fire_event(uint16_t obj, uint8_t val)
{
    s_ev_obj  = obj;
    s_ev_val  = val;
    s_ev_left = EVENT_REPEAT;
    s_fcnt++;
    fcnt_save();
    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    build_frame(s_idx);
    adv_refresh();
}

/* ---------------- 按键（GPIO9 板载 BOOT）
 * 短按    —— 存在状态切换（有人 1 / 无人 0），推 0x484E
 * 双击    —— 光照档位切换（0x4805）
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
            if (ms < 60) continue;              /* 毛刺 */
            if (ms >= 1000) continue;           /* 1~5s 长按不定义动作 */

            bool dbl = false;
            for (int t0 = 0; t0 < 400; t0 += 20) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (gpio_get_level(BOOT_PIN) == 0) { dbl = true; break; }
            }
            if (dbl) {
                while (gpio_get_level(BOOT_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(30));
                v_lux_idx = (v_lux_idx + 1) % LUX_TAB_N;
                v_lux = k_lux_tab[v_lux_idx];
                ESP_LOGW(TAG, ">>> 双击：光照切到 %.0f lux（0x4805）", v_lux);
                fire_event(OCC_OBJ_ILLUM, 0);
            } else {
                v_state = (v_state == OCC_HAS_ONE) ? OCC_NO_ONE : OCC_HAS_ONE;
                if (v_state == OCC_HAS_ONE) s_last_one_ms = now_ms();
                ESP_LOGW(TAG, ">>> 短按：存在状态 → %s（0x484E=%u）",
                         v_state ? "有人" : "无人", v_state);
                fire_event(OCC_OBJ_STATE, v_state);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* 串口命令：无需物理按键即可触发上报，方便云端双向验证 */
static void cli_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        for (char *p = line; *p; p++) { if (*p == '\r' || *p == '\n') { *p = 0; break; } }
        if (!strncmp(line, "presence", 8)) {
            const char *a = line + 8; while (*a == ' ') a++;
            if (*a) {
                int n = atoi(a);
                v_state = (n == 0) ? OCC_NO_ONE : OCC_HAS_ONE;
                if (v_state == OCC_HAS_ONE) s_last_one_ms = now_ms();
                ESP_LOGW(TAG, ">>> 串口 presence=%u（0x484E）", v_state);
                fire_event(OCC_OBJ_STATE, v_state);
            }
        } else if (!strncmp(line, "lux", 3)) {
            const char *a = line + 3; while (*a == ' ') a++;
            if (*a) { v_lux = (float)atof(a); ESP_LOGW(TAG, ">>> 串口 lux=%.0f（0x4805）", v_lux);
                      fire_event(OCC_OBJ_ILLUM, 0); }
        } else if (!strncmp(line, "info", 4)) {
            ESP_LOGI(TAG, "PID=0x%04X bound=%d presence=%u lux=%.0f", OCC_PID, s_auth.bound, v_state, v_lux);
        }
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
    xTaskCreate(cli_task, "cli", 3072, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    s_last_one_ms = now_ms();

    ESP_LOGI(TAG, "===== 乐天派人在传感器POP2（rhj-l730d / 0x%04X）=====", OCC_PID);
    ESP_LOGI(TAG, "对象号 0x484E 存在状态(u8 枚举)  0x4805 光照(f32)  0x484F 持续无人(u8 分)");
    ESP_LOGI(TAG, "       0x4850 持续有人(u8 分)   0x100A 电量(u8)");
    ESP_LOGI(TAG, "用法：米家 App → + → 添加设备 → 附近设备 → 绑定");
    ESP_LOGI(TAG, "      短按 BOOT = 有人/无人切换   双击 = 光照换档   长按 5 秒 = 恢复出厂");

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_device_init(&s_dev, &s_port);
    miot_device_attach(&s_dev, NULL, 0, NULL, NULL, NULL);

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
    ble_svc_gap_device_name_set(OCC_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** 会话建立（spec 通道就绪）***");
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

        /* 有人 → 120 秒没新动作自动回「无人」，立刻补发 0x484E=0 */
        if (v_state == OCC_HAS_ONE &&
            (int32_t)(t - s_last_one_ms) >= (int32_t)NO_ONE_MS) {
            v_state = OCC_NO_ONE;
            ESP_LOGI(TAG, "120 秒无动作 → 自动回到「无人」（0x484E=0）");
            fire_event(OCC_OBJ_STATE, OCC_NO_ONE);
        }

        if (!gatt_miot_is_connected()) {
            uint32_t period = s_ev_left > 0 ? EVENT_GAP_MS : ADV_PERIOD_MS;
            if (t - last_adv >= period) {
                last_adv = t;
                if (s_ev_left > 0) {
                    s_ev_left--;
                    s_fcnt++;
                    ESP_LOGI(TAG, "事件帧 0x%04X（剩 %d 次）", s_ev_obj, s_ev_left);
                    if (s_ev_left == 0) s_ev_obj = 0;
                } else {
                    s_idx = (s_idx + 1) % ROT_SLOTS;
                    s_fcnt++;
                    if ((s_fcnt % 10) == 0) fcnt_save();
                }
                if (build_frame(s_idx) >= 0) adv_refresh();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
