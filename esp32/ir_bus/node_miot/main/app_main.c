/**
 * @file app_main.c
 * @brief ir_bus 执行节点（伪装 门窗传感器3 xiaomi.magnet.03 / PID 0x8C05）
 *
 * ─────────────────────────────────────────────────────────────────────
 * 本工程 = door3_adv（已验证能上云的骨架）+ 继电器执行层。
 *
 * 目的只有一个：**验证继电器状态能不能经网关上云**。
 * 所以这一版刻意保持最小 —— 只加了 GPIO4 继电器和串口命令，
 * 没有加自定义 BLE 服务（那个等上云验证通过再加，避免一次改两处）。
 *
 * 链路：
 *   PC 串口发 "on"/"off"  →  继电器 GPIO4 动作  →  发 0x480C 事件广播
 *      →  青萍网关解密  →  云端  →  米家 App 显示「门已打开 / 门已关闭」
 *
 * 状态映射（继电器 ↔ 门磁语义）：
 *   继电器 通电(1) = 门「打开」= contact-state 0
 *   继电器 断电(0) = 门「关闭」= contact-state 1
 *
 * ★为什么必须发 0x480C 而不是随便一个对象号：门窗3 的 contact-state 是
 *   2.1012，按新式公式 eid = 0x4000|(siid<<10)|(piid-1000) = 0x480C。
 *   电量 3.1107 → 0x4C6B，且载荷是**枚举** 0=Fully 1=Low（发百分比会被整帧丢弃）。
 *
 * 串口命令（USB-Serial/JTAG @115200）：
 *   on / off / toggle     继电器动作 + 发事件帧
 *   info                  打印 PID / MAC / 绑定 / 继电器 / 门态
 *   obj XXXX / batt XXXX  临时改对象号（排查用）
 *   bval N                改电量枚举值
 *   factory               擦身份重启（可重新添加）
 *
 * 短按 BOOT = 翻转继电器；长按 5 秒 = 恢复出厂。
 * ─────────────────────────────────────────────────────────────────────
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

#include "gatt_miot.h"

void miot_port_esp32_crypto_init(void);

static const char *TAG = "NODEM";

/* ★伪装型号：门窗传感器3。PID 决定米家在云端把它认成哪款产品，不能随便填。 */
#define DOOR_PID        0x8C05
#define DOOR_NAME       "MCCGQ03HL"

/* ★NVS 命名空间必须与其它工程不同，否则会读到别人的绑定身份 */
#define NVS_NS          "nodemiot"
#define NVS_KEY_ID      "ident"
#define NVS_KEY_FCNT    "fcnt"

/* ★派生 MAC：米家按 MAC 去重。这块板子先在 ir_bus(node) 用过 0x4D:0x02，
 * 现在换成 0x70:0x08，与 door3(0x6F:0x03) / node(0x4D:0x02) 都不撞。 */
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4     0x70
#define BASE_MAC_B5     0x08
#endif

#define BOOT_PIN        9
#define RELAY_GPIO      4               /* 继电器 IN（多数模块低电平触发，见 relay_write）*/
#define RELAY_ACTIVE_LOW 0              /* 模块是低电平触发就改成 1 */

#define ADV_PERIOD_MS   4000
#define EVENT_GAP_MS    400
#define EVENT_REPEAT    3
#define WAKE_WINDOW_MS  120000
#define ADV_CONN_ALWAYS 1

#define OBJ_DOOR_OPEN   0
#define OBJ_DOOR_CLOSE  1

static uint16_t s_obj_contact = 0x480C;
static uint16_t s_obj_batt    = 0x4C6B;
#define MB_OBJ_DOOR3_CONTACT  s_obj_contact
#define MB_OBJ_DOOR3_BATT     s_obj_batt

static uint32_t s_fcnt;
static uint8_t  v_door = OBJ_DOOR_CLOSE;   /* 默认「关着」= 继电器断电 */
static uint8_t  v_batt = 0;                /* 枚举 0=Fully 1=Low */
static uint8_t  s_relay;                   /* 逻辑态：1=通电 0=断电 */
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

