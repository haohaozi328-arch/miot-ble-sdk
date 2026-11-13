/**
 * @file app_main.c
 * @brief 小米无线开关（蓝牙版，XMWXKG01CM / PID 0x2387）—— 可绑定版最小固件
 *
 * 一个完整可用的最小示例：米家 App 能走完「添加设备 → 注册绑定」，
 * 绑好之后短按板载 BOOT 键 = 单击、快速按两下 = 双击、按住 1 秒左右 = 长按，
 * 三种事件通过加密 MiBeacon 广播上报，米家设备日志能看到，
 * 云端同步后「自动化」编辑器里会出现这台设备的触发条件。
 *
 * 与同目录 t3pro_adv（温湿度计 3 Pro）的关系：
 *   骨架完全相同（认证 + 广播），差异只有三处，都在下面标了 ★：
 *     1) PID / 设备名（0x2387 / XMWXKG01CM）
 *     2) 广播内容：没有测量数据，常态发电量（0x100A），按键时发事件对象
 *        0x4E0C 单击 / 0x4E0D 双击 / 0x4E0E 长按（载荷 1B = 按键号，单键恒 1）
 *     3) BOOT 键不再只是"开唤醒窗口"，它本身就是被模拟的按键：
 *        单击 / 双击 / 长按(1s~5s) 分别发出对应事件；长按满 5s = 恢复出厂
 *
 * 链路（docs/protocol.md）：
 *   广播         MiBeacon v5 无 MAC 形态（FC 0x48/0x59）+ 0x038F V2 信标帧
 *   GATT 0xFE95  0x0010 命令/结果码 · 0x0019 认证 · 0x001A spec · 0x001B 第二写通道
 *   绑定         CMD_REGISTER(0xA2) → ECDH → 云下发 did(92B) → 云证书 → 回 0x11
 *
 * ★按键设备的上报特点是"事件驱动"：网关按广播边沿判定，事件帧必须**立刻**
 *   发出去（等下一轮心跳会落在别的帧上直接漏掉），并且重复广播约 2.5s
 *   （网关轮询周期内至少收到一帧）。这就是本工程主循环里 s_btn_obj /
 *   s_btn_until 那套机制，与 multi_dev 的 BTN 档案同源。
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

static const char *TAG = "BTN1";

/* ★改参数只动这一块：
 *   BTN1_PID   —— 小米无线开关（蓝牙版）国区 pdid 9095 = 0x2387
 *   BTN1_NAME  —— 扫描响应里的设备名（真机是 XMWXKG01CM） */
#define BTN1_PID       0x2387
#define BTN1_NAME      "XMWXKG01CM"
#define ADV_PERIOD_MS  3000      /* 心跳帧周期：常态只发电量，不用太密 */
#define REPEAT_MS      2500      /* 事件帧重复广播时长（网关轮询去重用） */
/* ★命名空间必须独立：multi_dev 的档案槽是 "miot"/"miot<sel>"，t3pro_adv 用
 * "t3pro"。本工程若复用别人名字，会读到别的设备的 did/bind_key，米家会把
 * 这台"开关"记到别人名下。独立命名空间让三个工程可以来回烧互不影响。 */
#define NVS_NS         "btn1"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* 1=平时也保持可连接（默认，方便添加和自动化调试）；0=只在按键后开唤醒窗口 */
#define ADV_CONN_ALWAYS  1
#define WAKE_WINDOW_MS   120000

/* ---------------- 模拟电量（接真实 ADC/电量计时替换） ---------------- */
static uint32_t s_fcnt;                 /* 帧计数器 */
static uint8_t  v_batt = 87;            /* %      */
static uint32_t s_wake_until;           /* 按键唤醒窗口截止（ms） */

/* 待广播的按键事件：0=无，否则 0x4E0C/0x4E0D/0x4E0E（载荷恒 1 = 1 号键） */
static uint16_t s_btn_obj;
static uint32_t s_btn_until;            /* 事件重复广播的截止时刻 */

static const char *btn_name(uint16_t obj)
{
    return obj == MB_OBJ_BTN_DOUBLE ? "DOUBLE" : obj == MB_OBJ_BTN_HOLD ? "HOLD" : "CLICK";
}

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static uint8_t  s_flen;
static int      s_was_bound;
static uint32_t s_wake_prev;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/** 按键唤醒窗口是否还开着 */
static bool win_active(void)
{
    return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0;
}

/** miot_port_t.time_now 的签名带 user 参数 */
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

/* frame_cnt 掉电不回跳（防重放），每 10 帧落一次盘 */
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

/** 编码第 idx 帧。
 * ★与 3 Pro 一样必须用 v5_nomac：service data 只有 23B 余量，
 *   带 MAC 的形态光帧头+MIC 就 18B，加 4B 对象直接超限。 */
