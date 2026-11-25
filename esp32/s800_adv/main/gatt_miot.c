/**
 * @file gatt_miot.c
 * @brief 0xFE95 服务与四个特征值的 NimBLE 实现
 */
#include "gatt_miot.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "MIOT-GATT";

/* 服务与特征值 UUID（SIG 分配给小米的 MiBeacon 服务 0xFE95）
 * 米家 App 侧的完整形式是 0000FE95-0000-1000-8000-00805F9B34FB */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xFE95);
/* ★特征集必须与真机逐条一致（2026-11-14 用真机 dump 对齐）：
 *   0001 0002 0004 0010 0013 0014 0016 0017 0018 0019 001A 001B
 * 缺一条 App 服务发现后就走错分支：实测缺 0016/0017/0018 时，App 只订阅
 * 0019+0010、一条 spec GET 都不发，14s 后 reason=0x213 主动断开。
 * 顺序也不要动——真机句柄是 0001=16 … 001B=45，App 侧有按句柄的兼容逻辑。 */
static const ble_uuid16_t s_c01_uuid = BLE_UUID16_INIT(0x0001);
static const ble_uuid16_t s_c02_uuid = BLE_UUID16_INIT(0x0002);
static const ble_uuid16_t s_c04_uuid = BLE_UUID16_INIT(0x0004);
static const ble_uuid16_t s_c10_uuid = BLE_UUID16_INIT(0x0010);
static const ble_uuid16_t s_c13_uuid = BLE_UUID16_INIT(0x0013);
static const ble_uuid16_t s_c14_uuid = BLE_UUID16_INIT(0x0014);
static const ble_uuid16_t s_c16_uuid = BLE_UUID16_INIT(0x0016);
static const ble_uuid16_t s_c17_uuid = BLE_UUID16_INIT(0x0017);
static const ble_uuid16_t s_c18_uuid = BLE_UUID16_INIT(0x0018);
static const ble_uuid16_t s_c19_uuid = BLE_UUID16_INIT(0x0019);
static const ble_uuid16_t s_c1a_uuid = BLE_UUID16_INIT(0x001A);
static const ble_uuid16_t s_c1b_uuid = BLE_UUID16_INIT(0x001B);

static miot_auth_t   *s_auth;
static miot_device_t *s_dev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;

static uint16_t s_h01, s_h02, s_h04, s_h10, s_h13, s_h14;
static uint16_t s_h16, s_h17, s_h18, s_h19, s_h1a, s_h1b;

/* 订阅状态（按 CCCD 的 attr handle 记）与本帧来路面。
 * ★这两个是「App 收不到应答」的根因：登录后 App 把 ag2 帧写到 **0x001A**，
 *   设备却把 ACK/应答一律 notify 到 0x0010 / 0x0019 —— App 在 001A 上收不到，
 *   于是每 6 秒重发同一条 `00 00 00 00 01 00`，spec 请求一条都不发。
 *   实测对照 _rb10.log（multi_dev 成功那轮）：App 第一帧 ag2 ACK 之后 60ms
 *   就发版本交换 `01 00 00 00 …`；我们这一版卡在同一位置。 */
static uint8_t  s_sub[64];
static uint16_t s_rx_chan;

void gatt_miot_on_subscribe(uint16_t attr_handle, int notify)
{
    if (attr_handle && attr_handle < sizeof(s_sub)) s_sub[attr_handle] = notify ? 1 : 0;
}

/** 应答/通知该投哪条特征：优先「本帧进来的那条」，其次 001A，最后回退 */
static uint16_t pick_chan(uint16_t fallback)
{
    if (s_rx_chan && s_rx_chan < sizeof(s_sub) && s_sub[s_rx_chan]) return s_rx_chan;
    if (s_h1a && s_h1a < sizeof(s_sub) && s_sub[s_h1a]) return s_h1a;
    return fallback;
}

/* 0x0002 PID（LE u16）与 0x0004 固件版本：App 在 LOGIN 后 read 这两条做设备确认，
 * 读失败会直接放弃初始化（multi_dev 上就是这么卡住的，见 docs/troubleshooting.md）。 */
static uint8_t  s_pid_le[2] = { 0x00, 0x00 };
/* ★必须是 `x.y.z_zzzz` 这个形状：米家原生层按固定格式解析固件版本，
 * 给 "1.0.0" 这类短串解析失败 → 判定设备异常 → 10s 后主动断开重连，
 * 期间一条 spec GET 都不发（2026-11-14 实测）。multi_dev 也是这个值。 */
static const char s_fw_ver[] = "9.9.9_9999";
static uint8_t  s_devid[20];
static uint8_t  s_devid_len;

