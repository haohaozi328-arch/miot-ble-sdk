/* ir_bus 本地总线协议定义（Hub 与 Node 共用）
 *
 * 设计取舍：
 *   本地总线用 BLE GATT 而不是 ESP-NOW —— 因为手头只有一块 ESP32，
 *   需要一个「不用第二块板子就能验证」的对端：
 *     · PC 用 bleak（Python）
 *     · 手机用 nRF Connect
 *   两者都能当 GATT 中央，所以 Hub 和 Node 都做成 GATT 服务端（peripheral），
 *   各自单独烧录就能测。
 *
 *   生产环境如果要 Hub 主动推给 N 个节点，把 Hub 改成 central（扫描并连节点）
 *   或换回 ESP-NOW 即可，上层 payload 不用变。
 */
#ifndef BLE_BUS_H
#define BLE_BUS_H

#include <stdint.h>
#include "host/ble_hs.h"
#include "host/util/util.h"

/* 服务 1a2b0001-9e3d-4c5f-8b7a-0d1e2f3a4b5c
 * CMD   1a2b0002-...  写/通知：Hub → Node 下指令
 * STATE 1a2b0003-...  读/通知：Node → Hub 回状态
 * 128bit UUID 在 NimBLE 里按小端字节序初始化（末字节在前）。 */
#define BUS_SVC_UUID_INIT   BLE_UUID128_INIT(0x5c, 0x4b, 0x3a, 0x2f, 0x1e, 0x0d, 0x7a, 0x8b, \
                                             0x5f, 0x4c, 0x3d, 0x9e, 0x01, 0x00, 0x2b, 0x1a)
#define BUS_CMD_UUID_INIT   BLE_UUID128_INIT(0x5c, 0x4b, 0x3a, 0x2f, 0x1e, 0x0d, 0x7a, 0x8b, \
                                             0x5f, 0x4c, 0x3d, 0x9e, 0x02, 0x00, 0x2b, 0x1a)
#define BUS_STATE_UUID_INIT BLE_UUID128_INIT(0x5c, 0x4b, 0x3a, 0x2f, 0x1e, 0x0d, 0x7a, 0x8b, \
                                             0x5f, 0x4c, 0x3d, 0x9e, 0x03, 0x00, 0x2b, 0x1a)

/* 指令字：直接沿用索尼电视码表的 cmd，语义好记，也方便和红外日志对照。
 *
 * 为什么 ON/OFF 用「音量+/−」而不是「电源」：
 *   米家索尼虚拟遥控只有一颗电源键（toggle），实测"打开电视"和"关闭电视"
 *   发出来都是 0x15，光靠它分不出开还是关。而 VOL+ / VOL− 是两个独立键，
 *   正好当绝对指令用：0x12 一定是开，0x13 一定是关，不会漂移。
 *   0x15 保留成 TOGGLE，和真实电视那颗电源键的行为一致。 */
#define BUS_CMD_ON      0x12   /* VOL+    → 开（置高） */
#define BUS_CMD_OFF     0x13   /* VOL-    → 关（置低） */
#define BUS_CMD_OFF_ALT 0x2F   /* PWR_OFF → 关（兼容旧映射） */
#define BUS_CMD_TOGGLE  0x15   /* POWER   → 翻转（和真电视一致，仅调试） */

typedef struct __attribute__((packed)) {
    uint8_t cmd;   /* BUS_CMD_* 或任意红外 cmd */
    uint8_t arg;   /* 预留参数：亮度/档位等 */
    uint8_t seq;   /* 序号，用于对端去重 */
} bus_msg_t;

#endif /* BLE_BUS_H */
