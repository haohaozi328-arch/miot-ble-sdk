/**
 * @file main.c
 * @brief 夜灯2 MUE4094RT 示例 —— spec 下行（亮度/延时/开关）+ 电量
 *
 * 详情：src/miot_profiles.c §5（v54 固件完整打通的属性表）
 * 帧格式：[len16: 0x2000|len][tid16][op][payload]（len16 高 4 位 = 协议版本 2）
 */
#include "../util.h"
#include "miot_ble/miot_profiles.h"

int main(void)
{
    printf("== night light MUE4094RT demo ==\n\n");

    miot_port_t port = { .send_notify = host_notify, .log = demo_log };
    port.user = "001A";

    miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    night_light_init(&dev, &port, &tbl, &cnt);

    uint8_t keys[40];
    for (int i = 0; i < 40; i++) keys[i] = (uint8_t)(0xE0 + i);
    miot_device_set_session_keys(&dev, keys);

    /* 模拟 App 下发 set_properties：2.3 power=1、2.1 bright=77（真实平台
     * 在 GATT 写回调里把解密后的明文帧交给 miot_device_dispatch_plain） */
    printf("-- simulate set_properties 2.3=1 / 2.1=77 --\n");
    uint8_t pt[] = {
        0x12, 0x20,                      /* len16 = 0x2000|18 */
        0x34, 0x12,                      /* tid */
        0x00,                            /* op: set_properties */
        0x02,                            /* count = 2 */
        /* 条目 = [siid][piid16 LE][type16 LE 纯类型号][len 1B][value] */
        0x02, 0x03, 0x00, 0x01, 0x00, 0x01, 0x01,    /* 2.3 bool len1 = 1 */
        0x02, 0x01, 0x00, 0x02, 0x00, 0x01, 0x4D,    /* 2.1 u8  len1 = 77 */
    };
    miot_device_dispatch_plain(&dev, pt, sizeof(pt));
    return 0;
}
