/**
 * @file app_main.c
 * @brief 蓝牙温湿度计2（LYWSD03MMC / 0x055B）—— 可绑定版固件（ESP32-C3）
 *
 * 一个完整可用的最小示例：米家 App 能走完「添加设备 → 注册绑定 → 连上读属性」，
 * 顶部实时读数、历史曲线都能出（曲线是云端按小时聚合，见 README 的 FAQ）。
 *
 * 链路（docs/protocol.md）：
 *   广播         MiBeacon v5（0xFE95 service data，绑定前后帧不同）+ 0x038F V2 信标帧
 *   GATT 0xFE95  0x0010 命令/结果码 · 0x0019 认证 · 0x001A spec · 0x001B 第二写通道
 *   绑定         CMD_REGISTER(0xA2) → ECDH → 云下发 did(92B) → 云证书 → 回 0x11
 *   会话         CMD_LOGIN(0x24) → appRand/devRand → devConfirm/appConfirm → 0x21
 *   数据         spec 属性帧推在 0x0010 上（App 只订阅 0x0010/0x0019，推 0x001A 页面收不到）
 *
 * 与门锁那类 securityChip 设备的根本差异：这里的 did 是**云端在绑定时下发**
 * （92B 加密帧里带 cloud_sign），设备不需要自证身份，所以不存在证书链那堵墙。
 * 设备要做的只有一件事：把 did 存住，重连时原样回传，别让 App 每次当新设备。
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

static const char *TAG = "THERMO";

/* 蓝牙温湿度计2（LYWSD03MMC / cgllc-cgdk3），国区 PID 0x055B。
 * 走标准认证（非 PSK），免网关，属性读写可验证。
 *
 * ★改参数只动这一块：
 *   THERMO_PID  —— 想模拟别的 BLE 型号就换它的 PID（米家后台/抓真机广播可查）；
 *                  换型号后通常还要同步改 miot_profiles.c 里对应型号的 spec 属性表
 *   THERMO_NAME —— 扫描响应里的设备名，随便改
 *   读数        —— 见下方 v_temp/v_hum/v_batt 与 sim_tick()，接真实传感器时替换 */
#define THERMO_PID     0x055B
#define THERMO_NAME    "LYWSD03MMC"
#define ADV_PERIOD_MS  1500
#define FRAME_SLOTS    3
#define NVS_NS         "miot"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* ---------------- 模拟读数（接真实传感器时替换） ----------------
 * ★值必须缓慢波动，不要写死：蓝牙网关对"值没变"的属性通常只上报一次，
 * 云端历史曲线要积累多个不同数据点才开始绘制，写死的值会导致
 * 设备页顶部有实时读数、下方历史图表一直"数据为空"。 */
static uint32_t s_fcnt;                  /* 帧计数器（模拟值波动也用它驱动） */
static int16_t  v_temp  = 256;   /* 0.1℃  */
static uint16_t v_hum   = 580;   /* 0.1%  */
static uint8_t  v_batt  = 88;    /* %     */

/* 每次轮播广播时推进模拟值：温度 25.0~26.4℃（约 12s 走 0.1），
 * 湿度 57.5~58.5%（约 24s 走 0.1），循环往复。 */
static void sim_tick(void)
{
    v_temp = 250 + (int16_t)((s_fcnt >> 3) % 15);
    v_hum  = 575 + (uint16_t)((s_fcnt >> 4) % 11);
}

static const char *const k_names[FRAME_SLOTS] = {
    "temperature", "humidity", "battery"
};

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

/** miot_port_t.time_now 的签名带 user 参数 */
static uint32_t port_time_now(void *user)
{
    (void)user;
    return now_ms();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ---------------- 身份持久化 ----------------
 * 绑定时云端下发的 did / cloud_sign / bind_key 全在这个 blob 里。
 * 丢了它，App 每次连接都会把设备当新设备重新绑定一遍。 */
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

/** 编码第 idx 帧。绑定后必须用云端派发的 bind_key，否则 App 解不开。 */
static int build_frame(int idx)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = THERMO_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    /* 未绑定：只发身份心跳（12B，带 Capability、registered=0）。
     * 这一帧是 App「附近设备」列表的唯一依据，别掺任何对象数据进去。 */
    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;
    switch (idx) {
    case 0:  n = miot_beacon_obj_i16(objs, MB_OBJ_TEMP_I16, v_temp); break;
    case 1:  n = miot_beacon_obj_u16(objs, MB_OBJ_HUM_U16,  v_hum);  break;
    default: n = miot_beacon_obj_u8 (objs, MB_OBJ_BATTERY,  v_batt); break;
    }

    int fl = miot_beacon_frame_v5(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl);
        return fl;
    }
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
    /* 必须显式带 16-bit Service UUID 列表：App 刷新数据时是按 0xFE95 过滤扫描的，
     * 只有 service data 而没有 UUID 列表时，Android 的 ScanFilter 匹配不上，
     * 设备就"扫不到"——绑定能成，但页面永远空着。
     * 31B 预算：uuid(4) + svc data(25) = 29，所以这里不再放 flags。 */
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

    /* V2 信标帧：0x20 = 未注册。App 靠这一位判断该不该走绑定流程，
     * 绑定完成后改成 0x00，否则它会一直把设备当待添加。 */
    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(THERMO_PID & 0xFF);
    mfr[m++] = (uint8_t)(THERMO_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)THERMO_NAME;
    rsp.name_len = sizeof(THERMO_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) ESP_LOGW(TAG, "adv_rsp set failed");

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 官方要求：未绑定 100~200ms、绑定后 500ms 保活 */
    p.itvl_min = s_auth.bound ? 800 : 160;
    p.itvl_max = s_auth.bound ? 800 : 320;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