void gatt_miot_set_pid(uint16_t pid)
{
    s_pid_le[0] = (uint8_t)(pid & 0xFF);
    s_pid_le[1] = (uint8_t)(pid >> 8);
}

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
        /* App 初始化会 read 0002/0004/0013/0014/001B；任何一条回 ATT error
         * 都可能让它判定"设备不正常"然后不再发任何数据请求。 */
        ESP_LOGI(TAG, "rd %04X", u);
        const miot_auth_identity_t *id = s_auth ? miot_auth_identity(s_auth) : NULL;
        switch (u) {
        case 0x0001: {
            /* Token（真机 12B，write+notify，没有 READ 属性，这里只是兜底）。
             * 标准 ECDH 型号不用它，但特征必须存在，否则 App 的服务发现结果
             * 与真机不一致，后续会走错分支。 */
            static const uint8_t ff12[12] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                             0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            if (id) return os_mbuf_append(ctxt->om, id->token, 12);
            return os_mbuf_append(ctxt->om, ff12, sizeof(ff12));
        }
        case 0x0002:
            return os_mbuf_append(ctxt->om, s_pid_le, sizeof(s_pid_le));
        case 0x0004:
            return os_mbuf_append(ctxt->om, s_fw_ver, sizeof(s_fw_ver) - 1);
        case 0x0013: {
            /* 绑定时 App 写过 20B Device ID，之后读回校验；回错当新设备处理 */
            if (id) {
                size_t n = sizeof(id->did);
                while (n && id->did[n - 1] == 0x00) n--;
                if (n) return os_mbuf_append(ctxt->om, id->did, (uint16_t)n);
            }
            static const uint8_t zero20[20] = { 0 };
            return os_mbuf_append(ctxt->om, zero20, sizeof(zero20));
        }
        case 0x0014:
            /* Beacon Key：App 读它解 0xFE95 广播帧 */
            if (id) return os_mbuf_append(ctxt->om, id->bind_key, sizeof(id->bind_key));
            {
                static const uint8_t z16[16] = { 0 };
                return os_mbuf_append(ctxt->om, z16, sizeof(z16));
            }
        case 0x0016:
        case 0x0017:
        case 0x0018: {
            /* 真机可读，值无实际语义；给 1 字节 0 比空值更安全 */
            static const uint8_t z = 0;
            return os_mbuf_append(ctxt->om, &z, 1);
        }
        case 0x001B: {
            static const uint8_t spec_ver = 0x02;
            return os_mbuf_append(ctxt->om, &spec_ver, 1);
        }
        default:
            return 0;
        }
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* 其它 op（prepare/execute write 等）直接放过 */
        return 0;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (u) {
    case 0x0013:
        /* App 绑定后写 20B Device ID，读回要用同一份 */
        if (got >= 4 && got <= sizeof(s_devid)) {
            memcpy(s_devid, buf, got);
            s_devid_len = got;
            ESP_LOGI(TAG, "wr 0013 devid %uB", got);
        }
        break;
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
        s_rx_chan = s_h19;
        ESP_LOGD(TAG, "wr 0019 #%lu len=%u", (unsigned long)s_wr19, got);
        if (s_auth) miot_auth_on_0019(s_auth, buf, got);
        break;
    case 0x001A:
        s_wr1a++;
        s_rx_chan = s_h1a;
        ESP_LOGI(TAG, "wr 001A #%lu len=%u cmd=%02X", (unsigned long)s_wr1a, got, got ? buf[0] : 0);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, got > 32 ? 32 : got, ESP_LOG_INFO);
        /* 登录前：ag2 认证 parcel 走认证层；登录后：spec 的 START/ACK/DATA
         * 全归 spec 层，由它自己回 ACK（ACK 现在会回到 001A）。 */
        if (s_auth && !miot_auth_is_logged_in(s_auth)) {
            miot_auth_on_0019(s_auth, buf, got);
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x001B:
        s_wr1b++;
        s_rx_chan = s_h1b;
        ESP_LOGI(TAG, "wr 001B #%lu len=%u cmd=%02X", (unsigned long)s_wr1b, got, got ? buf[0] : 0);
        if (s_auth && !miot_auth_is_logged_in(s_auth)) {
            miot_auth_on_0019(s_auth, buf, got);
        } else if (s_dev) {
            miot_device_on_rx(s_dev, buf, got);
        }
        break;
    case 0x0001:
        /* 老型号的 token 通道（12B）。标准 ECDH 型号不会走这里，
         * 但真机是「收下不报错」，回 ATT error 会让 App 判设备异常。 */
        ESP_LOGI(TAG, "wr 0001 len=%u (token, 忽略)", got);
        break;
    case 0x0016:
    case 0x0017:
    case 0x0018:
        ESP_LOGI(TAG, "wr %04X len=%u (探测桩，忽略)", u, got);
        break;
    default:
        /* ★不能回 ATT error：真机对这些特征一律「收下」，
         * 回错误会让 App 在服务发现阶段就判定设备不可用。 */
        ESP_LOGW(TAG, "wr %04X len=%u (未用特征，放过)", u, got);
        break;
    }
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        /* 0x0001 Token：真机存在（write+notify，无 READ）。标准 ECDH 型号不用它，
         * 但缺了这条 App 的服务发现结果与真机不一致 → 走错认证分支。 */
        .uuid = &s_c01_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h01,
    },
    {
        /* 0x0002 Product ID：真机只读，App 靠它确认型号 */
        .uuid = &s_c02_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h02,
    },
    {
        /* 0x0004 固件版本：LOGIN 后 App 必读，读失败会放弃初始化 */
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
        /* 0x0013 Device ID：可读可写（绑定后 App 写 20B 再读回校验） */
        .uuid = &s_c13_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_h13,
    },
    {
        /* 0x0014 Beacon Key：App 读它解广播帧 */
        .uuid = &s_c14_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
        .val_handle = &s_h14,
    },
    {
        /* 0x0016/0x0017/0x0018：真机存在（探测桩）。★2026-11-14 实测：
         * 缺这三条的版本，App 只订阅 0019+0010、零 spec GET，14s 后断开。 */
        .uuid = &s_c16_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h16,
    },
    {
        .uuid = &s_c17_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h17,
    },
    {
        .uuid = &s_c18_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h18,
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
        /* 0x001B：真机是 write+notify（**没有 READ**，之前我们多给了 READ、
         * 又漏了 NOTIFY，App 于是整条跳过不订阅） */
        .uuid = &s_c1b_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
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
    ESP_LOGI(TAG, "0xFE95 service registered (12 chrs: 0001/0002/0004/0010/0013/0014/"
                  "0016/0017/0018/0019/001A/001B — 与真机逐条一致)");
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
    s_rx_chan = 0;
    memset(s_sub, 0, sizeof(s_sub));   /* 订阅是按连接生效的，新连接重新记 */
    ESP_LOGI(TAG, "connected conn=%u mtu=%u", conn_handle, s_mtu);
}

