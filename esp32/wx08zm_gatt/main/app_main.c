/**
 * @file app_main.c
 * @brief 驱蚊器智能版 WX08ZM（0x040A）—— legacy PSK 可绑定固件（ESP32-C3）
 *
 * 和 [`esp32/thermo_gatt`](../thermo_gatt) 的差别只在「走哪条认证路径」：
 * 温湿度计走标准认证（REGISTER 0xA2 → 云下发 did），
 * 驱蚊器 / 夜灯重绑走 **legacy PSK**（App 先发 26B 启动帧，再 ECDH + 设备签名）。
 * 设备层（GATT 承载、spec 属性、广播）完全一样，所以这个工程是从 thermo_gatt
 * 复制后只改了 PID / 属性表 / 广播对象 三处。
 *
 * 链路（docs/protocol.md、docs/psk-auth.md）：
 *   广播         MiBeacon v5（0xFE95 service data，绑定前后帧不同）+ 0x038F V2 信标帧
 *   GATT 0xFE95  0x0010 命令/结果码 · 0x0019 认证 · 0x001A spec · 0x001B 第二写通道
 *   绑定(PSK)    26B 启动帧 → ECDH → info/0D/03/0C/04 五帧 → 16B server pub → 13B 绑定结果
 *   会话(PSK)    smartcfg-login-* 标签派生 session_key，数据面与标准认证一致
 *   数据         spec 属性帧推在 0x0010 上（App 只订阅 0x0010/0x0019，推 0x001A 页面收不到）
 *
 * 首次上真机时请对照 docs/psk-auth.md §6 那张表读串口，每行都对应握手的一个阶段。
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

static const char *TAG = "REPEL";

/* 驱蚊器智能版 WX08ZM，国区 PID 0x040A。
 * 这个型号走 **legacy PSK**（不是标准认证），绑定阶段就要完成 PSK 握手。
 *
 * ★改参数只动这一块：
 *   DEV_PID      —— 想模拟别的 BLE 型号就换它的 PID（米家后台/抓真机广播可查）；
 *                   换型号后通常还要同步改 miot_profiles.c 里对应型号的 spec 属性表
 *   DEV_NAME     —— 扫描响应里的设备名，随便改
 *   BOND_ABILITY —— 未绑定帧 Capability 里的绑定能力位（0 无 / 1 前绑定 / 2 后绑定 / 3 Combo）。
 *                   默认 2（后绑定：扫描后直连、设备侧本地确认）。
 *                   如果米家「附近设备」里压根不列这台，按 2 → 1 → 3 顺序试一遍。
 *   PSK_*        —— PSK 实测开关，见 docs/psk-auth.md §6
 *   读数         —— 见下方 v_sw/v_margin/v_batt 与 sim_tick()，接真实传感器时替换 */
#define DEV_PID        0x040A
/* 真机 WX08ZM 的广播名是 MJ_MR_V1（ESPHome ESP32 实抓），不是型号名。
 * 名字是扫描过滤的第 8 个候选差异，先照抄真机。 */
#define DEV_NAME       "MJ_MR_V1"
/* ★BondAbility 必须 = 1（前绑定，Capability=0x08）：
 * 2026-11-07 实测，App 对 zimi.mosq.v1 的添加页是「关机状态长按按键 5 秒进配对模式」
 * （BleMatchActivity），这是前绑定流程 —— App 会校验广播帧 Capability 里的
 * BondAbility 与型号云端配置是否一致，广播写 2（后绑定）时它永远不发起连接，
 * 表现为「扫得到设备，但一直转圈直到连接超时」，设备端一条 connect 都收不到。
 * 1 = 前绑定（先进配对模式再连），2 = 后绑定（扫描后直连，温湿度计是这种）。 */
#define BOND_ABILITY   1
#define ADV_PERIOD_MS  1500
#define FRAME_SLOTS    3
/* 扫描探测模式：未绑定时每 5s 轮播 miot_adv_probe_forms() 的 7 种 FC/capability
 * 形态，App 一连上就在串口打 SCANNED-ON: form i（docs/psk-auth.md §6.1）。
 *
 * ★2026-11-07 实测结论：扫不到的真正原因是「同一个 MAC 刚绑过别的型号」，
 * 换 BLE 地址后就正常了，与 FC/capability 形态无关 —— 所以这里关掉轮播，
 * 用标准形态（30 58 / cap 0x10）固定发。轮播还有个副作用：每 5s 要
 * stop/start 一次广播，正好在手机发起连接的瞬间停广播，就是「扫得到、
 * 连接超时」的来源。 */
#define ADV_PROBE      0
/* 换掉蓝牙 MAC：这块板子原来的公开地址（10:00:3B:CE:4D:02）已经在这个账号下
 * 绑成过温湿度计，米家/云端会把该地址当成「已在我的家里」，添加流程不列它。
 *
 * 先试过「随机静态地址」，实测结果是**扫得到、但连不上**：手机侧日志里
 * 系统配网服务把地址解析成 00:00:00:CE:4D:02（随机地址被脱敏），
 * 而米家的 BLE 栈按 PUBLIC 类型发起连接就永远连不上（设备端一条 connect 都没有）。
 * 所以这里改成「换基址 MAC」：BT MAC 由基址派生，仍是 PUBLIC 类型，
 * 和温湿度计成功时完全一致，只是换成一个全新地址。 */
