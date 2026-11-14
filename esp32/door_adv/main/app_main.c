/**
 * @file app_main.c
 * @brief 米家门窗传感器2（MCCGQ02HL / PID 0x098B）—— 小白版最小固件
 *
 * ─────────────────────────────────────────────────────────────────────
 * 这个文件只做三件事，其他都不用管：
 *   ① 被米家 App 发现并绑定（0xFE95 服务 + 标准认证，代码在 gatt_miot.c）
 *   ② 短按 BOOT —— 模拟「门开了 / 门关了」：发一帧 0x1019 事件广播，
 *      网关收到 → 云端 → App 页面状态变化 / 触发自动化
 *   ③ 空闲时每 4 秒轮换播一帧（开合 / 光照 / 电量），让网关随时有最新值
 *
 * 长按 BOOT 5 秒 = 恢复出厂（清掉绑定身份，可以重新添加）。
 *
 * 帧格式与协议细节在 docs/protocol.md；想搞懂原理先读 ../t3pro_adv/README.md。
 * 本工程与 t3pro_adv 骨架完全相同，只有 ★ 标记的四处不同。
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

static const char *TAG = "DOOR";

/* ★★★ 改参数只动这一块 ★★★
 *   想改型号 → 改 DOOR_PID / DOOR_NAME（PID 决定米家认成哪款产品，不能随便填）
 *   想改成真实磁簧开关 → 改 door_poll()，其余一律不用碰 */
#define DOOR_PID        0x098B          /* 门窗传感器2 MCCGQ02HL */
#define DOOR_NAME       "MCCGQ02HL"     /* 扫描响应里的名字（米家主要按 PID 认型号）*/

/* ★NVS 命名空间必须与其它工程不同：绑定的 did / bind_key 存在这里。
 * 若与别的工程共用 "miot"，会读到别人的身份，于是以「已绑定」状态广播本产品的帧，
 * 米家会把数据记到那台设备名下，本设备反而添加不了。 */
#define NVS_NS          "door"
#define NVS_KEY_ID      "ident"
#define NVS_KEY_FCNT    "fcnt"

/* ★派生 MAC（默认开）：米家按 MAC 去重 —— 同一块板子之前若被别的工程
 * （t3pro / scale / btn2 …）绑过，物理 MAC 已经在你的账号里，米家扫描时
 * 会直接把这块板子过滤掉，表现为「怎么都扫不到」。
 * 每个工程用一个独立后缀就互不干扰：btn2=0x5B, scale=0x5C, door=0x5D, motion=0x5E */
#define USE_CUSTOM_MAC  1
#if USE_CUSTOM_MAC
#define BASE_MAC_B4     0x5D
#define BASE_MAC_B5     0x01            /* 末字节 0x01 → 空中 BT MAC 末字节 0x02 */
#endif

#define BOOT_PIN        9               /* 板载 BOOT 键（按下 = 低电平）*/
#define ADV_PERIOD_MS   4000            /* 空闲轮换间隔；门窗是低频设备，4s 够用 */
#define EVENT_GAP_MS    400             /* 事件帧补发间隔（连发 3 次防丢帧）*/
#define EVENT_REPEAT    3
#define WAKE_WINDOW_MS  120000          /* 按一下键后 2 分钟内保持可连接 */
#define ADV_CONN_ALWAYS 1               /* 1=平时也保持可连接（方便添加）*/

/* 广播对象（真机同款）：
 *   0x1019 开合  1B   0=开 1=关 2=超时未关 3=复位
 *   0x1018 光照  1B   0=暗 非0=亮
 *   0x100A 电量  1B   %                                               */
#define OBJ_DOOR_OPEN   0
#define OBJ_DOOR_CLOSE  1

static uint32_t s_fcnt;                 /* 帧计数器（掉电不回跳，防重放）*/
static uint8_t  v_door  = OBJ_DOOR_CLOSE;   /* 当前状态：默认「关着」*/
static uint8_t  v_light = 1;                /* 有光 */
static uint8_t  v_batt  = 95;
static int      s_ev_left;              /* 还剩几帧事件要补发 */
static uint32_t s_wake_until;           /* 按键唤醒窗口截止（ms）*/

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

