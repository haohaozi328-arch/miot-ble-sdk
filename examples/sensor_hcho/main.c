/**
 * @file main.c
 * @brief 甲醛检测仪 JQJCY01YM 示例 —— MiBeacon v5 加密广播（不建连接）
 *
 * 广播对象：0x1010 甲醛(0.01mg/m³) / 0x1004 温度 / 0x1006 湿度 / 0x100A 电量，
 * 三路轮换分帧保证广播 ≤31B。
 * 详情：include/miot_ble/miot_beacon.h（对象表）+ docs/protocol.md §beacon
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"

int main(void)
{
    printf("== formaldehyde sensor JQJCY01YM demo ==\n\n");

    miot_beacon_cfg_t cfg = { .pid = 0x02DF, .mac_air = {0x51, 0x33, 0x5E, 0x2A, 0x11, 0xC8} };
    /* beaconkey 在绑定时生成/派生，掉电保存（真机流程见 miot_auth） */
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[16], frame[31];
    static uint32_t cnt = 100;

    /* 第 1 帧：甲醛 0.08 mg/m³ */
    size_t n = miot_beacon_obj_u16(objs, MB_OBJ_HCHO_U16, 8);
    int flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame#1 HCHO", frame, flen);

    /* 第 2 帧：温度 23.5℃ */
    n = miot_beacon_obj_i16(objs, MB_OBJ_TEMP_I16, 235);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame#2 temp", frame, flen);

    /* 第 3 帧：湿度 55.2% */
    n = miot_beacon_obj_u16(objs, MB_OBJ_HUM_U16, 552);
    flen = miot_beacon_frame_v5(&cfg, ++cnt, objs, n, frame, sizeof(frame));
    show_frame("-- frame#3 hum", frame, flen);

    printf("\n真实平台：把 frame 放进厂商自定义广播（公司码 0x038F）循环播出，\n"
           "frame_cnt 每帧递增且掉电不回跳（防重放）。\n");
    return 0;
}