void gatt_miot_on_disconnect(void)
{
    ESP_LOGI(TAG, "disconnected (wr 0010=%lu 0019=%lu 001A=%lu 001B=%lu)",
             (unsigned long)s_wr10, (unsigned long)s_wr19,
             (unsigned long)s_wr1a, (unsigned long)s_wr1b);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = BLE_ATT_MTU_DFLT;
    s_rx_chan = 0;
    memset(s_sub, 0, sizeof(s_sub));
}

void gatt_miot_dump_handles(void)
{
    /* value handle；CCCD 一般紧跟其后（+1/+2 视是否多个描述符而定）。
     * App 的 SUBSCRIBE 事件里给的是 CCCD handle，对照这里就能知道它订的是哪条通道。 */
    ESP_LOGI(TAG, "handles: 0001=%u 0002=%u 0004=%u 0010=%u 0013=%u 0014=%u "
                  "0016=%u 0017=%u 0018=%u 0019=%u 001A=%u 001B=%u",
             s_h01, s_h02, s_h04, s_h10, s_h13, s_h14,
             s_h16, s_h17, s_h18, s_h19, s_h1a, s_h1b);
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

uint16_t gatt_miot_mtu(void)
{
    return s_mtu;
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
    /* ★spec 的应答/事件必须回到「App 写入的那条特征」。
     * 实测（_rb10.log，multi_dev 成功那轮）：登录后 App 全部 ag2 帧都写在
     * 0x001A，设备若把 ACK 投到 0x0010，App 侧 ag2 状态机永远等不到确认，
     * 每 6s 重发 `00 00 00 00 01 00`，后续 13.1 sync 一条都不发。
     * 真机行为：数据面（START/SINGLE/数据帧）走 0x001A。 */
    uint16_t h = pick_chan(s_h10);
    int rc = notify_chr(h, d, len);
    if (rc != 0) ESP_LOGW(TAG, "spec notify %uB -> h%u rc=%d", (unsigned)len, h, rc);
    return rc;
}

int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    return notify_chr(s_h10, d, len);
}

int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len)
{
    (void)user;
    /* ★登录前一律投 0x0019：device info / devRand(0x0D) / devConfirm(0x0C) 这些
     * 认证 parcel 必须落在 App 写入的那条面上，而 App 的认证状态机只盯 0x0019
     * （docs/known-issues §2.1 表 1）。登录完成后的 ag2 才改看本帧来路面。 */
    uint16_t h = (s_auth && miot_auth_is_logged_in(s_auth)) ? pick_chan(s_h19) : s_h19;
    return notify_chr(h, d, len);
}
