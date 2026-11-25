/**
 * @file app_main.c
 * @brief 米家 LED 智能灯泡 蓝牙 Mesh 版（MJDP003）—— 可绑定 / 可调光调色（ESP32-C3）
 *
 * 为什么选这台做「新一点的 BLE 灯」：
 *   夜灯 3（MJYD06YL）与夜灯 4（2026-09 众筹）都没有蓝牙，官方明说「不支持
 *   蓝牙连接、不支持米家 APP」，只能纯本地。而 2026-08-31 上架的这支 LED 灯泡
 *   蓝牙 Mesh 版（29.9 元）官方写明「支持手机蓝牙直连，无需蓝牙网关即可在
 *   米家 App 直接控制」—— 也就是说它有一条真正的 GATT 通道，能被我们模拟。
 *
 * 它不是「广播型传感器」，也不是夜灯 2 那种 NUS 文本协议，而是**标准 spec 设备**：
 *   7.1 on                 bool    开关
 *   7.2 brightness         uint8   1 ~ 100 %
 *   7.3 color-temperature  uint16  2700 ~ 5000 K
 *   7.4 mode               uint8   0=Custom 1=Front 2=Drawer 3=Warm 4=NightLight
 *   2.1003 battery-level   uint8   0 ~ 100（只读）
 *   2.1 charging-state     uint8   0=充电 1=未充（只读）
 *   8.1 device-status.mode uint8   0=Off 1=Auto 2=On
 * 本工程实现以上全部，App 页面上会动的就是 7.1 / 7.2 / 7.3。
 *
 * 链路（与 esp32/thermo_gatt 完全一致，已被纯净 App 实测验证）：
 *   广播        未绑定 hello 形态（12B，registered=0）→ 绑定后 v5 加密帧
 *   绑定        0xA2 → ECDH → 云下发 did(92B) → 回 0x11
 *   会话        0x24 → appRand/devRand → 0x21
 *   数据        spec 属性帧推 0x0010（同发 0x001B，覆盖优化连接模型）
 *
 * ★PID 是唯一的未知量：
 *   公开渠道（小米官网、说明书、miot-spec）都只给型号 MJDP003，不给米家的
 *   蓝牙 PID。已按「换代沿用 pdid」推断填 0x06EB（= MJDP09YL 的 pdid 1771），
 *   见 BULB_PID_DEFAULT 的注释；仍不对才需要真机扫一帧
 *   （0xFE95 Service Data 第 3~4 字节，小端）。
 *   这里做成**运行期可改**：串口 `pid 0x1A2B` 写进 NVS 并重启，不用重新编译。
 *
 * 串口命令（USB-Serial/JTAG，115200）：
 *   info            当前状态（PID / 绑定 / 开关 / 亮度 / 色温）
 *   on / off        开关（等价于 App 点开关，会回推 properties_changed）
 *   bright <1~100>  设亮度百分比（直接就是 spec 的 7.2 值）
 *   ct <2700~5000>  设色温
 *   pid [0xXXXX]    查看 / 写入蓝牙 PID（写后自动重启）
 *   factory         擦 NVS 重启（回到未绑定；删设备重加前必须执行）
 *   reboot          重启
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
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

#include "gatt_miot.h"
#include "relay.h"
#include "mesh_miot.h"

void miot_port_esp32_crypto_init(void);

static const char *TAG = "BULB";

/* ======================================================================
 * 改参数只动这一段
 * ==================================================================== */

/** 编译期默认 PID。
 *  ★ 0x06EB = pdid 1771 =「米家 Mesh 灯泡 MJDP09YL」——MJDP003 是它的替代型号
 *    （同为 E27 5W 500lm 2700~6500K，官方 BHR4861CN）。米家同品类换代沿用 pdid，
 *    所以先填这个值试绑定；若米家列表不出现，依次退 0x0926（Yeelight Mesh Bulb M2）
 *    / 0x06EC（米家 Mesh 筒灯），仍不行才需要真机扫一帧。
 *  0 = 未填。注意 NVS 里的 `pid` 键优先级高于本默认值，换默认值后必须 erase-flash。 */
#define BULB_PID_DEFAULT  0x6266   /* 小米汽车磁吸补光灯 careco.nlight.o654（蓝牙，非 Mesh，无需网关） */

/* 1 = 登录完成立刻推全量属性（旧行为，App 1.1s 后必断）
 * 0 = 等 App 发 GET properties（真机时序，当前默认） */
#define PUSH_ON_LOGIN     0
/** 扫描响应里的设备名（米家列表显示的是插件名，这里只是 BLE local name） */
/* ★2026-11-18 广播名必须短。legacy adv 的 scan response 上限 31B，
 * 里面要同时放「名字 + 0x038F V2 信标」，信标固定占 16B（AD 头 2 + 14 数据），
 * 留给名字的只有 13B（AD 头 2 + 名字 ≤ 11）。用型号全名（18B）时
 * ble_gap_adv_rsp_set_fields 每次都返回错误 —— scan response 整个是空的，
 * 米家拿不到 0x20「未注册」标志，表现为搜不到 / 添加时报 -27。
 * 名字只影响显示（产品名由云端按 PID 给），所以这里取短名，优先保住信标。 */
#define BULB_NAME         "careco654"
/** 广播周期（绑定后 500ms 保活，未绑定 100~200ms，由 adv_start 内决定） */
#define ADV_PERIOD_MS     1500

/** 灯输出脚：ESP32-C3 SuperMini 板载蓝灯 */
#define BULB_LED_GPIO        8
/** 1 = 低电平点亮；外接高电平灯带/灯珠改成 0 */
#define BULB_LED_ACTIVE_LOW  1
#define BULB_LED_PWM_HZ      5000
#define BULB_LED_DUTY_BITS   LEDC_TIMER_10_BIT
#define BULB_LED_DUTY_MAX    1023

/** BOOT 键：短按开关灯，长按 3 秒恢复出厂 */
#define BULB_BTN_GPIO        9
#define BULB_LONG_PRESS_MS   3000

#define NVS_NS       "bulb"
#define NVS_KEY_ID   "ident"
#define NVS_KEY_FCNT "fcnt"
#define NVS_KEY_PID  "pid"
#define NVS_KEY_MESHOFF "meshoff"
#define NVS_KEY_MAC  "macb"   /* 6B 基础 MAC 覆盖（末字节 +2 = BLE 地址） */