static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = BTN1_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    /* 未绑定：只发身份心跳（带 Capability、registered=0）。
     *
     * ★FC 高字节必须带 bit9（solicited，=「设备请求被连接」）：
     *   hello 帧默认给的是 0x28，App 点击添加后会一直等这一位才发起连接，
     *   等不到就干等 30s 报「连接超时」（2026-11-13 在 t3pro_adv 上现场实证，
     *   设备端一个 CONNECT 事件都没有）。未绑定期间恒置这一位是安全的。 */
    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;       /* v2 | registered=0 | authMode=2 | solicited=1 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    /* 已绑定：有按键事件先发事件帧（网关按边沿判定，优先级最高），
     * 没有就发电量心跳 0x100A u8 %（ble_monitor 用它同时推算 2.2~3.1V 电压）。 */
    uint8_t objs[16];
    size_t n;
    if (s_btn_obj) n = miot_beacon_obj_u8(objs, s_btn_obj, 1 /* 按键号：单键恒 1 */);
    else           n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
        return fl;
    }
    /* ★唤醒窗口内置 solicited（0x59 + bit9 = 0x5B）；窗口外清掉 */
    if (win_active()) s_frame[1] = 0x5B;
    s_flen = (uint8_t)fl;
    return fl;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;        /* 已在广播，别重复启动（rc=6） */

    static const ble_uuid16_t uuid_fe95 = BLE_UUID16_INIT(0xFE95);

    uint8_t sd[32];
    int n = 0;
    sd[n++] = 0x95; sd[n++] = 0xFE;          /* MiBeacon service data */
    memcpy(sd + n, s_frame, s_flen);
    n += s_flen;

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    /* 必须显式带 16-bit Service UUID 列表：App/网关按 0xFE95 过滤扫描，
     * 只有 service data 没有 UUID 列表时 Android ScanFilter 匹配不上。 */
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

    /* V2 信标帧：0x20 = 未注册。绑定完成后改成 0x00。 */
    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(BTN1_PID & 0xFF);
    mfr[m++] = (uint8_t)(BTN1_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)BTN1_NAME;
    rsp.name_len = sizeof(BTN1_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
#if ADV_CONN_ALWAYS
    p.conn_mode = BLE_GAP_CONN_MODE_UND;     /* 随时可被 App 连接 */
#else
    p.conn_mode = win_active() ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
#endif
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* ★事件帧期间用快广播（30~50ms）：网关轮询窗口短，必须保证 2.5s
     * 重复期内它至少扫到一帧；平时未绑定 160ms、绑定后 800ms 保活。 */
    if (s_btn_obj) {
        p.itvl_min = 48;
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
        /* ★帧里的 MAC 字段必须是 LSB 序：nonce = mac||pid||cnt||counter_ext，
         * MAC 一错加密帧永远解不开。 */
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

/* ---------------- BOOT 键（GPIO9，板载 BOOT）----------------
 * ★本工程里 BOOT 键就是"被模拟的按键"本身，三种按法：
 *   单击   —— 按下后 500ms 内松开，且 400ms 内没有第二次按下 → 0x4E0C
 *   双击   —— 松开后 400ms 内再次按下 → 两次合并为 0x4E0D
 *   长按   —— 按住 ≥1s 才松开 → 0x4E0E
 *   按住 ≥5s —— 恢复出厂（擦 NVS），对齐真机"长按复位重新配对"
 * 状态机在 100ms 轮询里实现，逻辑见下方 btn_task。 */
static void btn_task(void *arg)
{
    (void)arg;
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
            if (ms < 60) goto wait_next;         /* 毛刺，忽略 */

            /* 判定长按 */
            if (ms >= 1000) {
                s_btn_obj  = MB_OBJ_BTN_HOLD;
            } else {
                /* 短按：等 400ms 看有没有第二次按下（双击判定） */
                bool dbl = false;
                for (int t0 = 0; t0 < 400; t0 += 20) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    if (gpio_get_level(9) == 0) { dbl = true; break; }
                }
                if (dbl) {
                    /* 等第二次按下结束 */
                    while (gpio_get_level(9) == 0) vTaskDelay(pdMS_TO_TICKS(30));
                    s_btn_obj = MB_OBJ_BTN_DOUBLE;
                } else {
                    s_btn_obj = MB_OBJ_BTN_CLICK;
                }
            }
            s_btn_until = now_ms() + REPEAT_MS;
            s_wake_until = now_ms() + WAKE_WINDOW_MS;
            s_fcnt++;
            fcnt_save();
            if (build_frame() >= 0) adv_refresh();
            ESP_LOGW(TAG, "BOOT -> event 0x%04X %s (repeat %dms, wake %ds)",
                     s_btn_obj, btn_name(s_btn_obj), REPEAT_MS, WAKE_WINDOW_MS / 1000);
        }
wait_next:
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

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 9,
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

    /* 设备层：★button_dev_init 用的是无线开关的真实 spec
     * （3.1020 按键配置 u16 RW + 3.1003 电量 u8 R），不要换成别的档案。 */
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
    ble_svc_gap_device_name_set(BTN1_NAME);
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

        /* 唤醒窗口到期：清 solicited 位 */
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

        /* 事件帧到期：回到电量心跳 */
        if (s_btn_obj && (int32_t)(t - s_btn_until) >= 0) {
            ESP_LOGI(TAG, "btn: event done (0x%04X %s) -> back to battery frame",
                     s_btn_obj, btn_name(s_btn_obj));
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
