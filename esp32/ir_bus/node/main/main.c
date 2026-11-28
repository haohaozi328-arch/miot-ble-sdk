/* ir_bus NODE（"N"）—— BLE GATT 接指令 + 执行 + 回状态
 *
 * 职责：不碰红外，只听总线。
 *   中央（Hub / PC / 手机）写 CMD → 驱动 GPIO4 继电器 → 通过 STATE 通知回报
 *
 * 指令语义是「绝对值」，不是翻转：
 *   BUS_CMD_ON  (0x12 VOL+)    → 置高（开）
 *   BUS_CMD_OFF (0x13 VOL−)    → 置低（关）
 *   BUS_CMD_OFF_ALT (0x2F)     → 置低（兼容旧映射）
 *   BUS_CMD_TOGGLE (0x15 POWER)→ 翻转（和真电视的电源键一致，仅调试）
 * 因为红外单向无回执，翻转会让状态漂移，正式用法走 VOL+/VOL−。
 *
 * 为什么不用电源键当开关：米家索尼遥控只有一颗电源键，实测"打开电视"和
 * "关闭电视"发出来都是 0x15，分不出开还是关。
 *
 * 单板验证：
 *   1. 烧这块板 → 串口 "IR-NODE ready"
 *   2. PC: python tools/ble_peer.py drive --name IR-NODE --cmd 0x15
 *      → GPIO4 拉高，STATE 通知 1
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "../../common/ble_bus.h"

#define RELAY_GPIO  GPIO_NUM_4
static const char *DEV_NAME = "IR-NODE";

static ble_uuid128_t s_svc_uuid   = BUS_SVC_UUID_INIT;
static ble_uuid128_t s_cmd_uuid   = BUS_CMD_UUID_INIT;
static ble_uuid128_t s_state_uuid = BUS_STATE_UUID_INIT;

static uint16_t s_cmd_val_handle;
static uint16_t s_state_val_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type;
static bool     s_state_sub;
static uint8_t  s_relay;          /* 0 = 断，1 = 通 */

static void adv_start(void);

static void state_notify(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_state_sub) return;
    uint8_t v = s_relay;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&v, 1);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn, s_state_val_handle, om);
    if (rc) printf("[node] state notify rc=%d\n", rc);
}

static void set_relay(int on)
{
    s_relay = on ? 1 : 0;
    gpio_set_level(RELAY_GPIO, s_relay);
    printf("[%6llums] EXEC relay=%d (GPIO%d=%d)\n",
           (unsigned long long)(esp_timer_get_time() / 1000), s_relay,
           (int)RELAY_GPIO, s_relay);
    state_notify();
}

/* 心跳：连上并订阅后每 3s 主动推一次状态。
 * 用途是让「PC 纯接收」可验证 —— 不用先写指令，也能看到 ESP 在发。 */
static void heartbeat_task(void *param)
{
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_state_sub) {
            printf("[%6llums] HB relay=%u\n",
                   (unsigned long long)(esp_timer_get_time() / 1000), (unsigned)s_relay);
            state_notify();
        }
    }
}

static void apply_cmd(uint8_t cmd, uint8_t arg)
{
    switch (cmd) {
    case BUS_CMD_ON:
        printf("[%6llums] MAP cmd=0x%02X -> ON\n",
               (unsigned long long)(esp_timer_get_time() / 1000), cmd);
        set_relay(1);
        break;
    case BUS_CMD_OFF:
    case BUS_CMD_OFF_ALT:
        printf("[%6llums] MAP cmd=0x%02X -> OFF\n",
               (unsigned long long)(esp_timer_get_time() / 1000), cmd);
        set_relay(0);
        break;
    case BUS_CMD_TOGGLE:
        printf("[%6llums] MAP cmd=0x%02X -> TOGGLE\n",
               (unsigned long long)(esp_timer_get_time() / 1000), cmd);
        set_relay(!s_relay);
        break;
    default:
        printf("[%6llums] MAP cmd=0x%02X arg=%u -> 未映射，忽略\n",
               (unsigned long long)(esp_timer_get_time() / 1000), cmd, arg);
        break;
    }
}

static int bus_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (attr_handle == s_cmd_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        bus_msg_t m = { 0, 0, 0 };
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(m)) len = sizeof(m);
        if (len > 0) os_mbuf_copydata(ctxt->om, 0, len, &m);
        printf("[%6llums] RX cmd=0x%02X arg=%u seq=%u (%u bytes)\n",
               (unsigned long long)(esp_timer_get_time() / 1000),
               m.cmd, m.arg, m.seq, len);
        apply_cmd(m.cmd, m.arg);
        return 0;
    }

    if (attr_handle == s_state_val_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = s_relay;
        return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_cmd_uuid.u,
        .access_cb = bus_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_cmd_val_handle,
    },
    {
        .uuid = &s_state_uuid.u,
        .access_cb = bus_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_state_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            printf("[node] 中央已连接 (conn=%d)\n", s_conn);
        } else {
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        printf("[node] 中央断开 (reason=%d)\n", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_state_sub = false;
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_state_val_handle) {
            s_state_sub = event->subscribe.cur_notify;
            printf("[node] 对端%s了状态通知\n", s_state_sub ? "订阅" : "取消订阅");
        }
        return 0;

    default:
        return 0;
    }
}

static void adv_start(void)
{
    /* 广播里直接带状态：company 0xFFFF + tag 0x01 + relay
     * 这样 PC/手机「不连接」也能扫到这个节点在不在、是通还是断。 */
    static uint8_t s_mfg[4];

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    s_mfg[0] = 0xFF;      /* company id, 小端 */
    s_mfg[1] = 0xFF;      /* 0xFFFF = 自定义/测试用 */
    s_mfg[2] = 0x01;      /* tag: 1 = ir_bus node state */
    s_mfg[3] = s_relay;   /* 0 = 断, 1 = 通 */
    fields.mfg_data = s_mfg;
    fields.mfg_data_len = sizeof(s_mfg);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) printf("[node] adv fields rc=%d\n", rc);

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) printf("[node] adv start rc=%d\n", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    adv_start();
    printf("[node] BLE 就绪，广播名 %s\n", DEV_NAME);
}

static void on_reset(int reason)
{
    printf("[node] BLE reset, reason=%d\n", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    printf("\n=== IR-NODE ready: relay -> GPIO%d ===\n", (int)RELAY_GPIO);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(RELAY_GPIO, 0);
    s_relay = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { printf("[node] nimble init 失败: %d\n", err); return; }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEV_NAME);

    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);

    nimble_port_freertos_init(host_task);

    xTaskCreate(heartbeat_task, "hb", 3072, NULL, 3, NULL);
}