static const uint8_t k_base_mac[6] = { 0xD0, 0x4C, 0x1A, 0x9B, 0x77, 0x30 };
#define NVS_NS         "miot"
#define NVS_KEY_ID     "ident"
#define NVS_KEY_FCNT   "fcnt"

/* PSK 实测开关（docs/psk-auth.md §6）：
 *   PSK_TRACE=1    每帧 hex 打到串口，用来对照「走到哪一步」
 *   PSK_SIGN_VAR   0=签 devRand‖appRand  1=只签 devRand  2=签 shared‖devRand‖appRand
 * 云端若拒签名（App 报错但串口已到 handshake sent），就 0→1→2 逐个试，每次只改这个数字。 */
#define PSK_TRACE      1
#define PSK_SIGN_VAR   MIOT_PSK_SIGN_DEV_APP_RAND

/* ---------------- 模拟读数（接真实开关/耗材时替换） ----------------
 * 驱蚊器只有两个可报量：开关(2.1) 和 电量(3.1)，广播里另有驱蚊片余量(0x1013)。
 * 开关**不自动翻转**：App 上下发的开关状态就是设备的真值（写在 miot_profiles.c
 * 的 rp_on 里），这里只做镜像，否则用户一按开关，几秒后又被模拟值盖回去。
 * 余量缓慢下降是为了让广播帧"有变化"（网关对没变的属性只上报一次）。 */
static uint32_t s_fcnt;                  /* 帧计数器（模拟值变化也用它驱动） */
static uint8_t  v_sw     = 1;    /* 2.1 开关 0/1，跟随 App 下发 */
static uint8_t  v_margin = 76;   /* 0x1013 驱蚊片余量 % */
static uint8_t  v_batt   = 65;   /* 3.1 电量 % */

/* 每 ~10 分钟掉 1% 余量，见底回满，循环往复 */
static void sim_tick(void)
{
    v_margin = (uint8_t)(76 - (uint8_t)((s_fcnt / 400) % 40));
}

static const char *const k_names[FRAME_SLOTS] = {
    "switch", "margin", "battery"
};

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static int      s_idx;
static uint8_t  s_flen;
static int      s_logged_in;
static uint32_t s_login_t;      /* 登录完成时刻（会话收尾定时用） */

#if ADV_PROBE
static const miot_adv_form_t *s_probe_form;
static size_t   s_probe_n, s_probe_i;
static uint32_t s_probe_last;
#endif
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
    cfg.pid = DEV_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    /* 未绑定：只发身份心跳（12B，带 Capability、registered=0）。
     * 这一帧是 App「附近设备」列表的唯一依据，别掺任何对象数据进去。 */
    if (!s_auth.bound) {
#if ADV_PROBE
        /* 探测模式：按候选表第 probe_i 种形态发未绑定帧。
         * cnt 用 probe_i 没关系——这一帧不加密、也没有防重放语义，
         * 只要让 App 能在 5s 窗口里看到当前形态即可。 */
        const miot_adv_form_t *f = s_probe_form;
        int fl = miot_beacon_frame_unbound(DEV_PID, cfg.mac_air, f->fc0, f->fc1,
                                           f->capability, (uint8_t)s_probe_i,
                                           s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "probe rc=%d", fl); return fl; }
        s_flen = (uint8_t)fl;
        return fl;
#else
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, BOND_ABILITY,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        /* ★FC bit9 = solicited（官方名 bindingCfm，「请求 App 进行注册绑定」）。
         * docs/ble-mibeacon.md：前绑定设备必须先发确认包（solicited=1），App 收到
         * 才会发起连接——否则米家停在「长按按键」匹配页永远超时（2026-11-07 实测：
         * App 每秒都在收广播（hook RES 日志 3123 条）却零连接，就是缺这个位）。
         * 真机由按键触发、持续约 10s；这里未绑定期间常置 1，行为等价且省一次按键。 */
        s_frame[1] |= 0x02;   /* FC bit9 → fc1: 0x58 -> 0x5A */
        s_flen = (uint8_t)fl;
        return fl;
