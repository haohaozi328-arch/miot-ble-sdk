/**
 * @file gatt_miot.c
 * @brief 0xFE95 服务与四个特征值的 NimBLE 实现
 */
#include "gatt_miot.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "MIOT-GATT";

/* 服务与特征值 UUID（SIG 分配给小米的 MiBeacon 服务 0xFE95）
 * 米家 App 侧的完整形式是 0000FE95-0000-1000-8000-00805F9B34FB */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFE95);
static const ble_uuid16_t s_c02_uuid = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t s_c04_uuid = BLE_UUID16_INIT(0x0004);
static const ble_uuid16_t s_c10_uuid = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t s_c13_uuid = BLE_UUID16_INIT(0x0013);
static const ble_uuid16_t s_c19_uuid = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t s_c1a_uuid = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t s_c1b_uuid = BLE_UUID16_INIT(0x001B);

static miot_auth_t   *s_auth;
static miot_device_t *s_dev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

/* 上层可以覆盖（定义强符号）来接管「App 发请求」这个事件。
 * 默认空实现，保证单独编译这个文件也能过。 */
__attribute__((weak)) void miot_demo_app_req_hook(void) { }
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;

static uint16_t s_h10, s_h19, s_h1a, s_h1b, s_h02, s_h04, s_h13;

/* 最后一次收到手机写入的时刻（esp_timer ms）。app_main 用它做「手机长时间
 * 没动静才收尾」的看门狗 —— 之前按登录时刻 +60s 硬断，会让 App 每 60s
 * 重连一次，页面永远在「设备初始化中」。 */
static volatile uint32_t s_rx_ms;

/* 真机 GATT 里就有的只读信息特征（mijia-protocol/docs/00-index.md GATT map）。
 * ★App 优化连接模型在 LOGIN 后会 read 0x0004 拿固件版本 —— 上一轮
 * "read version failed" 的元凶就是我们的表里没有 0x0004，GATT read 直接
 * ATT error。0x0002=PID(LE u16)、0x0013=序列号，一并补齐防后续卡点。 */
/* PID 由 app_main 在启动时按 Kconfig 选的型号注入（gatt_miot_set_pid），
 * 不再硬编码 —— 这个工程要模拟 6 种型号。 */
static uint8_t s_pid_le[2] = { 0x00, 0x00 };

/** 注入当前型号的 PID（0x0002 特征值要按型号回真实值） */
void gatt_miot_set_pid(uint16_t pid)
{
    s_pid_le[0] = (uint8_t)(pid & 0xFF);
    s_pid_le[1] = (uint8_t)(pid >> 8);
}
static const char     s_fw_ver[] = "1.0.0_0426";     /* 与真机 MCU 版本格式一致 */
static const char     s_serial[] = "0815/000001";

/* 各级通道的收发计数，出问题时先看这里：写得进来但没有回包，通常是状态机
 * 停在别的分支；一条写都没有，则是 App 压根没认这个服务。 */
static uint32_t s_wr10, s_wr19, s_wr1a, s_wr1b;

/* 0x0010 是同一条通道上的多路复用：认证命令（A4/A2/24，长度 1 或 4）
 * 和认证之后的数据帧（spec，ag2 封装，首字节 0x00）都从这里进来。
 * 按已知认证命令白名单分流，剩下的都交给 spec 层。 */
static int is_auth_cmd(const uint8_t *d, uint16_t len)
{
    if (len == 1 && d[0] == 0xA4) return 1;                    /* 建链 */
    if (len != 4) return 0;
    if (d[1] || d[2] || d[3]) return 0;
    return d[0] == 0x24 || (d[0] & 0x7F) == 0x22;              /* LOGIN / REGISTER */
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    uint16_t u = ble_uuid_u16(ctxt->chr->uuid);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "rd %04X from phone", u);
        switch (u) {
        case 0x0002:
            return os_mbuf_append(ctxt->om, s_pid_le, sizeof(s_pid_le));
        case 0x0004:
            return os_mbuf_append(ctxt->om, s_fw_ver, sizeof(s_fw_ver) - 1);
        case 0x0013:
            return os_mbuf_append(ctxt->om, s_serial, sizeof(s_serial) - 1);
        case 0x001B: {
            /* spec 版本字节（优化连接模型 LOGIN 后也会读它） */
            static const uint8_t spec_ver = 0x02;
            return os_mbuf_append(ctxt->om, &spec_ver, 1);
        }
        default:
            return 0;
        }
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);

    switch (u) {
    case 0x0010:
        s_wr10++;
        /* 命令很短（1B 或 4B），打印首字节足够定位是哪一步 */
        ESP_LOGI(TAG, "wr 0010 #%lu len=%u cmd=%02X", (unsigned long)s_wr10, got, got ? buf[0] : 0);
        if (is_auth_cmd(buf, got)) {
            if (s_auth) miot_auth_on_0010(s_auth, buf, got);
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x0019:
        s_wr19++;
        ESP_LOGD(TAG, "wr 0019 #%lu len=%u", (unsigned long)s_wr19, got);
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x001A:
        s_wr1a++;
        ESP_LOGI(TAG, "wr 001A #%lu len=%u cmd=%02X", (unsigned long)s_wr1a, got, got ? buf[0] : 0);
        /* 通知上层的「App 主动发起了一次属性请求」。
         * 米家插件点「测量」时并不会发任何“开始称重”的指令，唯一的痕迹就是
         * 连发两次属性请求（实测间隔稳定 6s；平时心跳是 30~36s）。上层靠这个
         * 间隔判断“用户点了测量”，替它把称重数据推上去。 */
        miot_demo_app_req_hook();
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    case 0x001B:
        s_wr1b++;
        ESP_LOGI(TAG, "wr 001B #%lu len=%u cmd=%02X", (unsigned long)s_wr1b, got, got ? buf[0] : 0);
        if (s_dev) miot_device_on_rx(s_dev, buf, got);
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_c02_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h02,
    },
    {
        .uuid = &s_c04_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h04,
    },
    {
        .uuid = &s_c10_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h10,
    },
    {
        .uuid = &s_c13_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h13,
    },
    {
        .uuid = &s_c19_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h19,
    },
    {
        .uuid = &s_c1a_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1a,
    },
    {
        .uuid = &s_c1b_uuid.u,
        .access_cb = gatt_access,
        /* ★必须带 READ：优化连接模型 LOGIN 后 read 0x001B 拿 spec 版本；
         *   必须带 NOTIFY：优化模型的 spec RPC 通道挂在 0x001B（对象 wy0），
         *   温湿度计那种「全走 0x0010」在驱蚊器上 App 是不收的。 */
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h1b,
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

void gatt_miot_register_svcs(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "0xFE95 service registered (0002/0004/0010/0013/0019/001A/001B)");
}

void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev)
{
    s_auth = auth;
    s_dev = dev;
}

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu)
{    s_conn = conn_handle;
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    s_wr10 = s_wr19 = s_wr1a = s_wr1b = 0;
    ESP_LOGI(TAG, "connected conn=%u mtu=%u", conn_handle, s_mtu);
}

