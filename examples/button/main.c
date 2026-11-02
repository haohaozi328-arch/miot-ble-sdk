/**
 * @file main.c
 * @brief 无线按钮 XMWXKG01LM 示例 —— 按钮配置属性读写（3.1020）
 *
 * 详情：src/miot_profiles.c §4
 */
#include "../util.h"
#include "miot_ble/miot_profiles.h"

int main(void)
{
    printf("== wireless button XMWXKG01LM demo ==\n\n");

    miot_port_t port = { .send_notify = host_notify, .log = demo_log };
    port.user = "001A";

    miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    button_dev_init(&dev, &port, &tbl, &cnt);

    uint8_t keys[40];
    for (int i = 0; i < 40; i++) keys[i] = (uint8_t)(0xE0 + i);
    miot_device_set_session_keys(&dev, keys);

    /* 模拟插件下发 get_properties 3.1020（App 配置按钮时回读）
     * GET 条目 = [siid][piid16 LE] */
    printf("-- simulate get_properties 3.1020 --\n");
    uint8_t pt[] = {
        0x07, 0x20,              /* len16 = 0x2000|7 */
        0x35, 0x12,              /* tid */
        0x02,                    /* op: get_properties */
        0x01,                    /* count = 1 */
        0x03, 0xFC, 0x03,        /* siid=3, piid=1020 (0x03FC LE) */
    };
    miot_device_dispatch_plain(&dev, pt, sizeof(pt));
    return 0;
}
