/**
 * @file app_main.c
 * @brief 小米魔方控制器（XMMF01JQD / jiqid.robot.cube / PID 0x04E1）—— 小白版最小固件
 *
 * ─────────────────────────────────────────────────────────────────────
 * 这台和传感器完全不同的地方：**spec 几乎是空的**。
 *   jiqid-cube 只有服务 1（Device Information：厂商/型号/SN/固件），
 *   没有任何属性、没有事件、没有 action —— 动作全靠 **MiBeacon 老对象**上报。
 *
 *   动作对象：0x1001（3 字节）。XiaomiGateway3 对它的解码是
 *     BLEMapConv("action", "sensor", mi=4097, map={"000000": "right",
 *                                                   "010000": "left"})
 *   → byte0 = 方向（00 顺时针 / 01 逆时针），后两字节是步进/角度。
 *   本工程按这个基线发两种动作，先验通道，再按 App 实测扩展。
 *
 * 为什么不做 spec 会话上报：
 *   App 的魔方页面是「动作记录 + 自动化触发器」，读不到属性；
 *   绑定后事件走 **加密广播** 就够了（和 v5kwm 八键开关同一条路）。
 *
 * 操作（板载 BOOT 键）：
 *   ① 短按 —— 翻转（顺时针 / right）：0x1001 = 00 00 00
 *   ② 双击 —— 翻转（逆时针 / left）：0x1001 = 01 00 00
 *   ③ 长按 5 秒 = 恢复出厂
 * 空闲时轮播电量帧（0x100A），App 页面的电量靠它。
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

static const char *TAG = "CUBE";

/* ★★★ 改参数只动这一块 ★★★
 *   CUBE_PID  —— 小米魔方控制器的产品 ID（pdid 1249 = 0x04E1）
 *   CUBE_NAME —— 扫描响应里的设备名（真机是 CUBE） */
#define CUBE_PID        0x04E1
#define CUBE_NAME       "CUBE"

/* ★NVS 命名空间必须与其它工程不同（理由见 door_adv/main/app_main.c 注释）*/
#define NVS_NS          "cube"
#define NVS_KEY_ID      "ident"
#define NVS_KEY_FCNT    "fcnt"

/* ★派生 MAC（默认开）：米家按 MAC 去重，每个工程用一个独立后缀
 *   door=0x5D motion=0x5E leak=0x5F hcho=0x61 nightl2=0x70 bulb=0x71 v5kwm=0x72
 *   pir2s=0x73 occ=0x74 s800=0x75 cube=0x76 */
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4     0x76
#define BASE_MAC_B5     0x01            /* 末字节 0x01 → 空中 BT MAC 末字节 0x02 */
#endif

#define BOOT_PIN        9
#define ADV_PERIOD_MS   3000            /* 空闲轮换间隔（魔方是「按一下才说话」）*/
#define EVENT_GAP_MS    400             /* 事件帧补发间隔 */
#define EVENT_REPEAT    3
#define SOLICIT_MS      (10u * 60u * 1000u)
#define WAKE_WINDOW_MS  120000
#define ADV_CONN_ALWAYS 1

/* ── MiBeacon 老对象（非 spec）──
 * 0x1001 是魔方的「动作」对象：3 字节，byte0 = 方向 00=right / 01=left，
 * 后两字节为步进/角度（GW3 的 map 只收录了 00/01 这两种 0 角度形态）。 */
#define CUBE_OBJ_ACTION     0x1001      /* 3B 动作 */
#define CUBE_OBJ_BATTERY    0x100A      /* 电量标准对象：1B u8 % */

/* 两种基线动作（GW3 map 实锤的两种形态）*/
static const uint8_t k_act_right[3] = { 0x00, 0x00, 0x00 };   /* 顺时针 */
static const uint8_t k_act_left[3]  = { 0x01, 0x00, 0x00 };   /* 逆时针 */

static uint32_t s_fcnt;
static uint8_t  s_action[3] = { 0x00, 0x00, 0x00 };  /* 当前要播的动作 payload */
static int      s_act_left;                          /* 动作帧还剩几帧要补发 */
static uint8_t  v_batt = 92;
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
 * 未绑定：只发身份帧（hello）。已绑定：一次一个对象。
 *
 * ★双形态轮换（2026-11-15 魔方首次尝试「绑定成功但 App 显示离线」后加）：
 *   魔方 XMMF01JQD 是 2018 年的老 PID（0x04E1），云端产品档案大概率仍按
 *   **v2 明文广播**解析；而我们前期设备（hcho/pir2s/occ，2019 后的 PID）
 *   实测走 v5 加密帧（FC 0x48/0x59）。同一份对象明文，两种承载各播一帧：
 *     - 偶数帧：v5 加密无 MAC（0x48/0x59）—— 新档案认得
 *     - 奇数帧：v2 明文带 MAC（0x50/0x29）—— 老档案认得
 *   网关按自己档案选一种解，另一种自然被忽略。空口代价 2 帧/6 秒，可接受。
 *
 * ★明文形态的位定义（与 miot_beacon_frame_hello 同源，已验证能被 App 扫到）：
 *   FC lo 0x50 = bit4 MAC Include | bit6 Object Include（不加密，无 Capability）
 *   FC hi 0x29 = bit0 registered=1 | bit2~3 authMode=2 | bit4~7 version=2
 *   配对窗口置 bit1(solicited) → 0x2B。 */
