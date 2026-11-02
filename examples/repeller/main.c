/**
 * @file main.c
 * @brief 驱蚊器智能版 WX08ZM 示例 —— 开关下行/回报 + 电量
 *
 * 详情：src/miot_profiles.c §6（0x040A 执行器档案）
 */
#include <string.h>
#include "../util.h"
#include "miot_ble/miot_profiles.h"

int main(void)
{
    printf("== repeller WX08ZM demo ==\n\n");

    miot_port_t port = { .send_notify = host_notify, .log = demo_log };
    port.user = "001A";

    miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    repeller_init(&dev, &port, &tbl, &cnt);

    uint8_t keys[40];
    for (int i = 0; i < 40; i++) keys[i] = (uint8_t)(0xE0 + i);
    miot_device_set_session_keys(&dev, keys);

    printf("-- simulate set_properties 2.1 power=1 --\n");
    uint8_t pt[] = {
        0x0B, 0x20,                      /* len16 = 0x2000|11 */
        0x34, 0x12,                      /* tid */
        0x00,                            /* op: set_properties */
        0x01,                            /* count = 1 */
        0x02, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01,  /* 2.1 bool = 1 */
    };
    miot_device_dispatch_plain(&dev, pt, sizeof(pt));

    /* set 后立刻回报（真机行为：页面开关才停在正确位置） */
    printf("-- report back properties_changed 2.1=1 --\n");
    miot_spec_value_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.siid = 2; pc.piid = 1; pc.type = MIOT_TYPE_BOOL; pc.val.u8 = 1;
    miot_device_send_props_changed(&dev, &pc, 1);
    return 0;
}
