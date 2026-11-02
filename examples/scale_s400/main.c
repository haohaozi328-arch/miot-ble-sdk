/**
 * @file main.c
 * @brief scale_s400 示例入口 —— host 可跑的演示（notify 打印到 stdout）
 */
#include <stdio.h>
#include <stdarg.h>
#include "miot_ble/miot_device.h"

extern miot_device_t *scale_device_create(miot_port_t *port, const uint8_t keys[40]);
extern void scale_report_measurement(miot_device_t *dev, uint32_t weight_01g,
                                     uint32_t impedance_01, uint32_t ts);

static int demo_notify(void *user, const uint8_t *d, size_t n)
{
    (void)user;
    printf("[notify %uB]:", (unsigned)n);
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

int main(void)
{
    printf("== scale_s400 SDK demo ==\n");
    printf("NOTE: host demo 未注册加密后端，帧加密返回错误（一次性提示）。\n"
           "      真实平台请先调用 miot_crypto_set_ops()（参考 port/esp32）。\n\n");
    miot_port_t port = { .send_notify = demo_notify, .log = demo_log };

    /* 假设 40B 会话密钥（真实场景来自标准认证 LOGIN 派生） */
    uint8_t keys[40] = {
        0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF, /* E  */
        0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF, /* D  */
        0x11,0x22,0x33,0x44,                                                             /* ivRx */
        0x55,0x66,0x77,0x88,                                                             /* ivTx */
    };

    miot_device_t *dev = scale_device_create(&port, keys);

    /* 模拟一次 64.5kg / 阻抗 50.0Ω 的测量上报 */
    scale_report_measurement(dev, 64500 /* 0.01kg */, 500 /* 0.1Ω */, 1795000000u);
    return 0;
}
