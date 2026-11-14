/**
 * @file app_main.c
 * @brief 米家无线开关（双键版，XMWXKG01YL / PID 0x1949）—— 可绑定版最小固件
 *
 * 和同仓库 btn1_adv（单键版）是同一套骨架，差异只有三处，下面都标了 ★：
 *   1) PID 0x1949 / 设备名 XMWXKG01YL（单键版是 0x2387 / XMWXKG01CM）
 *   2) 两路按键输入：GPIO9（板载 BOOT）= 1 号键，GPIO3 = 2 号键
 *      事件对象仍然是 0x4E0C 单击 / 0x4E0D 双击 / 0x4E0E 长按，
 *      靠载荷 1B 区分键位：1 = 左键，2 = 右键（真机语义，ble_monitor 不解析载荷）
 *   3) 事件队列：两键共用一个"待发槽"，后按的覆盖先按的（真机是各自独立队列，
 *      这里只做演示，连着按两键请间隔 2.5s 以上，见 README「已知边界」）
 *
 * 链路（docs/protocol.md）：
 *   广播         MiBeacon v5 无 MAC 形态（FC 0x48/0x59）+ 0x038F V2 信标帧
 *   GATT 0xFE95  0x0010 命令/结果码 · 0x0019 认证 · 0x001A spec · 0x001B 第二写通道
 *   绑定         CMD_REGISTER(0xA2) → ECDH → 云下发 did(92B) → 云证书 → 回 0x11
 *
 * ★按键设备是"事件驱动"：网关按广播边沿判定，事件帧必须**立刻**发出，
 *   并重复约 2.5s（网关轮询窗口内至少收到一帧），否则整帧被当抖动丢掉。
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

static const char *TAG = "BTN2";

/* ★改参数只动这一块：
 *   BTN2_PID   —— 米家无线开关 双键版 PID 0x1949（单键版 0x2387）
 *   BTN2_NAME  —— 扫描响应里的设备名（真机是 XMWXKG01YL） */
#define BTN2_PID       0x1949
#define BTN2_NAME      "XMWXKG01YL"
#define ADV_PERIOD_MS  3000      /* 心跳帧周期：常态只发电量 */
#define REPEAT_MS      2500      /* 事件帧重复广播时长（网关轮询去重用） */
/* 命名空间只用来跟别的工程隔离（multi_dev 用 "miot"/"miotN"，btn1_adv 用 "btn1"），
 * 否则会读到别的工程的 did/bind_key，米家把这台"开关"记到别人名下。
 * 换身份不靠改这里 —— 直接 `idf.py erase-flash` 清 NVS。 */
#define NVS_NS         "btn2"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* MAC 开关：1 = 用派生 MAC（推荐），0 = 用芯片出厂物理 MAC。
 * 米家按 MAC 认设备，物理 MAC 已被 t3pro_adv 绑成 3 Pro → 默认派生一个专属的。 */
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4    0x5B        /* 第 5 字节：本项目专属；scale_adv 用 0x5C */
#define BASE_MAC_B5    0x01        /* 末字节 0x01 → 空中 BT MAC 末字节 0x02 */
#endif

/* 1=平时也保持可连接（默认，方便添加与调试）；0=只在按键后开唤醒窗口 */
#define ADV_CONN_ALWAYS  1
#define WAKE_WINDOW_MS   120000

/* ---------------- 两路按键定义 ----------------
 * 左键 = 板载 BOOT（GPIO9，按下 = 低电平），同时兼「按住 5s 恢复出厂」
 * 右键 = GPIO3 对 GND 短接（外接轻触开关，或拿根杜邦线/镊子碰一下）
 * ★不要改成 GPIO2/8/9 以外的 strapping 脚，上电瞬间会被拉低影响启动。 */
#define KEY1_PIN   9
#define KEY2_PIN   3

static uint32_t s_fcnt;
static uint8_t  v_batt = 95;
static uint32_t s_wake_until;

/* 待广播的按键事件：0=无，否则 0x4E0C/0x4E0D/0x4E0E；s_btn_key = 键位 1/2 */
static uint16_t s_btn_obj;
static uint8_t  s_btn_key = 1;
static uint32_t s_btn_until;

static const char *btn_name(uint16_t obj)
{
    return obj == MB_OBJ_BTN_DOUBLE ? "DOUBLE" : obj == MB_OBJ_BTN_HOLD ? "HOLD" : "CLICK";
}

static const char *key_name(int keyno) { return keyno == 2 ? "右键" : "左键"; }

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static uint8_t  s_flen;
static int      s_was_bound;
static uint32_t s_wake_prev;

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

