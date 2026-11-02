/**
 * @file main.c
 * @brief 水浸传感器 SJWS01LM 示例 —— MiBeacon v5 加密广播
 *
 * 广播对象：0x1014 漏水状态(0=正常 >0=漏水) / 0x100A 电量。真机已验证 ✅
 * 详情：include/miot_ble/miot_beacon.h
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"

int main(void)
{
    printf("== water leak sensor SJWS01LM demo ==\n\n");

    miot_beacon_cfg_t cfg = { .pid = 0x0863, .mac_air = {0x73, 0x33, 0x5E, 0x2A, 0x11, 0xC8} };
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[16], frame[31];
    static uint32_t cnt = 400;

    /* 漏水报警 */
    size_t n = miot_beacon_obj_u8(objs, MB_OBJ_LEAK, 1);
    n += miot_beacon_obj_u8(objs + n, MB_OBJ_BATTERY, 90);
    int flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame LEAK + battery", frame, flen);

    /* 恢复正常 */
    n = miot_beacon_obj_u8(objs, MB_OBJ_LEAK, 0);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame dry", frame, flen);
    return 0;
}
