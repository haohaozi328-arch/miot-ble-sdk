/**
 * @file main.c
 * @brief 无线开关「按键事件」广播示例 —— 单键 XMWXKG01CM(0x2387) / 双键 XMWXKG01YL(0x1949)
 *
 * 事件对象（实测：两台均用同一套，载荷 1B = 按键号）：
 *   0x4E0C 单击 / 0x4E0D 双击 / 0x4E0E 长按
 *   单键：载荷恒 1；双键：1=左 2=右
 *
 * 说明：按键事件只走广播（不进 GATT），米家靠网关收帧后触发自动化；
 *       设备可连接（用于绑定/配置），但绑定完成后的按键值不需要建连接。
 * 详情：include/miot_ble/miot_beacon.h
 */
#include "../util.h"
#include "miot_ble/miot_beacon.h"
#include <string.h>

static void emit(uint16_t pid, const uint8_t mac_air[6], uint16_t obj,
                 uint8_t key, uint32_t cnt, const char *label)
{
    miot_beacon_cfg_t cfg;
    cfg.pid = pid;
    memcpy(cfg.mac_air, mac_air, 6);
    for (int i = 0; i < 16; i++) cfg.beacon_key[i] = (uint8_t)(0xA0 + i);

    uint8_t objs[8], frame[32];
    size_t n = miot_beacon_obj_u8(objs, obj, key);
    int flen = miot_beacon_frame_v5(&cfg, cnt, objs, n, frame, sizeof(frame));
    show_frame(label, frame, flen);
}

int main(void)
{
    printf("== wireless switch key-event demo ==\n\n");

    /* 单键蓝牙版（PID 0x2387）：单击 */
    const uint8_t mac_btn[6] = { 0x85, 0x22, 0x11, 0xBC, 0xAB, 0x68 };
    emit(0x2387, mac_btn, MB_OBJ_BTN_CLICK, 1, 700, "-- single-key click   (0x2387)");

    /* 双键版（PID 0x1949）：左键双击 / 右键长按 */
    const uint8_t mac_sw[6] = { 0x7D, 0x22, 0x11, 0xBC, 0xAB, 0x68 };
    emit(0x1949, mac_sw, MB_OBJ_BTN_DOUBLE, 1, 701, "-- dual-key L double  (0x1949)");
    emit(0x1949, mac_sw, MB_OBJ_BTN_HOLD,   2, 702, "-- dual-key R hold    (0x1949)");
    return 0;
}
