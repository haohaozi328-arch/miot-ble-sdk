/**
 * @file main.c
 * @brief 人体传感器2 RTCGQ02LM 示例 —— MiBeacon v5 加密广播
 *
 * 广播对象：0x000F 移动+光照(3B) / 0x1017 距上次移动秒数 / 0x1001 按键 / 0x100A 电量
 * 详情：include/miot_ble/miot_beacon.h
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"

int main(void)
{
    printf("== motion sensor RTCGQ02LM demo ==\n\n");

    miot_beacon_cfg_t cfg = { .pid = 0x0A8D, .mac_air = {0x59, 0x33, 0x5E, 0x2A, 0x11, 0xC8} };
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[16], frame[31];
    static uint32_t cnt = 300;

    /* 人体移动事件：0x000F = 256（移动 + 有光）；byte1=1 有人 byte2=1 光照>0 */
    uint8_t ml[3] = { 0x01, 0x01, 0x00 };
    size_t n = miot_beacon_obj_bytes(objs, MB_OBJ_MOTION_LIGHT, ml, 2);
    int flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame MOTION + light", frame, flen);

    /* 无人静默期心跳：0x1017 = 120s 无人 */
    n = miot_beacon_obj_u32(objs, MB_OBJ_NO_MOTION, 120);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame no-motion 120s", frame, flen);
    return 0;
}