/* ---------------- 身份持久化 ----------------
 * 绑定时云端下发的 did / bind_key 全在这个 blob 里；丢了就要重新绑定。 */
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
 * 未绑定：只发身份帧（hello），米家「附近设备」靠它列出 MCCGQ02HL。
 * 已绑定：加密帧，一次只装一个对象 —— 门窗三类值轮换播，事件优先插队。
 *
 * ★为什么用「无 MAC 形态」(miot_beacon_frame_v5_nomac)：
 *   BLE 一个广播包只有 31B，扣掉 AD 头/UUID/0xFE95 后帧最多 26B。
 *   带 MAC 的形态光帧头+MIC 就 18B，加一个对象就接近上限，
 *   一旦超了 ble_gap_adv_set_fields 返回 rc=4，广播直接停摆（App 表现为「一直
 *   正在连接」）。真机 24B 的测量帧同样不带 MAC，网关按广播源地址归属。 */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = DOOR_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        /* 未绑定：Capability + 「待绑定」对象都在 hello 里，别自己拼 */
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                        s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello 帧失败 rc=%d", fl); return fl; }
        /* ★bit9（solicited）必须置 1：App 点了「添加」之后会一直等这一位才发起连接，
         * 等不到就干等 30 秒报「连接超时」（串口侧一个 CONNECT 都没有）。 */
        s_frame[1] = 0x2A;
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;

    if (s_ev_left > 0) {
        /* 事件帧插队：开/关门单独发，别混进电量之类的对象 */
        n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR, v_door);
    } else {
        switch (idx) {
        case 0:  n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR,   v_door);  break;
        case 1:  n = miot_beacon_obj_u8(objs, MB_OBJ_LIGHT,  v_light); break;
        default: n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt); break;
        }
    }

    int fl = miot_beacon_frame_v5_nomac(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "加密帧失败 rc=%d（加密后端没初始化？）", fl);
        return fl;
    }
    /* 按键唤醒窗口内补 bit9，等于告诉 App「我在这儿、可以连」；窗口外必须清掉，
     * 长期置位会让网关以为设备一直有待传数据。0x59 → 0x5B。 */
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
    /* 必须显式带 16-bit Service UUID 列表：Android 的 ScanFilter 按 0xFE95 过滤，
     * 只有 service data 没有 UUID 列表时同样扫不到。 */
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

    /* V2 信标帧：0x20 = 未注册。App 靠这一位决定走不走绑定流程。 */
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

/** 重新广播：先停，避免在广播中改字段长度被拒 */
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
            ESP_LOGI(TAG, "手机连上来了（广播已暂停）");
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
        adv_start();
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
        /* ★帧里的 MAC 是 LSB 序：网关拿扫描到的 BLE 地址按 LSB 拼 nonce，
         * 写反了 nonce 就错，加密帧它永远解不开（表现为设备在线、页面永远没值）。 */
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

/* ---------------- 按键（GPIO9 板载 BOOT）
 * 短按 —— 模拟门动作：开 ↔ 关 切换，立刻发事件帧（连发 3 次防丢）
 * 长按 5s —— 恢复出厂：擦 NVS 重启，广播回到未绑定态 */
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
            if (ms >= 60) {
                v_door = (v_door == OBJ_DOOR_CLOSE) ? OBJ_DOOR_OPEN : OBJ_DOOR_CLOSE;
                s_ev_left = EVENT_REPEAT;
                s_fcnt++;
                ESP_LOGW(TAG, ">>> 按键触发：门%s（0x1019 = %u）",
                         v_door == OBJ_DOOR_OPEN ? "被打开" : "被关上", v_door);
                s_wake_until = now_ms() + WAKE_WINDOW_MS;
                build_frame(s_idx);
                adv_refresh();
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
    /* ★必须在任何 BLE / 协议栈初始化之前落定 MAC：米家按 MAC 认设备 */
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

    ESP_LOGI(TAG, "===== 门窗传感器2 小白版（MCCGQ02HL / 0x%04X）=====", DOOR_PID);
    ESP_LOGI(TAG, "用法：米家 App → + → 添加设备 → 点 MCCGQ02HL → 绑定");
    ESP_LOGI(TAG, "      短按 BOOT = 开门/关门一次   长按 5 秒 = 恢复出厂");

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    /* 门窗传感器是纯广播设备：手机页面的开合/光照/电量全部来自
     * 「广播 → 网关 → 云端」，没有 spec 属性表（真机也是这个形态）。 */
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

    /* ECDH 预生成：ESP32-C3 上约 390ms，放进 0xA2 处理里会让 device info
     * 迟到、App 以 -51 断开 */
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
            ESP_LOGI(TAG, "*** 会话建立（spec 通道就绪）***");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** %s ***", s_was_bound ? "绑定成功，广播切到已绑定形态"
                                                     : "绑定状态被清除");
            build_frame(s_idx);
            adv_refresh();
        }

        /* 唤醒窗口到期：把 solicited 位清掉，回到常态形态 */
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
                    s_fcnt++;                       /* 事件帧每次都用新序号 */
                    ESP_LOGI(TAG, "事件帧 %s（剩 %d 次）", 
                             v_door == OBJ_DOOR_OPEN ? "开门" : "关门", s_ev_left);
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
