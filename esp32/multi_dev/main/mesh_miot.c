/**
 * @file mesh_miot.c
 * @brief 米家 BLE Mesh 承载层实现（ESP-IDF NimBLE Mesh = Zephyr 风格 API）
 *
 * ★2026-11-13 终局方案：云端档案是「蓝牙 Mesh」，Mesh 网关不走 GATT 连接，
 * 唯一通路是 SIG Mesh 配网 + Mesh 广播消息。本模块把协议栈挂进现有 app。
 *
 * Device UUID 候选格式（来源：社区逆向，telink mesh for 小米系列文章）：
 *   [0..1]  CID    LE（小米 0x038F -> 8F 03）
 *   [2]     PID 标识（示例值 0x1A，含义未明）
 *   [3..6]  ProductID LE 4 字节（0x30D9 -> D9 30 00 00）
 *   [7..12] MAC 人读序 6 字节
 *   [13]    FeatureFlag
 *   [14..15] RFU
 * 格式未获官方确认，串口 `muuid <32hex>` 可现场整帧替换做对照实验。
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "mesh/mesh.h"

#include "mesh_miot.h"

static const char *TAG = "MIOT-MESH";

#define MIOT_MESH_CID   0x038F  /* 小米 Bluetooth SIG 公司 ID */

/* ---------------- 状态 ---------------- */
static uint8_t  s_uuid[16];
static uint16_t s_pid;
static bool     s_prov_on;          /* 配网承载开关（PB-ADV/PB-GATT） */
static bool     s_inited;

bool mesh_miot_provisioned(void) { return bt_mesh_is_provisioned(); }

/* bt_mesh_prov_active() 是 mesh 内部 API（prov.h 不对外），这里用回调自己记账 */
static bool s_link_active;
bool mesh_miot_prov_active(void) { return s_link_active; }

bool mesh_miot_adv_owns_radio(void)
{
    /* 只要配网承载开着，Mesh 层就可能在发 beacon（未配网 beacon 每 ~2s 一帧），
     * 也可能持有 PB-GATT 的可连接广播。宿主 app 一律让位，避免 adv busy 打架。 */
    return s_prov_on && !bt_mesh_is_provisioned();
}

/* ---------------- vendor model：所有下行一律记录（抓包窗口） ---------------- */
static int vnd_recv(struct bt_mesh_model *model,
                    struct bt_mesh_msg_ctx *ctx,
                    struct os_mbuf *buf)
{
    ESP_LOGW(TAG, "VND rx model=0x%06X src=0x%04X dst=0x%04X len=%d:",
             (unsigned)(model->id), ctx->addr, ctx->recv_dst, buf->om_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf->om_data, buf->om_len, ESP_LOG_WARN);
    return 0;
}

/* 注册 opcode 0x00~0x0F 全段：真实 opcode 未知，先全收下来看网关说什么 */
static struct bt_mesh_model_op miot_spec_ops[] = {
    { BT_MESH_MODEL_OP_3(0x00, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x01, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x02, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x03, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x04, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x05, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x06, MIOT_MESH_CID), 0, vnd_recv },
    { BT_MESH_MODEL_OP_3(0x07, MIOT_MESH_CID), 0, vnd_recv },
    BT_MESH_MODEL_OP_END,
};

static struct bt_mesh_model vnd_models[] = {
    /* 0x038f0000 = MIOT Spec Server；0x038f0002 = Mijia Server（文档表）。
     * 两个都挂上，下行全进 vnd_recv 日志。 */
    BT_MESH_MODEL_VND(MIOT_MESH_CID, 0x0000, miot_spec_ops, NULL, NULL),
    BT_MESH_MODEL_VND(MIOT_MESH_CID, 0x0002, miot_spec_ops, NULL, NULL),
};

static const struct bt_mesh_health_srv_cb health_srv_cb = {0};
static struct bt_mesh_health_srv health_srv = { .cb = &health_srv_cb };
static struct bt_mesh_model_pub health_pub;

static void health_pub_init(void)
{
    health_pub.msg = BT_MESH_HEALTH_FAULT_MSG(0);
}

static struct bt_mesh_model root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
    BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
};

static struct bt_mesh_elem elements[] = {
    BT_MESH_ELEM(0, root_models, vnd_models),
};

static const struct bt_mesh_comp comp = {
    .cid = MIOT_MESH_CID,
    .elem = elements,
    .elem_count = ARRAY_SIZE(elements),
};