static int build_plain(const miot_beacon_cfg_t *cfg, const uint8_t *objs, size_t n)
{
    int p = 0;
    s_frame[p++] = 0x50;                       /* MAC | Object，无加密 */
    s_frame[p++] = win_active() ? 0x2B : 0x29; /* v2 | registered | auth=2 */
    s_frame[p++] = (uint8_t)(cfg->pid & 0xFF);
    s_frame[p++] = (uint8_t)(cfg->pid >> 8);
    s_frame[p++] = (uint8_t)s_fcnt;
    memcpy(s_frame + p, cfg->mac_air, 6);      /* MAC 字段：网关归属依据 */
    p += 6;
    memcpy(s_frame + p, objs, n);
    p += (int)n;
    return p;
}

static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = CUBE_PID;
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

    if (s_act_left > 0) {
        /* ★动作帧：0x1001 三字节（同一份 payload 补发 3 次，网关按边沿计数）*/
        n = miot_beacon_obj_bytes(objs, CUBE_OBJ_ACTION, s_action, 3);
    } else {
        /* 空闲：只播电量。★千万别在空闲帧里塞动作 —— App 会把每一帧都记成一次动作 */
        n = miot_beacon_obj_u8(objs, CUBE_OBJ_BATTERY, v_batt);
    }

    /* 形态轮换：动作期按补发剩余次数奇偶，空闲期按 idx 奇偶 */
    bool plain = (s_act_left > 0) ? ((s_act_left & 1) == 0) : ((idx & 1) != 0);
    if (plain) {
        int fl = build_plain(&cfg, objs, n);
        s_flen = (uint8_t)fl;
        return fl;
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
    mfr[m++] = (uint8_t)(CUBE_PID & 0xFF);
    mfr[m++] = (uint8_t)(CUBE_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)CUBE_NAME;
    rsp.name_len = sizeof(CUBE_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "扫描响应设置失败");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    /* ★-28 修复（官方 FAQ：设备确认配对类产品）：手机直连后 1~2s 内广播必须
     *   继续保持「请求配对」形态。连接态 NimBLE 发不了可连接广播（MAX_CONNECTIONS=1），
     *   改发同帧内容的「非连接」beacon —— 真机连接期间就是这么干的。 */
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
    /* -28 修复：连接态也要重启广播（非连接 beacon），不能见连接就跳过 */
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
            /* ★-28 修复（官方 FAQ：设备确认配对类产品）：直连后 1~2s 内广播必须
             *   继续保持「请求配对」形态（miio_ble_user_adv_init(1)），停播即 -28。
             *   这里不停播，重建同内容帧后切非连接 beacon 继续播。 */
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

/* ---------------- 动作触发 ----------------
 * 魔方没有属性可写，一次「翻转」= 0x1001 三字节 payload 连播 3 帧。
 * ★连播是必要的：网关（小爱音箱/多模网关）扫广播有丢包，单帧经常吃不到。 */
static void fire_action(const uint8_t act[3])
{
    memcpy(s_action, act, 3);
    s_act_left = EVENT_REPEAT;
    s_fcnt++;
    fcnt_save();
    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    build_frame(s_idx);
    adv_refresh();
    ESP_LOGW(TAG, ">>> 动作 0x1001 = %02X %02X %02X（%s，播 %d 帧）",
             act[0], act[1], act[2],
             act[0] == 0 ? "right/顺时针" : "left/逆时针", EVENT_REPEAT);
}

/* ---------------- 按键（GPIO9 板载 BOOT）
 * 短按    —— 翻转 right（0x1001 = 00 00 00）
 * 双击    —— 翻转 left （0x1001 = 01 00 00）
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

            /* 短按：等 400ms 看有没有第二次按下（双击判定）*/
            bool dbl = false;
            for (int t0 = 0; t0 < 400; t0 += 20) {
                vTaskDelay(pdMS_TO_TICKS(20));
                if (gpio_get_level(BOOT_PIN) == 0) { dbl = true; break; }
            }
            if (dbl) {
                while (gpio_get_level(BOOT_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(30));
                fire_action(k_act_left);
            } else {
                fire_action(k_act_right);
            }
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

    miot_port_esp32_crypto_init();
    fcnt_load();

    ESP_LOGI(TAG, "===== 小米魔方控制器 小白版（XMMF01JQD / 0x%04X）=====", CUBE_PID);
    ESP_LOGI(TAG, "动作对象 0x1001（3B）= GW3 的 mi=4097：byte0 00=right 01=left");
    ESP_LOGI(TAG, "空闲只播电量 0x100A（空闲帧里绝不能塞动作，否则 App 每帧都记一次）");
    ESP_LOGI(TAG, "用法：米家 App → + → 添加设备 → 附近设备 → 绑定");
    ESP_LOGI(TAG, "      短按 BOOT = 翻转 right   双击 = 翻转 left   长按 5 秒 = 恢复出厂");

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
    ble_svc_gap_device_name_set(CUBE_NAME);
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

        if (!gatt_miot_is_connected()) {
            uint32_t period = s_act_left > 0 ? EVENT_GAP_MS : ADV_PERIOD_MS;
            if (t - last_adv >= period) {
                last_adv = t;
                if (s_act_left > 0) {
                    s_act_left--;
                    s_fcnt++;
                    ESP_LOGI(TAG, "动作帧 0x1001（%02X %02X %02X，剩 %d 次）",
                             s_action[0], s_action[1], s_action[2], s_act_left);
                } else {
                    s_idx = (s_idx + 1) % 2;
                    s_fcnt++;
                    if ((s_fcnt % 10) == 0) fcnt_save();
                }
                if (build_frame(s_idx) >= 0) adv_refresh();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