/* ======================================================================
 * 属性表：careco.nlight.o654（小米汽车磁吸补光灯，PID 0x6266）
 *   urn:miot-spec-v2:device:night-light:0000A0AB:careco-o654:1
 *   7.1 on(bool) / 7.2 brightness(u8 1~100) / 7.3 color-temperature(u16 2700~5000)
 *   7.4 mode(u8 0=Custom 1=Front 2=Drawer 3=Warm 4=NightLight)
 *   2.1003 battery-level(u8 0~100) / 2.1 charging-state(u8 0=充电 1=未充)
 *   8.1 device-status.mode(u8 0=Off 1=Auto 2=On)
 * ★类型必须和 spec 完全一致：亮度报 U16、色温报 U32 都会让插件判为类型错误，
 *   表现是详情页一直转圈（和「缺 0x0004」的症状很像，别搞混）。
 * ==================================================================== */

static uint8_t b_on     = 0;
static int32_t b_bright = 100;   /* 1 ~ 100（百分比，不是 65535） */
static int32_t b_ct     = 5000;  /* 2700 ~ 5000 K */
static int32_t b_lmode  = 0;     /* light.mode */
static int32_t b_dsmode = 2;     /* device-status.mode，0=Off 1=Auto 2=On */
static int32_t b_bat    = 100;   /* battery-level */
static int32_t b_chg    = 1;     /* charging-state，0=Charging 1=Not Charging */
static int32_t b_time   = 0;     /* log.time（uint32 秒），App 进页面会 SET */
static char    s_test[32] = "";  /* device-status.test（string，插件只读不用） */

/** 待回推的属性号（set 成功后由主循环发 properties_changed） */
static volatile uint16_t s_dirty_piid;

static void bulb_on_set(const struct miot_prop *p)
{
    s_dirty_piid = p->piid;        /* 真机行为：set 之后立刻回报这一条 */
}

/* ★★ 与 home.miot-spec.com/spec/careco.nlight.o654 逐条对齐（2026-11-19 实测）：
 *   App 进来先 GET 7.1/7.2/7.3/7.4 + 8.1 + 8.2 + 5.1003，再 SET 9.1（时间戳）。
 *   之前电量挂在 siid=2、缺 8.2 和 9.1，于是 GET 回 ERR、SET 9.1 也回 ERR，
 *   插件判定设备不完整，页面不出数。注意 battery 是 siid=5，不是 2。 */
static miot_prop_t b_props[] = {
    { 7, 1,    MIOT_TYPE_BOOL,   MIOT_PROP_RW, 1, &b_on,     0,    1,    bulb_on_set, "light.on"          },
    { 7, 2,    MIOT_TYPE_U8,     MIOT_PROP_RW, 4, &b_bright, 1,    100,  bulb_on_set, "light.brightness"  },
    { 7, 3,    MIOT_TYPE_U16,    MIOT_PROP_RW, 4, &b_ct,     2700, 5000, bulb_on_set, "light.color_temp"  },
    { 7, 4,    MIOT_TYPE_U8,     MIOT_PROP_RW, 4, &b_lmode,  0,    4,    bulb_on_set, "light.mode"        },
    { 8, 1,    MIOT_TYPE_U8,     MIOT_PROP_RW, 4, &b_dsmode, 0,    2,    bulb_on_set, "devstatus.mode"    },
    { 8, 2,    MIOT_TYPE_STRING, MIOT_PROP_RW, 1, s_test,    0,    0,    NULL,        "devstatus.test"    },
    { 5, 1003, MIOT_TYPE_U8,     MIOT_PROP_R,  4, &b_bat,    0,    100,  NULL,        "battery.level"     },
    { 5, 1,    MIOT_TYPE_U8,     MIOT_PROP_R,  4, &b_chg,    0,    1,    NULL,        "battery.charging"  },
    { 9, 1,    MIOT_TYPE_U32,    MIOT_PROP_RW, 4, &b_time,   0,    0,    NULL,        "log.time"          },
};

/* ======================================================================
 * 状态
 * ==================================================================== */

static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_port_t   s_port;

static uint8_t  s_mac_air[6];
static uint8_t  s_frame[32];
static uint32_t s_pair_win_until;  /* 短按进入的配对窗口（ms），期间快播 */
static int      s_idx;
static uint8_t  s_flen;
/* FC 的 bit7 = mesh 标志（见 include/miot_ble/miot_beacon.h 位定义）。
 * 真机 Mesh 设备在 MiBeacon 里会宣告自己是 Mesh 设备，网关/App 据此走 Mesh
 * 配网而不是 GATT 直连。默认开，串口 `meshbit 0|1` 可关掉做对照。 */
static uint8_t  s_mesh_bit = 0;   /* 默认不宣告 mesh：纯 BLE 形态 App 才肯走手机直连 */
/* ★2026-11-19 真模组对照组：meshpair=1 时按真机 MHCB12G 的「配网举手」帧广播。
 *   真机实测（同一帧重复出现，内容恒定）：
 *     90 54 ...  待机 2s/帧，米家不列出
 *     D0 5C + 14B mesh 状态 ...  配网窗口内 ~0.3s/帧，米家列表里出现
 *   这里原样复刻 D05C 帧，仅 MAC/PID 用我们自己的，用于验证「App 认帧不认证书」到哪一步。 */
static uint8_t  s_mesh_pair = 0;
/* FC 第二字节 bit2~3 = authMode，决定 App 用什么通道绑：
 *   0=RC4/legacy  1=SECURE  2=STANDARD(ECDH,无证书)  3=PSK(无证书,Mesh快速配网)
 * ★2026-11-16 从 App 反编译（BleConnectActivity.access$700）得到真实分发：
 *   OooOooo==1 → securityChipConnect；==2 → bleStandardAuthConnect；
 *   其它(0/3) → secureConnect(Combo)。也就是只有 2 才是 occ_adv 验证过的那条
 *   标准认证链路；填 3 会进 Combo，App 收到设备抢发的 0x0D/0x03 后既不回公钥
 *   也不报错，界面停在「三元组认证」。默认改回 2。 */
static uint8_t  s_auth_mode = 2;
/* 绑定后广播形态（串口 `advmode 0|1` 热切换，不必重烧）：
 *   0 = v5 加密帧（[58 59 ..]，用 ECDH 派生的 bind_key 作 beaconKey）
 *   1 = 明文帧 + registered=1（把 0x1011 从 0「待绑定」改成 1「已绑定」）
 * ★2026-11-16 绑定成功后实测：手机每 5s 连一次、1.6s 就主动断开（0x213），
 *   设备侧 wr 0010=0 0019=0 —— App 连探针都不做。怀疑 v5 帧解不开：
 *   beaconKey 是我们从 ECDH 自己派生的，云端档案里并没有这个值。
 *   先用明文 registered 帧做对照，能一把判定问题是否在加密层。 */