/* 重新广播：先停，避免 set_fields 在广播中改长度被拒 */
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
        s_logged_in = 0;
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            build_frame(s_idx);
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
        /* ★这里曾经写成 addr[5-i]，等于把 ble_hs 已经给出的空中(LSB)序
         * 又反转成 MSB 序。广播帧里的 MAC 字段必须是 LSB 序：
         * 云端/网关拿扫描到的 BLE 地址按 LSB 序拼 nonce
         * (mac||pid||cnt||counter_ext)，MAC 一变 nonce 就变，
         * 我们加密出来的帧它永远解不开 —— 表现为设备在线、广播正常、
         * 页面永远没有读数。用 esp_read_mac 重新取一次，序是确定的。 */
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "host synced, MAC %02X:%02X:%02X:%02X:%02X:%02X  bound=%d",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0], s_auth.bound);

    s_idx = 0;
    build_frame(s_idx);
    adv_start();
    gatt_miot_dump_handles();

    /* 绑定态决定 V2 帧的 registered 位，这里同步一次 */
    s_was_bound = s_auth.bound;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 长按 BOOT(GPIO9) 5 秒 = 恢复出厂：清掉绑定身份，广播回到未注册态。
     * 真机靠按键复位重新配对，这里同理 —— 否则一旦绑定过一次，
     * App 会认为设备已属于某个账号，再也进不了"添加设备"流程。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 9,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(9) == 0) {
        for (int i = 0; i < 50; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(9) != 0) break;      /* 松手即取消 */
            if (i == 49) {
                ESP_LOGW(TAG, "BOOT held 5s -> erase NVS (factory reset)");
                nvs_flash_erase();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    /* 设备层：spec 属性 + 会话 */
    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;        /* 事件时间戳按开机毫秒给，够排序用 */
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_prop_t *tbl; size_t cnt;
    thermometer_init(&s_dev, &s_port, &tbl, &cnt);

    /* 认证层：notify 走 0x0010 / 0x0019，身份落 NVS */
    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.save        = id_save;
    ap.load        = id_load;
    ap.random      = port_random;
    miot_auth_init(&s_auth, &ap);

    /* ECDH 预生成：ESP32-C3 上约 390ms，放在 0xA2 处理里会让 device info
     * 迟到、App 以 -51 断开（S400 实测） */
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
    ble_svc_gap_device_name_set(THERMO_NAME);
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(host_task);

    uint32_t last_report = 0;
    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        /* 登录成功：把会话密钥塞给设备层，spec 通道才能收发 */
        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** session established: spec channel ready ***");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame(s_idx);
            adv_refresh();
        }

        if (!gatt_miot_is_connected()) {
            if (t - last_adv >= ADV_PERIOD_MS) {
                last_adv = t;
                s_idx = (s_idx + 1) % FRAME_SLOTS;
                s_fcnt++;
                sim_tick();
                if ((s_fcnt % 10) == 0) fcnt_save();
                if (build_frame(s_idx) >= 0) {
                    adv_refresh();
                    ESP_LOGD(TAG, "adv#%d %-13s frame=%dB cnt=%lu",
                             s_idx + 1, k_names[s_idx], s_flen, (unsigned long)s_fcnt);
                }
            }
        } else if (s_logged_in && t - last_report >= 5000) {
            last_report = t;
            thermometer_report(&s_dev, (float)v_temp / 10.0f,
                               (float)v_hum / 10.0f, (uint32_t)t);
            ESP_LOGI(TAG, "spec properties pushed (%.1fC / %.1f%% / batt %u%%)",
                     (double)v_temp / 10.0, (double)v_hum / 10.0, v_batt);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
