/**
 * @file main.c
 * @brief 温湿度计3 mini MJWSD06MMC 示例 —— MiBeacon v5 加密广播（float 对象批次）
 *
 * 广播对象：0x4C01 温度 float32℃ / 0x4C02 湿度 uint8% / 0x4803 电量 uint8%
 * （国区 pdid 21941 = 0x55B5；0x5BEA 是海外版 23530，国区 App 扫不到）
 * 对象 ID 依据 XiaomiGateway3 devices.py 21941：mi=19457/19458/18435。
 * 详情：include/miot_ble/miot_beacon.h
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"

int main(void)
{
    printf("== thermo-hygrometer 3 mini MJWSD06MMC demo ==\n\n");

    miot_beacon_cfg_t cfg = { .pid = 0x55B5, .mac_air = {0x45, 0x33, 0x5E, 0x2A, 0x11, 0xC8} };
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[16], frame[31];
    static uint32_t cnt = 500;

    /* 温度 25.2℃（float32） */
    size_t n = miot_beacon_obj_f32(objs, MB_OBJ_TEMP_F32, 25.2f);
    int flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame temp 25.2C", frame, flen);

    /* 湿度 39% */
    n = miot_beacon_obj_u8(objs, MB_OBJ_HUM_U8, 39);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame hum 39%", frame, flen);
    return 0;
}