static uint8_t  s_adv_plain = 1;
static int      s_logged_in;
static int      s_was_bound;

static uint16_t s_pid;            /* 运行期 PID（NVS 优先） */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t port_time_now(void *user)
{
    (void)user;
    return now_ms();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ======================================================================
 * LED：亮度百分比 → PWM。单色灯表达不了色温，只记值 + 打日志
 * ==================================================================== */

static void led_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BULB_LED_DUTY_BITS,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = BULB_LED_PWM_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = BULB_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = BULB_LED_ACTIVE_LOW ? BULB_LED_DUTY_MAX : 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
    ESP_LOGI(TAG, "LED: GPIO%d %s, %dHz PWM（2.2 亮度 → 占空比）",
             BULB_LED_GPIO, BULB_LED_ACTIVE_LOW ? "低电平点亮" : "高电平点亮", BULB_LED_PWM_HZ);
}

static void led_sync(void)
{
    int pct = 0;
    if (b_on || b_dsmode != 0) {
        pct = (int)b_bright;               /* 已是 1~100 百分比 */
        if (pct < 1) pct = 1;
        if (pct > 100) pct = 100;
    }
    int duty = (BULB_LED_DUTY_MAX * pct) / 100;
    if (BULB_LED_ACTIVE_LOW) duty = BULB_LED_DUTY_MAX - duty;
    if (duty < 0) duty = 0;
    if (duty > BULB_LED_DUTY_MAX) duty = BULB_LED_DUTY_MAX;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ======================================================================
 * 身份 / 帧计数 / PID 持久化
 * ==================================================================== */

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

static uint32_t s_fcnt;

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

static uint16_t pid_load(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return BULB_PID_DEFAULT;
    if (nvs_get_u16(h, NVS_KEY_PID, &v) != ESP_OK) v = BULB_PID_DEFAULT;
    nvs_close(h);
    return v;
}

/* ★2026-11-16 对照实验：meshoff=1 时完全不注册 0x1827/0x1828，GATT 表回到
 * 「1800+1801+FE95」纯 BLE 形态。用来判定米家 App 的 82ms cancelOpen 是否由
 * Mesh Provisioning 服务存在引起（PID 0x0926/0x4683 都试过，行为一致）。 */
static bool s_mesh_off = true;

static bool mesh_off_load(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return true;
    if (nvs_get_u8(h, NVS_KEY_MESHOFF, &v) != ESP_OK) v = 1;
    nvs_close(h);
    return v ? true : false;
}

static void mesh_off_save(bool off)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_MESHOFF, off ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void pid_save(uint16_t pid)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_KEY_PID, pid);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- MAC 覆盖 ---------------------------------------------------------
 * 米家会把「反复配网失败」的地址写进 DeviceBlackList（App 日志原文：
 *   MiuiConfigNetConnectController: device is in the DeviceBlackList: <mac>
 *   MihomeAutoConfigNetManager_plugin: mac <mac> in black list. do not deal with it）
 * 一旦进名单，App 连上后一个 write 都不发就断开（reason=0x213），
 * 表现和 PID 不匹配完全一样。所以地址必须能在串口里换，否则每失败一次
 * 就得重新编译烧录一次。
 * 存 6 字节「基础 MAC」，BLE 地址由它派生（ESP_MAC_BT = base 末字节 +2）。 */
static bool mac_load(uint8_t base[6])
{
    nvs_handle_t h;
    size_t n = 6;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_blob(h, NVS_KEY_MAC, base, &n) == ESP_OK && n == 6);
    nvs_close(h);
    return ok;
}

static void mac_save(const uint8_t base[6])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_MAC, base, 6);
    nvs_commit(h);
    nvs_close(h);
}

/* mpair（举手帧模式）持久化。之前是纯运行时变量，一重启就归零，
 * 调试时反复出现「明明设了 mpair 1，重连后空口又变回普通帧」。 */
