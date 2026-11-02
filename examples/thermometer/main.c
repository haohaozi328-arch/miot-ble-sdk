/**
 * @file main.c
 * @brief 温湿度计示例（cgdk3 / miaomiaoce-t9 双 spec 兼容）
 *
 * 详情：src/miot_profiles.c §2（属性表 + 双 siid 兼容说明）
 */
#include "../util.h"
#include "miot_ble/miot_profiles.h"

int main(void)
{
    printf("== thermometer demo (cgdk3 / miaomiaoce-t9) ==\n\n");

    miot_port_t port = { .send_notify = host_notify, .log = demo_log };
    port.user = "001A";

    miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    thermometer_init(&dev, &port, &tbl, &cnt);

    uint8_t keys[40];
    for (int i = 0; i < 40; i++) keys[i] = (uint8_t)(0xE0 + i);
    miot_device_set_session_keys(&dev, keys);

    printf("-- report 25.6C / 58%% --\n");
    thermometer_report(&dev, 25.6f, 58.0f, 0);
    return 0;
}