/* ---------------- 广播 ---------------- */
static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = BTN2_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    /* 未绑定：身份心跳 + solicited（bit9）——App 就等这一位才发起连接 */
    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;       /* v2 | registered=0 | authMode=2 | solicited=1 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    /* 已绑定：有按键事件先发事件帧（网关按边沿判定），否则发电量心跳 */
    uint8_t objs[16];
    size_t n;
    if (s_btn_obj) n = miot_beacon_obj_u8(objs, s_btn_obj, s_btn_key);
    else           n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
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
        ESP_LOGE(TAG, "adv_set_fields rc=%d (svc_data=%dB)", rc, n);
        return;
    }

    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(BTN2_PID & 0xFF);
    mfr[m++] = (uint8_t)(BTN2_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)BTN2_NAME;
    rsp.name_len = sizeof(BTN2_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
#if ADV_CONN_ALWAYS
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
#else
    p.conn_mode = win_active() ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
#endif
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (s_btn_obj) {
        p.itvl_min = 48;      /* 事件期快广播 30~50ms */
        p.itvl_max = 80;
    } else {
        p.itvl_min = s_auth.bound ? 800 : 160;
        p.itvl_max = s_auth.bound ? 800 : 320;
    }

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
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            build_frame();
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

/* ---------------- 按键状态机（两路共用） ----------------
 * 单击：按下 <1s 松开，且 400ms 内无第二次按下 → 0x4E0C
 * 双击：松开后 400ms 内再次按下          → 0x4E0D
 * 长按：按住 ≥1s                         → 0x4E0E
 * 左键按住 ≥5s                            → 擦 NVS 恢复出厂（真机长按复位）
 *
 * ★载荷 = 键位（1=左键 / 2=右键），这是双键版与单键版唯一的协议差异。
 * ★一次只处理一个键：正在等双击的 400ms 里，另一键按下会被忽略 ——
 *   演示够用，README「已知边界」写清了。 */
static void fire_event(int keyno, uint16_t obj)
{
    if (s_btn_obj) {
        ESP_LOGW(TAG, "上一事件 0x%04X 还没发完，本次 K%d %s 覆盖它",
                 s_btn_obj, keyno, btn_name(obj));
    }
    s_btn_obj   = obj;
    s_btn_key   = (uint8_t)keyno;
    s_btn_until = now_ms() + REPEAT_MS;
    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    s_fcnt++;
    fcnt_save();
    if (build_frame() >= 0) adv_refresh();
    ESP_LOGW(TAG, "K%d(%s) -> event 0x%04X %s key=%d (repeat %dms)",
             keyno, key_name(keyno), obj, btn_name(obj), keyno, REPEAT_MS);
}

/** 处理一次完整按压；返回 0 表示毛刺已忽略 */
static int handle_press(int pin, int keyno)
{
    int ms = 0;
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ms += 100;
        if (ms >= 5000 && keyno == 1) {          /* 只有左键长按满 5s 才恢复出厂 */
            ESP_LOGW(TAG, "K1 held 5s -> erase NVS (factory reset)");
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
    if (ms < 60) return 0;                       /* 毛刺 */

    if (ms >= 1000) { fire_event(keyno, MB_OBJ_BTN_HOLD); return 1; }

    /* 短按：等 400ms 看有没有第二次按下（双击判定） */
    bool dbl = false;
    for (int t0 = 0; t0 < 400; t0 += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(pin) == 0) { dbl = true; break; }
    }
    if (dbl) {
        while (gpio_get_level(pin) == 0) vTaskDelay(pdMS_TO_TICKS(30));
        fire_event(keyno, MB_OBJ_BTN_DOUBLE);
    } else {
        fire_event(keyno, MB_OBJ_BTN_CLICK);
    }
    return 1;
}

static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (gpio_get_level(KEY1_PIN) == 0) { handle_press(KEY1_PIN, 1); continue; }
        if (gpio_get_level(KEY2_PIN) == 0) { handle_press(KEY2_PIN, 2); continue; }
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
        .pin_bit_mask = (1ULL << KEY1_PIN) | (1ULL << KEY2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    /* ★button_dev_init 用的是无线开关的真实 spec（3.1020 按键配置 + 3.1003 电量），
     * 单键/双键版共用同一张表 —— 双键的差异只在广播载荷的键位。 */
    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_prop_t *tbl; size_t cnt;
    button_dev_init(&s_dev, &s_port, &tbl, &cnt);
    ESP_LOGI(TAG, "spec table: %u props (3.1020 btn-config / 3.1003 battery)", (unsigned)cnt);

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
    ble_svc_gap_device_name_set(BTN2_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame();
            adv_refresh();
        }

        {
            bool win_now = win_active();
            if (s_wake_prev && !win_now) {
                ESP_LOGI(TAG, "wake window closed -> adv back to %s",
                         s_auth.bound ? "0x59" : "0x2A");
                build_frame();
                adv_refresh();
            }
            s_wake_prev = win_now;
        }

        if (s_btn_obj && (int32_t)(t - s_btn_until) >= 0) {
            ESP_LOGI(TAG, "btn: event done (0x%04X K%d %s) -> back to battery frame",
                     s_btn_obj, s_btn_key, btn_name(s_btn_obj));
            s_btn_obj = 0;
            s_fcnt++;
            if (build_frame() >= 0) adv_refresh();
        }

        if (!gatt_miot_is_connected() && !s_btn_obj &&
            t - last_adv >= ADV_PERIOD_MS) {
            last_adv = t;
            s_fcnt++;
            if ((s_fcnt % 10) == 0) fcnt_save();
            if (build_frame() >= 0) adv_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