static void mpair_save(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mpair", v);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t mpair_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "mpair", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

/* ======================================================================
 * 日志 / 随机数
 * ==================================================================== */

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

/* ======================================================================
 * 广播
 * ==================================================================== */

/** 绑定后广播里带的那条对象：开关状态。对象号按 MiBeacon 习惯取 0x1012，
 *  真机若用别的号，只影响 App 列表里显示的开关状态，不影响 GATT 控制。 */
#define BULB_OBJ_ON  0x1012

static int build_frame(int idx)
{
    (void)idx;
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = s_pid;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound && s_mesh_pair) {
        /* 真机举手帧逐字节复刻（2026-11-19 22:23 用 tools/adv_dump2.py 并排比对）：
         *   真机 FE95 payload = 00 3c95e681d814 | 008008b272d379 00000000 0200
         *                       └cnt┘ └─MAC 6B──┘ └──── mesh 状态 13B ────┘
         * ★之前这里写成 14B（79 后面多塞了一个 00），整帧比真机长 1 字节，
         *   尾部 02 00 被顶偏，App 解析 mesh 状态错位 → 判「不可配网」。
         * 长度：FC(2)+PID(2)+cnt(1)+MAC(6)+state(13) = 24B
         *       + service-data AD 头（len,type,uuid 共 4B）= 28B，进 31B 预算。 */
        static const uint8_t mstate[13] = {
            0x00, 0x80, 0x08, 0xB2, 0x72, 0xD3, 0x79,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00
        };
        int n2 = 0;
        s_frame[n2++] = 0xD0;                 /* fc_lo: mesh|bit6|bit4 */
        /* ★2026-11-19 22:40 关键：fc_hi 的 bit3:2 是 authMode。
         *   真机 0x5C → authMode=3 → App 走「工厂证书链」认证
         *                （0x40 之后要吐 503B 的 X.509，含 fulian/miio-factory
         *                 CN=Mijia Device 链，验签过才发 Mesh 网络密钥；
         *                 我们伪造不了，只能 -17）
         *   改 0x5A → authMode=2 → App 走标准 BLE 认证（PSK/公钥那套，
         *                本仓库 occ/pir2s/v5kwm/hcho 四台设备实测都能绑成功）。
         *   ★位定义（troubleshooting Q16 实证）：bit0=registered、bit1=solicited、
         *     bit2~3=authMode、bit4~7=version。
         *     0x5B 会顺带把 bit0 置 1 → App 认为「已绑定」→ 待添加列表里根本不显示；
         *     必须用 0x5A（v5|auth2|solicited|registered=0）。 */
        s_frame[n2++] = 0x5A;                 /* fc_hi: v5|auth2|solicited1|reg0 */
        s_frame[n2++] = (uint8_t)(s_pid & 0xFF);
        s_frame[n2++] = (uint8_t)(s_pid >> 8);
        s_frame[n2++] = 0x00;                 /* frame counter */
        memcpy(s_frame + n2, s_mac_air, 6); n2 += 6;
        memcpy(s_frame + n2, mstate, sizeof(mstate)); n2 += sizeof(mstate);
        s_flen = (uint8_t)n2;
        return n2;
    }

    if (!s_auth.bound) {
        /* 未绑定：只发 12B 身份心跳，App「附近设备」的唯一依据 */
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2, s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello rc=%d", fl); return fl; }
        if (s_mesh_bit) s_frame[0] |= 0x80;     /* FC bit7 = mesh */
        /* ★2026-11-16 关键修正（对齐 occ_adv 实测可绑形态）：
         * FC 第二字节 0x28 = v2|authMode2，缺 bit1「请求被连接」。
         * App 扫描到后弹出「请短按一次重置键」，然后**等设备把 bit1 置位**
         * 才去连 GATT。我们之前短按只改亮度/广播间隔，帧没变 →
         * App 认为设备没反应，串口里永远没有 connected。
         * occ_adv 的未绑定帧恒为 0x2A，所以它能绑。这里照抄。 */
        s_frame[1] = (uint8_t)((s_frame[1] & ~0x0C) | ((s_auth_mode & 0x03) << 2));
        s_frame[1] |= 0x02;                     /* bit1 = bindingCfm（绑定确认） */
        s_flen = (uint8_t)fl;
        return fl;
    }

    if (s_auth.bound && s_adv_plain == 2) {
        /* 已绑定态的「干净」明文帧：只有 12B 身份头，不带 0x1011。
         * 0x1011 的语义是「我在待绑定窗口里」，已绑定设备再带它等于自相矛盾，
         * App 可能因此仍按未绑定处理。 */
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2, s_frame, sizeof(s_frame));
        if (fl < 0) return fl;
        s_frame[1] = (uint8_t)(0x01 | ((s_auth_mode & 0x03) << 2) | 0x20);
        if (s_mesh_bit) s_frame[0] |= 0x80;
        s_flen = 12;
        return 12;
    }

    if (s_auth.bound && s_adv_plain == 1) {
        /* 已绑定但发明文帧：FC 第二字节置 registered(bit0)，0x1011 对象改 1。
         * 目的只有一个 —— 让 App 不依赖 beaconKey 也能认出「这台已经绑过」，
         * 从而停止纯探测、开始正式 LOGIN 建链。 */
        int fl = miot_beacon_frame_hello(&cfg, s_fcnt, 2, s_frame, sizeof(s_frame));
        if (fl < 0) { ESP_LOGE(TAG, "hello(bound) rc=%d", fl); return fl; }
        s_frame[1] = (uint8_t)(0x01 | ((s_auth_mode & 0x03) << 2) | 0x20);  /* registered|authMode|v2 */
        if (s_mesh_bit) s_frame[0] |= 0x80;
        if (fl >= 16) s_frame[15] = 0x01;      /* 0x1011: 0=待绑定 → 1=已绑定 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);
    uint8_t objs[8];
    size_t n = miot_beacon_obj_u8(objs, BULB_OBJ_ON, b_on ? 1 : 0);

    int fl = miot_beacon_frame_v5(&cfg, s_fcnt, objs, n, s_frame, sizeof(s_frame));
    if (fl < 0) { ESP_LOGE(TAG, "frame rc=%d (crypto backend?)", fl); return fl; }
    if (s_mesh_bit) s_frame[0] |= 0x80;         /* FC bit7 = mesh */
    s_flen = (uint8_t)fl;
    return fl;
}

/* 把 s_frame 拼成原始 AD 并写进控制器。广播进行中调用也能立即生效。 */
static int adv_set_payload(void)
{
    uint8_t raw[31];
    int r = 0;
    if (7 + (int)s_flen <= 31) {          /* flags 3B + AD头 2B + uuid 2B + 帧 */
        raw[r++] = 0x02; raw[r++] = 0x01; raw[r++] = 0x06;
    }
    raw[r++] = (uint8_t)(1 + 2 + s_flen); /* AD 长度：type+uuid+帧 */
    raw[r++] = 0x16;                      /* 16-bit Service Data */
    raw[r++] = 0x95; raw[r++] = 0xFE;
    memcpy(raw + r, s_frame, s_flen);
    r += s_flen;

    int rc = ble_gap_adv_set_data(raw, r);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_data rc=%d (frame=%uB total=%dB)",
                 rc, (unsigned)s_flen, r);
    }
    return rc;
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;
    /* 配网链路建立后广播实例归 Mesh 栈（PB-GATT 握手中要独占 legacy adv），
     * 这时再 ble_gap_adv_start 只会拿到 busy。链路一关自动恢复 MiBeacon。 */
    if (mesh_miot_adv_owns_radio()) return;

    /* ★2026-11-19 改用原始 AD 拼装（ble_gap_adv_set_data），不再走
     * ble_gap_adv_set_fields：
     *   后者会**强制**塞 flags(3B)，再叠 uuid16 列表(4B)，对 25B 的 Mesh
     *   举手帧就是 34B，超 31B 上限 → 返回错误 → 旧帧继续广播，设备看起来
     *   「好好的在广播但内容不对」，极难查。
     *   真机 MiBeacon 广播只有一条服务数据（2B 头 + 2B UUID + 帧），
     *   25B 帧正好 29B，加 flags 是 32B —— 所以真机也不带 flags。
     *   这里手工拼：flags(3) 能放就放，放不下就只留服务数据。 */
    if (adv_set_payload() != 0) return;

    /* 0x038F V2 信标：0x20 = 未注册（App 据此判断要不要走绑定） */
    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(s_pid & 0xFF);
    mfr[m++] = (uint8_t)(s_pid >> 8);
    mfr[m++] = (uint8_t)s_fcnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    /* ★scan response 同样是 31B 预算，超了 ble_gap_adv_rsp_set_fields 直接返回
     * 错误、整包被丢弃 —— App 就拿不到 0x038F 里的「未注册(0x20)」标志，
     * 表现为「搜不到」或点添加后直接 -27。分级降级：先保信标、砍名字。 */
    static int s_rsp_warned;
    struct ble_hs_adv_fields rsp;
    int rr;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)BULB_NAME;
    rsp.name_len = sizeof(BULB_NAME) - 1;
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    rr = ble_gap_adv_rsp_set_fields(&rsp);
    if (rr != 0) {
        rsp.name = NULL;
        rsp.name_len = 0;
        rsp.name_is_complete = 0;
        rr = ble_gap_adv_rsp_set_fields(&rsp);
        if (rr != 0 && s_rsp_warned < 3) {
            s_rsp_warned++;
            ESP_LOGW(TAG, "adv_rsp set failed rc=%d (name %uB + mfg %uB)",
                     rr, (unsigned)(sizeof(BULB_NAME) - 1), (unsigned)m);
        }
    }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* ★配对窗口：短按后 60s 用 30ms 快播。米家「请短按设备」那一步就是在等
     * 设备进入这个状态——快播让它下一次扫描立刻拿到新包，慢播容易被当成
     * 「按了没反应」。 */
    bool win = s_pair_win_until && now_ms() < s_pair_win_until;
    const bool mp = (s_mesh_pair && !s_auth.bound);
    p.itvl_min = (win || mp) ? 48 : (s_auth.bound ? 800 : 160);
    p.itvl_max = (win || mp) ? 80 : (s_auth.bound ? 800 : 320);

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                               gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void adv_refresh(void)
{
    if (gatt_miot_is_connected()) return;
    /* 广播已经被 Mesh 未配网 beacon 占用时，这里不能 stop —— 那会把
     * 0x1827 的 beacon 一起掐掉，米家就再也发现不了设备。 */
    if (mesh_miot_adv_owns_radio()) return;

    /* ★2026-11-19 这里原来是 `stop(); adv_start();`，是个隐蔽的竞态：
     *   停止广播要等 host task 处理完才真正生效，紧接着调 adv_start 时
     *   ble_gap_adv_active() 仍返回 true，于是 adv_start 第一行就 return 了。
     *   结果：命令打印「meshpair = 1」、`frame` 也打印 25B D05C，
     *   但空口上一直是启动时那 16B 的旧帧（实测 702ABC6A… 反复出现），
     *   米家按旧帧判「version too low」→ 配网 -17，怎么改参数都不动。
     *   现在改成：不在广播中就直接 start；已在广播中就直接替换 payload，
     *   不断流、不依赖 stop 的异步完成。 */
    if (!ble_gap_adv_active()) {
        adv_start();
        return;
    }
    adv_set_payload();
}

