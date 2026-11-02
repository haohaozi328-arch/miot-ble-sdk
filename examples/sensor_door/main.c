/**
 * @file main.c
 * @brief 门窗传感器2 MCCGQ02HL 示例 —— MiBeacon v5 加密广播
 *
 * 广播对象：0x1019 开合(0=开 1=关 2=超时未关 3=复位) / 0x1018 光照 / 0x100A 电量
 * 详情：include/miot_ble/miot_beacon.h
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"

int main(void)
{
    printf("== door sensor MCCGQ02HL demo ==\n\n");

    miot_beacon_cfg_t cfg = { .pid = 0x098B, .mac_air = {0x6D, 0x33, 0x5E, 0x2A, 0x11, 0xC8} };
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[16], frame[31];
    static uint32_t cnt = 200;

    /* 开合事件：门被打开（0=开） */
    size_t n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR, 0);
    n += miot_beacon_obj_u8(objs + n, MB_OBJ_LIGHT, 1);       /* 光照：亮 */
    int flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame door OPEN + light", frame, flen);

    /* 关门事件（1=关） */
    n = miot_beacon_obj_u8(objs, MB_OBJ_DOOR, 1);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame door CLOSE", frame, flen);
    return 0;
}