/* ---------------- 配网事件（全部打日志） ---------------- */
static void prov_link_open(bt_mesh_prov_bearer_t bearer)
{
    s_link_active = true;
    ESP_LOGW(TAG, "*** PROV LINK OPEN bearer=%s ***",
             bearer == BT_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
}

static void prov_link_close(bt_mesh_prov_bearer_t bearer)
{
    s_link_active = false;
    ESP_LOGW(TAG, "*** PROV LINK CLOSE bearer=%s ***",
             bearer == BT_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    s_link_active = false;
    ESP_LOGW(TAG, "*** PROVISIONED net_idx=%u primary=0x%04X ***", net_idx, addr);
    /* 配网完成后停止未配网 beacon；Mesh 层自己转入 Secure Beacon */
    s_prov_on = false;
}

static void prov_reset(void)
{
    ESP_LOGW(TAG, "*** MESH RESET -> 重新进入未配网 beacon ***");
    s_prov_on = true;
    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static const struct bt_mesh_prov prov = {
    .uuid        = s_uuid,
    .oob_info    = 0,
    .output_size = 0,
    .output_actions = 0,
    .input_size  = 0,
    .input_actions = 0,
    .link_open   = prov_link_open,
    .link_close  = prov_link_close,
    .complete    = prov_complete,
    .reset       = prov_reset,
};

/* ---------------- UUID ---------------- */
void mesh_miot_set_uuid(const uint8_t u[16])
{
    memcpy(s_uuid, u, 16);
    ESP_LOGW(TAG, "dev uuid -> %02X %02X %02X %02X %02X %02X %02X %02X "
                  "%02X %02X %02X %02X %02X %02X %02X %02X",
             s_uuid[0], s_uuid[1], s_uuid[2], s_uuid[3],
             s_uuid[4], s_uuid[5], s_uuid[6], s_uuid[7],
             s_uuid[8], s_uuid[9], s_uuid[10], s_uuid[11],
             s_uuid[12], s_uuid[13], s_uuid[14], s_uuid[15]);
}

static void build_uuid_candidate0(uint16_t pid, const uint8_t mac_human[6])
{
    uint8_t u[16] = {0};
    u[0] = 0x8F; u[1] = 0x03;                 /* CID 0x038F LE */
    u[2] = 0x1A;                              /* 候选标识（示例值） */
    u[3] = (uint8_t)(pid & 0xFF);
    u[4] = (uint8_t)(pid >> 8);
    u[5] = 0x00; u[6] = 0x00;                 /* ProductID LE 4B */
    memcpy(&u[7], mac_human, 6);              /* MAC 人读序 */
    u[13] = 0x00;                             /* FeatureFlag */
    u[14] = 0x00; u[15] = 0x00;               /* RFU */
    mesh_miot_set_uuid(u);
}

/* ---------------- 对外接口 ---------------- */
void mesh_miot_set_prov(bool on)
{
    if (on && !s_prov_on) {
        int rc = bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
        ESP_LOGW(TAG, "prov_enable rc=%d (0=OK, beacon 开跑)", rc);
        s_prov_on = (rc == 0);
    } else if (!on && s_prov_on) {
        int rc = bt_mesh_prov_disable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
        ESP_LOGW(TAG, "prov_disable rc=%d", rc);
        s_prov_on = false;
    }
}

void mesh_miot_status(char *out, size_t cap)
{
    snprintf(out, cap,
             "mesh: inited=%d provisioned=%d prov_on=%d uuid=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             s_inited, bt_mesh_is_provisioned(), s_prov_on,
             s_uuid[0], s_uuid[1], s_uuid[2], s_uuid[3],
             s_uuid[4], s_uuid[5], s_uuid[6], s_uuid[7],
             s_uuid[8], s_uuid[9], s_uuid[10], s_uuid[11],
             s_uuid[12], s_uuid[13], s_uuid[14], s_uuid[15]);
}

void mesh_miot_early_init(uint16_t pid, const uint8_t mac_human[6])
{
    if (s_inited) return;
    s_pid = pid;
    build_uuid_candidate0(pid, mac_human);
    health_pub_init();

    /* ★时序关键（本移植的约定）：
     *   启动前  bt_mesh_register_gatt() —— 把 Proxy(0x1828)/Prov(0x1827) 服务
     *           追加进 GATT 注册表，host 启动 ble_gatts_start() 建表时收编；
     *   同步后  bt_mesh_init() —— proxy_init 末尾 resolve_svc_handles()
     *           按 UUID 找句柄，此时表已建好，才能命中。
     * 两步放同一边都会在 pb_gatt_srv.c resolve 断言崩溃（两边都实测过）。 */
    bt_mesh_register_gatt();
    ESP_LOGW(TAG, "mesh gatt svcs registered (pre-start) cid=0x%04X pid=0x%04X", MIOT_MESH_CID, pid);
}

void mesh_miot_on_sync(void)
{
    if (!s_inited) {
        int rc = bt_mesh_init(BLE_ADDR_PUBLIC, &prov, &comp);
        if (rc) {
            ESP_LOGE(TAG, "bt_mesh_init rc=%d —— Mesh 栈未挂载", rc);
            return;
        }
        s_inited = true;
        ESP_LOGW(TAG, "bt_mesh_init OK (post-sync) cid=0x%04X pid=0x%04X elems=1 models=cfg+health+vnd(0x0000/0x0002)",
                 MIOT_MESH_CID, s_pid);

        /* ★ESP 移植要求应用自己启动 Mesh 广播线程（porting.h: mesh_adv_thread）。
         * 不启动的话 bt_mesh_adv_update 投递的事件无人消费，第一次
         * bt_mesh_prov_enable 就 Load access fault 崩溃。 */
        {
            extern void mesh_adv_thread(void *args);
            if (xTaskCreate(mesh_adv_thread, "mesh_adv", 4096, NULL,
                            9 /* BLE_MESH_ADV_TASK_PRIO */, NULL) != pdPASS) {
                ESP_LOGE(TAG, "mesh_adv task create failed");
            }
        }
    }
    /* 未配网状态下开启配网承载（真实 Mesh 设备出厂即广播未配网 beacon） */
    if (!bt_mesh_is_provisioned()) {
        mesh_miot_set_prov(true);
    }
}