/* ======================================================================
 * GAP 事件
 * ==================================================================== */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            gatt_miot_on_connect(event->connect.conn_handle, BLE_ATT_MTU_DFLT);
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "phone connected -> adv stopped");
            relay_on_phone(1);
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
            build_frame(0);
        }
        adv_start();
        ESP_LOGI(TAG, "disconnected reason=0x%02x -> adv resumed", event->disconnect.reason);
        relay_on_phone(0);
        return 0;

    case BLE_GAP_EVENT_MTU:
        gatt_miot_on_mtu(event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u (%s) notify=%d",
                 event->subscribe.attr_handle,
                 gatt_miot_handle_tag(event->subscribe.attr_handle),
                 event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

/* ======================================================================
 * 状态回推
 * ==================================================================== */

/** 把某一条属性推给 App（set 成功后调用，真机也是这么回的） */
static void push_prop(uint16_t piid)
{
    if (!s_auth.logged_in || !gatt_miot_is_connected()) return;

    miot_spec_value_t v;
    memset(&v, 0, sizeof(v));
    v.siid = 7;
    v.piid = piid;
    switch (piid) {
    case 1:  v.type = MIOT_TYPE_BOOL; v.val.u8  = b_on ? 1 : 0; break;
    case 2:  v.type = MIOT_TYPE_U8;   v.val.u8  = (uint8_t)b_bright; break;
    case 3:  v.type = MIOT_TYPE_U16;  v.val.u16 = (uint16_t)b_ct; break;
    default: v.type = MIOT_TYPE_U8;   v.val.u8  = (uint8_t)b_lmode; break;
    }
    miot_device_send_props_changed(&s_dev, &v, 1);
    ESP_LOGI(TAG, "properties_changed 7.%u", piid);
}

/** ★登录成功后主动上报一次全量属性。
 *  米家插件在本地会话建立后靠这一帧建立「实时状态」：它先渲染云端缓存值，
 *  再等设备主动报一次真值；等不到就判本地通道不可用，界面上就是
 *  「连接中」→「连接失败」（logcat 里 cancelOpen + 0 次 read/writeCharacteristic）。
 *  真机（MHCB12G / 夜灯2）在 LOGIN 完成 0x21 之后都是立刻上报。 */
static void push_all_props(void)
{
    if (!s_auth.logged_in || !gatt_miot_is_connected()) return;

    miot_spec_value_t v[7];
    memset(v, 0, sizeof(v));
    size_t n = 0;

    v[n].siid = 7; v[n].piid = 1;    v[n].type = MIOT_TYPE_BOOL; v[n].val.u8  = b_on ? 1 : 0;      n++;
    v[n].siid = 7; v[n].piid = 2;    v[n].type = MIOT_TYPE_U8;   v[n].val.u8  = (uint8_t)b_bright;  n++;
    v[n].siid = 7; v[n].piid = 3;    v[n].type = MIOT_TYPE_U16;  v[n].val.u16 = (uint16_t)b_ct;     n++;
    v[n].siid = 7; v[n].piid = 4;    v[n].type = MIOT_TYPE_U8;   v[n].val.u8  = (uint8_t)b_lmode;   n++;
    v[n].siid = 5; v[n].piid = 1003; v[n].type = MIOT_TYPE_U8;   v[n].val.u8  = (uint8_t)b_bat;     n++;
    v[n].siid = 5; v[n].piid = 1;    v[n].type = MIOT_TYPE_U8;   v[n].val.u8  = (uint8_t)b_chg;     n++;
    v[n].siid = 8; v[n].piid = 1;    v[n].type = MIOT_TYPE_U8;   v[n].val.u8  = (uint8_t)b_dsmode;  n++;

    int rc = miot_device_send_props_changed(&s_dev, v, n);
    ESP_LOGI(TAG, "login -> push %u props rc=%d", (unsigned)n, rc);
}

/* ======================================================================
 * 按键：短按开关灯，长按 3 秒恢复出厂
 * ==================================================================== */

static void btn_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BULB_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int64_t t_down = 0;
    bool down = false;
    bool long_done = false;

    for (;;) {
        int lv = gpio_get_level(BULB_BTN_GPIO);
        int64_t t = esp_timer_get_time();

        if (lv == 0 && !down) {
            down = true; long_done = false; t_down = t;
        } else if (lv == 0 && down && !long_done &&
                   (t - t_down) / 1000 >= BULB_LONG_PRESS_MS) {
            long_done = true;
            ESP_LOGW(TAG, "BOOT held %dms -> factory reset", BULB_LONG_PRESS_MS);
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (lv != 0 && down) {
            down = false;
            if (!long_done) {
                /* ★短按真正的作用是「进入配对/确认窗口」，不是切灯。
                 * 之前只改 2.1 属性，空中帧几乎不变，App 侧看到的就是
                 * 「按了没反应」（2026-11-16 实测）。 */
                s_pair_win_until = now_ms() + 60000;
                build_frame(0);
                adv_refresh();
                ESP_LOGW(TAG, "BOOT 短按 -> 配对窗口 60s（快播 30ms）");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ======================================================================
 * 串口命令
 * ==================================================================== */

/* ======================================================================
 * 广播交替：MiBeacon ↔ SIG 未配网 beacon
 *
 * 为什么必须交替：NimBLE 只有一个 legacy adv 实例，两个形态互斥。
 *  - 手机米家「附近设备」只认 MiBeacon(0xFE95)，没有就列表为空；
 *  - 网关（小爱音箱）只认 SIG 未配网 beacon(0x1827)，没有就不配网。
 * 只能时间分片：各占一半窗口，扫描侧都有机会命中。
 * 手机/网关的连接态下不动广播（否则把正在进行的会话打断）。
 * ==================================================================== */
static bool s_adv_mesh_phase;      /* 当前处于 mesh beacon 阶段 */
static bool s_adv_auto = true;     /* false = 串口手动锁定形态，交替任务停手 */

static void adv_handover(bool to_mesh)
{
    /* 先松手再交接：直接切会因实例被占而刷 adv_send err -2 */
    mesh_miot_set_adv_first(false);
    mesh_miot_set_prov(false);
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(120));
    if (to_mesh) {
        mesh_miot_set_adv_first(true);
        mesh_miot_set_prov(true);
    } else {
        build_frame(0);
        adv_refresh();
    }
    s_adv_mesh_phase = to_mesh;
}

static void adv_cycle_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));   /* 3s 太短：App 发起连接时正好撞上切换窗口，实测连不上 */
        if (!s_adv_auto) continue;
        if (gatt_miot_is_connected()) continue;      /* 会话中不打断 */
        if (mesh_miot_prov_active()) continue;       /* 配网进行中，广播权归 mesh */
        adv_handover(!s_adv_mesh_phase);
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[128];
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
        if (!n) continue;

        if (!strcmp(line, "info")) {
            char st[160]; mesh_miot_status(st, sizeof(st));
            ESP_LOGW(TAG, "pid=0x%04X bound=%d login=%d conn=%d on=%u bright=%ld ct=%ldK meshbit=%u",
                     s_pid, (int)s_auth.bound, (int)s_auth.logged_in,
                     (int)gatt_miot_is_connected(), b_on,
                     (long)b_bright, (long)b_ct, s_mesh_bit);
            ESP_LOGW(TAG, "%s", st);
        } else if (!strcmp(line, "on") || !strcmp(line, "off")) {
            b_on = (line[1] == 'n') ? 1 : 0;
            b_dsmode = b_on ? 2 : 0;
            led_sync();
            push_prop(1);
            ESP_LOGW(TAG, "7.1 = %u", b_on);
        } else if (!strncmp(line, "bright", 6)) {
            const char *a = line + 6;
            while (*a == ' ') a++;
            if (*a) {
                int pct = atoi(a);
                if (pct < 1) pct = 1;
                if (pct > 100) pct = 100;
                b_bright = pct;
                led_sync();
                push_prop(2);
                ESP_LOGW(TAG, "7.2 = %ld%%", (long)b_bright);
            } else {
                ESP_LOGW(TAG, "bright 现在 %ld%%", (long)b_bright);
            }
        } else if (!strncmp(line, "ct", 2)) {
            const char *a = line + 2;
            while (*a == ' ') a++;
            if (*a) {
                int v = atoi(a);
                if (v < 2700) v = 2700;
                if (v > 5000) v = 5000;
                b_ct = v;
                push_prop(3);
                ESP_LOGW(TAG, "7.3 = %ldK（单色灯只记值，不体现在灯珠上）", (long)b_ct);
            } else {
                ESP_LOGW(TAG, "ct 现在 %ldK", (long)b_ct);
            }
        } else if (!strncmp(line, "pid", 3)) {
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (*a) {
                uint16_t v = (uint16_t)strtoul(a, NULL, 0);
                pid_save(v);
                ESP_LOGW(TAG, "PID 写为 0x%04X，重启生效", v);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            } else {
                ESP_LOGW(TAG, "PID 现在 0x%04X", s_pid);
            }
        } else if (!strncmp(line, "mac", 3)) {
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (*a) {
                uint8_t base[6]; int n = 0; const char *p = a;
                while (n < 6 && p[0] && p[1]) {
                    char h2[3] = { p[0], p[1], 0 };
                    base[n++] = (uint8_t)strtoul(h2, NULL, 16);
                    p += 2;
                    if (*p == ':' || *p == '-') p++;
                }
                if (n == 6) {
                    mac_save(base);
                    ESP_LOGW(TAG, "基础 MAC 写为 %02X:%02X:%02X:%02X:%02X:%02X（BLE 用末字节+2），重启生效",
                             base[0], base[1], base[2], base[3], base[4], base[5]);
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                } else {
                    ESP_LOGW(TAG, "mac 需要 12 位 hex，例：mac 10003BCE7203");
                }
            } else {
                ESP_LOGW(TAG, "当前广播 MAC %02X:%02X:%02X:%02X:%02X:%02X",
                         s_mac_air[5], s_mac_air[4], s_mac_air[3],
                         s_mac_air[2], s_mac_air[1], s_mac_air[0]);
            }
        } else if (!strcmp(line, "factory")) {
            ESP_LOGW(TAG, "factory reset: erase NVS");
            nvs_flash_erase();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (!strcmp(line, "reboot")) {
            esp_restart();
        } else if (!strncmp(line, "authmode", 8)) {
            const char *a = line + 8;
            while (*a == ' ') a++;
            if (*a) {
                int m = atoi(a);
                if (m < 0) m = 0;
                if (m > 3) m = 3;
                s_auth_mode = (uint8_t)m;
                build_frame(0);
                adv_refresh();
            }
            ESP_LOGW(TAG, "authmode = %u（FC 第二字节 bit2~3；0=RC4 1=SECURE 2=STANDARD 3=PSK）",
                     s_auth_mode);
        } else if (!strcmp(line, "frame")) {
            /* 打印当前 MiBeacon 帧原始字节，用于和真机广播对拍。
             * 广播是这个设备唯一会被 App 提前判定的东西，出问题必须先看字节。 */
            char buf[3 * 32 + 1];
            int p = 0;
            for (int i = 0; i < s_flen && p < (int)sizeof(buf) - 4; i++)
                p += snprintf(buf + p, sizeof(buf) - p, "%02X ", s_frame[i]);
            buf[p] = 0;
            ESP_LOGW(TAG, "frame(%uB, %s): %s", (unsigned)s_flen,
                     s_auth.bound ? (s_adv_plain ? "bound/plain" : "bound/v5") : "unbound",
                     buf);
        } else if (!strncmp(line, "advmode", 7)) {
            const char *a = line + 7;
            while (*a == ' ') a++;
            if (*a) {
                int m = atoi(a);
                if (m < 0) m = 0;
                if (m > 2) m = 2;
                s_adv_plain = (uint8_t)m;
                build_frame(0);
                adv_refresh();
            }
            ESP_LOGW(TAG, "advmode = %u（0=v5 加密 1=明文16B带0x1011 2=明文12B无对象）", s_adv_plain);
        } else if (!strncmp(line, "meshbit", 7)) {
            const char *a = line + 7;
            while (*a == ' ') a++;
            if (*a) {
                s_mesh_bit = (uint8_t)(atoi(a) ? 1 : 0);
                build_frame(0);
                adv_refresh();
            }
            ESP_LOGW(TAG, "meshbit = %u（FC 首字节 bit7，1=在 MiBeacon 里宣告 Mesh 设备）",
                     s_mesh_bit);
        } else if (!strncmp(line, "muuid", 5)) {
            const char *a = line + 5;
            while (*a == ' ') a++;
            uint8_t u[16];
            int n2 = 0;
            if (strlen(a) == 32) {
                for (int i = 0; i < 16; i++) {
                    unsigned v = 0;
                    if (sscanf(a + i * 2, "%2x", &v) != 1) break;
                    u[i] = (uint8_t)v; n2++;
                }
            }
            if (n2 == 16) {
                mesh_miot_set_uuid(u);
                ESP_LOGW(TAG, "Device UUID 已替换（下一轮配网生效）");
            } else {
                char st[160]; mesh_miot_status(st, sizeof(st));
                ESP_LOGW(TAG, "%s", st);
                ESP_LOGW(TAG, "用法: muuid <32位hex>");
            }
        } else if (!strncmp(line, "mpair", 5)) {
            const char *a = line + 5;
            while (*a == ' ') a++;
            if (*a) {
                s_mesh_pair = (uint8_t)(atoi(a) ? 1 : 0);
                mpair_save(s_mesh_pair);
                build_frame(0);
                adv_refresh();
            }
            ESP_LOGW(TAG, "meshpair = %u（1=按真机 D05C 举手帧广播，用于 Mesh 配网复刻）",
                     s_mesh_pair);
        } else if (!strncmp(line, "meshadv", 7)) {
            const char *a = line + 7;
            while (*a == ' ') a++;
            if (*a) {
                int on = atoi(a) ? 1 : 0;
                s_adv_auto = false;          /* 手动锁定，交替任务停手 */
                adv_handover(on ? true : false);
                ESP_LOGW(TAG, "meshadv=%d（1=Mesh beacon，0=MiBeacon；交替已停，`auto 1` 恢复）", on);
            } else {
                ESP_LOGW(TAG, "meshadv 现在 %d（1=Mesh beacon，0=MiBeacon）",
                         (int)mesh_miot_adv_owns_radio());
            }
        } else if (!strncmp(line, "meshoff", 7)) {
            const char *a = line + 7;
            while (*a == ' ') a++;
            if (*a) { mesh_off_save(atoi(a) ? true : false); ESP_LOGW(TAG, "meshoff=%d，重启生效", atoi(a)); }
            else ESP_LOGW(TAG, "meshoff=%d（1=不注册 Mesh 服务，纯 BLE 形态）", (int)s_mesh_off);
        } else if (!strncmp(line, "auto", 4)) {
            const char *a = line + 4;
            while (*a == ' ') a++;
            if (*a) s_adv_auto = atoi(a) ? true : false;
            ESP_LOGW(TAG, "auto=%d（1=MiBeacon↔Mesh beacon 每 3s 交替）", (int)s_adv_auto);
        } else if (!strncmp(line, "mesh", 4)) {
            const char *a = line + 4;
            while (*a == ' ') a++;
            if (*a) {
                mesh_miot_set_prov(atoi(a) ? true : false);
            }
            char st[160]; mesh_miot_status(st, sizeof(st));
            ESP_LOGW(TAG, "%s prov_active=%d", st, (int)mesh_miot_prov_active());
        } else if (!strncmp(line, "ver", 3)) {
            const char *a = line + 3;
            while (*a == ' ') a++;
            if (*a) {
                gatt_miot_set_ver(atoi(a));
            }
            for (int i = 0; i < gatt_miot_ver_count(); i++) {
                ESP_LOGW(TAG, "  ver #%d = '%s'", i, gatt_miot_ver_name(i));
            }
            ESP_LOGW(TAG, "ver 用法：`ver N` 钉住第 N 条，`ver -1` 恢复每连接轮换");
        } else {
            ESP_LOGW(TAG, "命令: info | on | off | bright 0-100 | ct 2700-6500 | pid [0xXXXX] | "
                          "ver [N|-1] | meshbit [0|1] | mpair [0|1] | meshadv [0|1] | mesh [0|1] | muuid <32hex> | "
                          "mac <12hex> | auto [0|1] | meshoff [0|1] | factory | reboot");
        }
    }
}

/* ======================================================================
 * 启动
 * ==================================================================== */

static void on_sync(void)
{
    uint8_t addr[6];
    if (ble_hs_util_ensure_addr(0) == 0 &&
        ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        /* 广播帧里的 MAC 必须是 LSB 序，nonce 是 mac||pid||cnt 拼的 */
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];
    }
    ESP_LOGI(TAG, "host synced, MAC %02X:%02X:%02X:%02X:%02X:%02X  PID=0x%04X bound=%d",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0], s_pid, s_auth.bound);

    s_idx = 0;
    /* 必须在任何广播启动之前（HCI 在广播开启时会拒绝 Set Random Address） */
    relay_prepare();
    /* Mesh 栈必须在 host 同步后初始化（见 mesh_miot.h 的时序说明），
     * 未配网时它会把 PB-GATT/未配网 beacon 开起来。 */
    if (!s_mesh_off) mesh_miot_on_sync();
    build_frame(s_idx);
    adv_start();
    if (RELAY_TARGET_MAC[0]) relay_start(RELAY_TARGET_MAC);
    gatt_miot_dump_handles();
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

    s_pid = pid_load();
    s_mesh_off = mesh_off_load();
    /* NVS 里的 `pid` 键优先级高于编译默认值（运行期 `pid 0xXXXX` 的遗产）。
     * 换默认值后不 erase-flash 会继续用旧值，这一行就是为了不让人误会。 */
    if (s_pid != BULB_PID_DEFAULT) {
        ESP_LOGW(TAG, "PID 0x%04X 来自 NVS，编译默认 0x%04X；要回默认值请 erase-flash",
                 s_pid, BULB_PID_DEFAULT);
    }

    /* ★独立 MAC：仓库里每个工程一个前缀，避免米家把「反复绑定失败过的地址」
     * 写进 DeviceBlackList 之后，所有工程一起扫不到。
     *   ESP_MAC_BT = 基础 MAC 末字节 +2，所以这里写 0x03 → 广播出去是 …:71:05。
     * 串口 `mac 10003BCE7203` 可改并重启（NVS 键 macb），换地址绕黑名单用。
     *
     * ★★2026-11-20 relay 模式强制冒用真灯 MAC（-40 的头号嫌疑）：
     *   App 与 ESP32 建链时把**连接地址**塞进 /v2/blemesh/auth 的 mac 字段，
     *   而证书里写的是真灯的 MAC；auth 不校验、bind 校验 → 真签名也被 -40。
     *   现在 relay 开着就把 BT 地址设成真灯的（base = 真灯MAC - 2），广播
     *   MAC 字段与 GATT 连接地址同时变成 14:D8:81:E6:95:3C，NV S 覆盖失效。 */
    {
        uint8_t base[6] = { 0x10, 0x00, 0x3B, 0xCE, 0x72, 0x03 };
        s_mesh_pair = mpair_load();   /* 举手帧模式跨重启保留 */
        if (RELAY_TARGET_MAC[0]) {
            unsigned v[6];
            if (sscanf(RELAY_TARGET_MAC, "%x:%x:%x:%x:%x:%x",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                for (int i = 0; i < 6; i++) base[i] = (uint8_t)v[i];
                base[5] -= 2;   /* ESP_MAC_BT = base +2，倒推 base */
                esp_base_mac_addr_set(base);
                ESP_LOGW(TAG, "relay 模式：BT 地址强制 = 真灯 %s（base %02X:%02X:%02X:%02X:%02X:%02X）",
                         RELAY_TARGET_MAC, base[0], base[1], base[2], base[3], base[4], base[5]);
                goto mac_done;
            }
        }
        if (!mac_load(base)) {
            ESP_LOGW(TAG, "MAC 未覆盖，用编译默认");
        } else {
            ESP_LOGW(TAG, "MAC 覆盖自 NVS: %02X:%02X:%02X:%02X:%02X:%02X",
                     base[0], base[1], base[2], base[3], base[4], base[5]);
        }
        esp_base_mac_addr_set(base);
    }
mac_done:;

    ESP_LOGI(TAG, "===== 小米汽车磁吸补光灯 careco.nlight.o654（PID 0x6266）=====");
    if (s_pid == 0) {
        ESP_LOGE(TAG, "PID 还没填（当前 0x0000）—— 米家「附近设备」不会出现本设备");
        ESP_LOGE(TAG, "填法：扫真机 0xFE95 Service Data，第 3~4 字节（小端）即 PID，");
        ESP_LOGE(TAG, "      然后串口执行 `pid 0x1A2B`（会自动重启），不用重新编译。");
    } else {
        ESP_LOGI(TAG, "PID 0x%04X  MAC 后续打印", s_pid);
    }

    led_init();
    led_sync();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();
    fcnt_load();

    memset(&s_port, 0, sizeof(s_port));
    s_port.send_notify = gatt_miot_notify_spec;
    s_port.time_now    = port_time_now;
    s_port.random      = port_random;
    s_port.log         = port_log;

    miot_device_init(&s_dev, &s_port);
    miot_device_attach(&s_dev, b_props, sizeof(b_props) / sizeof(b_props[0]),
                       NULL, NULL, NULL);

    miot_auth_port_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.notify_0010 = gatt_miot_notify_auth_0010;
    ap.notify_0019 = gatt_miot_notify_auth_0019;
    ap.notify_0016 = gatt_miot_notify_0016;
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
    ble_svc_gap_device_name_set(BULB_NAME);
    gatt_miot_set_pid(s_pid);          /* 0x0002 读回值要与实际 PID 一致 */
    gatt_miot_register_svcs();
    gatt_miot_bind(&s_auth, &s_dev);
    ble_att_set_preferred_mtu(247);

    /* ★Mesh 服务(0x1828 Proxy / 0x1827 Prov)必须在 ble_gatts_start()（host 启动时
     * 建表）之前进注册表，否则 pb_gatt_srv 的 resolve_svc_handles 断言崩溃。
     * mac 是人读序（mac[0] 是 OUI 首字节），mesh Device UUID 里的 MAC 也按这个序。 */
    if (!s_mesh_off) mesh_miot_early_init(s_pid, mac);
    else ESP_LOGW(TAG, "meshoff=1：不注册 0x1827/0x1828，纯 BLE 形态");

    nimble_port_freertos_init(host_task);
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);
    xTaskCreate(cli_task, "cli", 4096, NULL, 4, NULL);
    xTaskCreate(adv_cycle_task, "advcyc", 3072, NULL, 2, NULL);

    uint32_t last_adv = 0;

    for (;;) {
        uint32_t t = now_ms();
        miot_auth_poll(&s_auth, t);

        if (s_auth.logged_in && !s_logged_in) {
            s_logged_in = 1;
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            ESP_LOGI(TAG, "*** session established: spec channel ready ***");
            /* ★2026-11-18 A/B：真机米家 BLE 的时序是 App 主动发 GET properties，
             * 设备不该抢跑。之前登录完成立刻推 7 条全量属性，App 1.1s 后必断；
             * 现在改为等 App 自己来 GET（handle_get 已有完整实现）。 */
            if (PUSH_ON_LOGIN) push_all_props();
            else ESP_LOGI(TAG, "login: wait for App GET (no push)");
        }

        /* set 成功后回推（真机同款时序：先落值再报） */
        uint16_t piid = s_dirty_piid;
        if (piid) {
            s_dirty_piid = 0;
            led_sync();
            push_prop(piid);
        }

        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d ***", s_was_bound);
            build_frame(0);
            adv_refresh();
        }

        if (!gatt_miot_is_connected() && t - last_adv >= ADV_PERIOD_MS) {
            last_adv = t;
            s_fcnt++;
            if ((s_fcnt % 10) == 0) fcnt_save();
            if (build_frame(0) >= 0) adv_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