/** 带毫秒时间戳的单行事件日志，PC 侧好解析（格式：[ 56038ms] XXX ...） */
static void evt(const char *fmt, ...)
{
    char body[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    printf("[%6lums] %s\n", (unsigned long)now_ms(), body);
}

static bool win_active(void)
{
    return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0;
}

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

/* ---------------- 继电器 ---------------- */
static void relay_write(uint8_t on)
{
    s_relay = on ? 1 : 0;
    int level = RELAY_ACTIVE_LOW ? !s_relay : s_relay;
    gpio_set_level(RELAY_GPIO, level);
}

/* ---------------- 广播帧 ---------------- */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = DOOR_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                        s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        s_frame[1] = 0x2A;      /* bit9 solicit：App 等这一位才发起连接 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;

    if (s_ev_left > 0) {
        n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR3_CONTACT, v_door);
    } else {
        switch (idx) {
        case 0:  n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR3_CONTACT, v_door); break;
        default: n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR3_BATT, v_batt);    break;
        }
    }

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "加密帧失败 rc=%d（加密后端没初始化？）", fl);
        return fl;
    }
    {
        uint16_t oid = (uint16_t)(objs[0] | (objs[1] << 8));
        evt("ADV idx=%d ev_left=%d obj=0x%04X val=%u len=%dB",
            idx, s_ev_left, oid, objs[3], fl);
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
    mfr[m++] = (uint8_t)(DOOR_PID & 0xFF);
    mfr[m++] = (uint8_t)(DOOR_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)DOOR_NAME;
    rsp.name_len = sizeof(DOOR_NAME) - 1;
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
            evt("CONN 手机连上（广播暂停）");
        } else {
            evt("CONN 失败 status=%d", event->connect.status);
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
        evt("DISC reason=0x%02x 恢复广播", event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        evt("SUB attr=%u notify=%d", event->subscribe.attr_handle,
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
    evt("READY mac=%02X:%02X:%02X:%02X:%02X:%02X bound=%d relay=%u",
        s_mac_air[5], s_mac_air[4], s_mac_air[3],
        s_mac_air[2], s_mac_air[1], s_mac_air[0], s_auth.bound, s_relay);

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

/* ---------------- 执行层：继电器 ↔ 门态 ---------------- */
/** 置继电器并立刻发事件帧（连发 3 次防丢），语义映射到门磁 contact-state */
static void relay_set(uint8_t on, const char *src)
{
    if (s_relay == (on ? 1 : 0) && v_door == (on ? OBJ_DOOR_OPEN : OBJ_DOOR_CLOSE)) {
        evt("EXEC relay=%u 已是目标态（%s）—— 仍然补发事件帧", s_relay, src);
    }
    relay_write(on);
    v_door = on ? OBJ_DOOR_OPEN : OBJ_DOOR_CLOSE;
    s_ev_left = EVENT_REPEAT;
    s_fcnt++;
    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    evt("EXEC relay=%u door=%s (0x480C=%u) src=%s",
        s_relay, s_relay ? "开门" : "关门", v_door, src);
    build_frame(s_idx);
    adv_refresh();
}

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
                    ESP_LOGW(TAG, "长按 5 秒 → 恢复出厂，重启后可重新添加");
                    nvs_flash_erase();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            if (ms >= 60) relay_set(!s_relay, "按键");
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* 串口 CLI：PC 用它当「指令源」，验证 执行 → 上报 这一段 */
static void cli_task(void *arg)
{
    (void)arg;
    char line[64];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (char *p = line; *p; p++) { if (*p == '\r' || *p == '\n') { *p = 0; break; } }
        if (!strncmp(line, "on", 2)) {
            evt("RX cmd=ON");
            relay_set(1, "串口");
        } else if (!strncmp(line, "off", 3)) {
            evt("RX cmd=OFF");
            relay_set(0, "串口");
        } else if (!strncmp(line, "toggle", 6)) {
            evt("RX cmd=TOGGLE");
            relay_set(!s_relay, "串口");
        } else if (!strncmp(line, "info", 4)) {
            evt("INFO pid=0x%04X mac=%02X:%02X:%02X:%02X:%02X:%02X bound=%d relay=%u door=%u batt=%u",
                DOOR_PID, s_mac_air[5], s_mac_air[4], s_mac_air[3],
                s_mac_air[2], s_mac_air[1], s_mac_air[0],
                s_auth.bound, s_relay, v_door, v_batt);
        } else if (!strncmp(line, "obj", 3)) {
            const char *a = line + 3; while (*a == ' ') a++;
            if (*a) { s_obj_contact = (uint16_t)strtoul(a, NULL, 16);
                      evt("SET contact obj=0x%04X", s_obj_contact); }
            else evt("INFO contact=0x%04X batt=0x%04X", s_obj_contact, s_obj_batt);
        } else if (!strncmp(line, "batt", 4)) {
            const char *a = line + 4; while (*a == ' ') a++;
            if (*a) { s_obj_batt = (uint16_t)strtoul(a, NULL, 16);
                      evt("SET batt obj=0x%04X", s_obj_batt); }
        } else if (!strncmp(line, "bval", 4)) {
            const char *a = line + 4; while (*a == ' ') a++;
            if (*a) { v_batt = (uint8_t)atoi(a); evt("SET batt val=%u", v_batt); }
        } else if (!strncmp(line, "factory", 7)) {
            evt("RX factory → 擦身份重启");
            nvs_flash_erase(); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
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

    /* 继电器脚：先拉低再配成输出，避免上电瞬间误吸合 */
    gpio_config_t ro = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&ro);
    relay_write(0);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);
    xTaskCreate(cli_task, "cli", 4096, NULL, 3, NULL);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    ESP_LOGI(TAG, "===== ir_bus 执行节点（伪装门窗传感器3 / 0x%04X）=====", DOOR_PID);
    ESP_LOGI(TAG, "串口命令：on / off / toggle / info / factory");
    ESP_LOGI(TAG, "短按 BOOT = 翻转继电器   长按 5 秒 = 恢复出厂");

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
    ble_svc_gap_device_name_set(DOOR_NAME);
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
            evt("LOGIN 会话建立");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            evt("BIND %s", s_was_bound ? "绑定成功，广播切加密形态" : "绑定状态被清除");
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

        if (!gatt_miot_is_connected()) {
            uint32_t period = s_ev_left > 0 ? EVENT_GAP_MS : ADV_PERIOD_MS;
            if (t - last_adv >= period) {
                last_adv = t;
                if (s_ev_left > 0) {
                    s_ev_left--;
                    s_fcnt++;
                    evt("EVT 补发 relay=%u 剩 %d 次", s_relay, s_ev_left);
                } else {
                    s_idx = (s_idx + 1) % 3;
                    s_fcnt++;
                    if ((s_fcnt % 10) == 0) fcnt_save();
                }
                if (build_frame(s_idx) >= 0) adv_refresh();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
