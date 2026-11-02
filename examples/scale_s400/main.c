/**
 * @file main.c
 * @brief S400 体脂秤模拟器完整示例 —— 认证 + spec 通道 + 测量上报全链路（host 演示）
 *
 * 真实平台（如 ESP32+NimBLE）的差异只在：
 *   1. 用你的 GATT 栈实现 notify_0010 / notify_0019 / send_notify
 *   2. 在 GATT 写回调里转发到 miot_auth_on_0010/0019 与 miot_device_on_rx
 *   3. 注册加密后端 miot_crypto_set_ops（参考 port/esp32）
 */
#include <stdio.h>
#include <stdarg.h>
#include "miot_ble/miot_profiles.h"

/* ---------- host 移植层：notify 打印到 stdout ---------- */

static int host_notify(void *user, const uint8_t *d, size_t n)
{
    const char *ch = (const char *)user;
    printf("  >> notify[%s] %uB:", ch, (unsigned)n);
    for (size_t i = 0; i < n; i++) printf(" %02x", d[i]);
    printf("\n");
    return 0;
}

static void demo_log(void *user, int lv, const char *tag, const char *fmt, ...)
{
    (void)user; (void)lv;
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---------- 持久化演示（内存版） ---------- */

static void id_save(void *user, const miot_auth_identity_t *id)
{
    (void)user;
    printf("  [persist] save identity (did=%02x.. token=%02x%02x..)\n",
           id->did[0], id->token[0], id->token[1]);
    /* 真实平台写入 NVS / flash */
}

int main(void)
{
    printf("== S400 scale full demo (auth + spec) ==\n\n");

    miot_port_t port = { .send_notify = host_notify, .log = demo_log };
    port.user = "001A";

    /* 1) 设备层：属性表 + spec 分派 */
    miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    scale_s400_init(&dev, &port, &tbl, &cnt);

    /* 2) 认证层：REGISTER + LOGIN 状态机 */
    static miot_auth_t auth;
    profiles_wire_auth(&auth, &dev, host_notify, host_notify, "0019");
    /* host 无加密后端时 prepare 会失败——真实平台先 miot_crypto_set_ops()
     * （参考 port/esp32/miot_port_esp32.c）。此处演示接口接线。 */
    miot_auth_prepare(&auth);
    (void)id_save;

    /* 3) 模拟：认证已完成，注入会话密钥（真实场景 LOGIN 成功后自动获得） */
    uint8_t keys[40];
    for (int i = 0; i < 40; i++) keys[i] = (uint8_t)(0xE0 + i);
    miot_device_set_session_keys(&dev, keys);

    /* 4) 模拟一次测量上报（64.5kg / 阻抗 50.0Ω） */
    printf("\n-- report measurement --\n");
    scale_s400_report(&dev, 64500, 500, 1795000000u);
    return 0;
}