#endif
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[16];
    size_t n;
    switch (idx) {
    case 0:  n = miot_beacon_obj_u8(objs, MB_OBJ_SWITCH,  v_sw);     break;
    case 1:  n = miot_beacon_obj_u8(objs, MB_OBJ_MARGIN,  v_margin); break;
    default: n = miot_beacon_obj_u8(objs, MB_OBJ_BATTERY, v_batt);   break;
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
    mfr[m++] = (uint8_t)(DEV_PID & 0xFF);
    mfr[m++] = (uint8_t)(DEV_PID >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)DEV_NAME;
    rsp.name_len = sizeof(DEV_NAME) - 1;
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
#if ADV_PROBE
            if (s_probe_form)
                ESP_LOGI(TAG, "*** SCANNED-ON: form %u (%s) ***  <- 这一种就是 App 认可的形态",
                         (unsigned)(s_probe_i + 1), s_probe_form[s_probe_i].desc);
#endif
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

    /* 地址类型固定 PUBLIC（adv_start 里传 BLE_OWN_ADDR_PUBLIC）。
     * 随机静态地址实测「扫得到但连不上」，不要再切回去。 */
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
    /* ★必须在初始化蓝牙控制器之前改基址：BT MAC 由基址派生，
     * 改完这里读到的就是新地址，且地址类型仍是 PUBLIC。 */
    esp_base_mac_addr_set(k_base_mac);
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

#if ADV_PROBE
    s_probe_form = miot_adv_probe_forms(&s_probe_n);
    ESP_LOGI(TAG, "adv probe mode: %u forms, 5s each (psk-auth.md §6.1)", (unsigned)s_probe_n);
#endif

    /* 设备层：spec 属性 + 会话 */
    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;        /* 事件时间戳按开机毫秒给，够排序用 */
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_prop_t *tbl; size_t cnt;
    repeller_init(&s_dev, &s_port, &tbl, &cnt);

    /* 认证层：notify 走 0x0010 / 0x0019，身份落 NVS
     * 默认 AUTO：标准认证为主，收到 PSK 的 26B 启动帧时自动切到 legacy PSK。
     * 驱蚊器这个型号 App 会直接发 PSK 启动帧，所以 AUTO 就会走 PSK 分支。 */
    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.save        = id_save;
    ap.load        = id_load;
    ap.random      = port_random;
    miot_auth_init(&s_auth, &ap);
    miot_auth_set_psk_options(&s_auth, PSK_TRACE, PSK_SIGN_VAR);

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
    ble_svc_gap_device_name_set(DEV_NAME);
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
            s_login_t = t;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** session established: spec channel ready ***");
            /* ★优化连接模型（zimi.mosq.v1）下，spec 通道的第一件事是版本交换
             * （官方文档：设备必须应答 op=0xF0，否则手机在要任何属性之前就关闭
             * 通道 —— 上一轮 logcat 的 "read version failed" 就是它）。App 在
             * 优化模型里并不主动发 0xF0 请求，这里设备主动发一帧 echo 版本帧。
             * 温湿度计不走优化模型，从未触发这一步。 */
            miot_device_send_version_reply(&s_dev, s_dev.tid++);
            ESP_LOGI(TAG, "spec version exchange frame sent (op=0xF0)");
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d (did stored, adv switches to registered) ***",
                     s_was_bound);
            build_frame(s_idx);
            adv_refresh();
        }

        if (!gatt_miot_is_connected()) {
#if ADV_PROBE
            if (!s_auth.bound && s_probe_form && t - s_probe_last >= 5000) {
                s_probe_last = t;
                s_probe_i = (s_probe_i + 1) % s_probe_n;
                const miot_adv_form_t *f = &s_probe_form[s_probe_i];
                ESP_LOGI(TAG, "[adv] form %u/%u FC=%02X %02X cap=%02X  %s",
                         (unsigned)(s_probe_i + 1), (unsigned)s_probe_n,
                         f->fc0, f->fc1, f->capability, f->desc);
                build_frame(s_idx);
                adv_refresh();
            }
#endif
            /* 未绑定阶段：hello 帧内容是静态的（PID/MAC/capability 都不变），
             * 发出去之后就别再动广播。
             * ★别在这里周期性 build_frame + adv_refresh：adv_refresh 是
             *   stop→start，手机正好在那一刻发起连接就会失败，App 侧表现为
             *   「扫得到设备，但连接超时」，而设备串口一条 connect 都不会打。 */
            if (s_auth.bound && t - last_adv >= ADV_PERIOD_MS) {
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
            v_sw = repeller_power();        /* App 按过开关就以设备侧为准 */
            repeller_report(&s_dev, v_sw, v_batt, v_margin, (uint32_t)t);
            ESP_LOGI(TAG, "spec properties pushed (power=%u / batt %u%% / margin %u%%)",
                     v_sw, v_batt, v_margin);
        }

        /* ★会话收尾兜底：真机不会主动断，是手机决定何时收尾。之前按「登录后
         * 60s」硬断，App 于是每 60s 重连一次，页面永远停在「设备初始化中」
         * （hook_cap.log 11:42/12:01 两轮都是整 60s 被我们踢掉）。
         * 现在只在「手机连续 120s 一个字节都没写」时才断，链路活着就一直陪它。 */
        if (s_logged_in && gatt_miot_is_connected()) {
            uint32_t base = gatt_miot_rx_ms();
            if (base < s_login_t) base = s_login_t;      /* 还没收到过写入 */
            if (t - base >= 120000) {
                ESP_LOGW(TAG, "*** session close: phone idle %lums ***",
                         (unsigned long)(t - base));
                gatt_miot_disconnect();
                s_logged_in = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