void gatt_miot_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected (wr 0010=%lu 0019=%lu 001A=%lu 001B=%lu)",
             (unsigned long)s_wr10, (unsigned long)s_wr19,
             (unsigned long)s_wr1a, (unsigned long)s_wr1b);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = BLE_ATT_MTU_DFLT;
}

void gatt_miot_dump_handles(void)
{
    /* value handle；CCCD 一般紧跟其后（+1/+2 视是否多个描述符而定）。
     * App 的 SUBSCRIBE 事件里给的是 CCCD handle，对照这里就能知道它订的是哪条通道。 */
    ESP_LOGI(TAG, "handles: 0010=%u 0019=%u 001A=%u 001B=%u (cccd ~ +1)",
             s_h10, s_h19, s_h1a, s_h1b);
}

void gatt_miot_on_mtu(uint16_t mtu)
{
    s_mtu = mtu ? mtu : BLE_ATT_MTU_DFLT;
    ESP_LOGI(TAG, "mtu -> %u", s_mtu);
}

int gatt_miot_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

uint32_t gatt_miot_rx_ms(void)
{
    return s_rx_ms;
}

void gatt_miot_disconnect(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    /* 模拟真机会话收尾：温湿度计实测 App 在 LOGIN 完成收完数据 ~4s 后主动断开，
     * 页面随即翻面。驱蚊器因 hook 模块把 PSK 连接重路由，Core 侧完成信号可能
     * 没传回原调用链导致会话挂死 —— 设备侧主动断，逼页面走「断开后展示数据」
     * 的路径。 */
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static uint16_t s_notify_fail;

static int notify_chr(uint16_t handle, const uint8_t *d, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || handle == 0) return -1;

    uint16_t max = s_mtu > 3 ? (uint16_t)(s_mtu - 3) : 20;
    if (len > max) {
        /* 协议层没按 MTU 分片，说明对端协商出来的 MTU 比预期小（或没协商）。
         * 直接截断会送出半帧，宁可不发。 */
        ESP_LOGW(TAG, "notify h=%u len=%u > mtu-3=%u, dropped", handle, (unsigned)len, max);
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)len);
    if (!om) return -1;

    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0 && rc != BLE_HS_EDONE) {
        /* 未订阅时返回 ATT 错误是正常的：App 还没写 CCCD。
         * 但如果是「一直没人订阅」，这里就是数据到不了 App 的原因，
         * 所以前几次用 WARN 打出来，别让它静默消失。 */
        if (s_notify_fail < 6) {
            s_notify_fail++;
            ESP_LOGW(TAG, "notify h=%u len=%u rc=%d (nobody subscribed?)",
                     handle, (unsigned)len, rc);
        }
    } else {
        s_notify_fail = 0;
    }
    return (rc == 0 || rc == BLE_HS_EDONE) ? 0 : -1;
}

int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* ★spec 数据双通道同发（2026-11-07 logcat 定位）：
     * - 0x0010：温湿度计实测的页面数据通道（非优化模型唯一路径）；
     * - 0x001B：优化连接模型（zimi.mosq.v1 命中 is6Optimize...）多订出来的
     *   通道（hook: "BLE notify req ... 0000001b _m_j.wy0"）。
     * 两条 App 都订阅了（0x001A 也订），各收一份由 spec tid 幂等去重；
     * 只发一边时另一边的消费方就一直等不到，页面停在「设备初始化」。 */
    notify_chr(s_h10, d, len);

    /* ★ag2 链路层应答必须回到 0x001A（23:45 定位）。
     * App 把自己的 spec 帧写在 0x001A 上，然后就在 0x001A 等链路层 ACK；
     * 我们之前把 ACK 只发到 0010/001B，它永远收不到应答，于是：
     *   wr 001A #N len=6: 00 00 00 00 03 00   ← START，声明 3 个分片
     *   之后一个分片都不来，30s 后重试（页面上就是「点了没反应」）。
     * 形状固定是 4 字节 00 00 01 xx，只把这种控制帧补一路到 001A，
     * 数据帧仍走 0010/001B，避免同一条数据被两条通道各处理一次。 */
    if (len == 4 && d[0] == 0 && d[1] == 0 && d[2] == 1)
        notify_chr(s_h1a, d, len);

    return notify_chr(s_h1b, d, len);
}

int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h10, d, len);
}

int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h19, d, len);
}
