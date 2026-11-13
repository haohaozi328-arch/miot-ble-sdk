/**
 * @file gatt_miot.h
 * @brief 米家 Spec v2 的 GATT 承载层（0xFE95 服务）
 *
 * 认证与 spec 两个状态机都由 SDK 提供，这一层只做三件事：
 *   1. 注册 4 个特征值（0x0010 / 0x0019 / 0x001A / 0x001B）
 *   2. 把 GATT 写事件分发给对应状态机
 *   3. 提供 notify 出口（按通道选特征值句柄）
 *
 * 通道分工（docs/protocol.md §1）：
 *   0x0010  App→设备：0xA4 建链 / 0xA2 注册 / 0x24 登录；设备→App：结果码 0x11/0x21
 *   0x0019  认证数据，ag2 分帧（公钥、devRandom、devConfirm、appConfirm）
 *   0x001A  spec 数据第二通道（App 只订阅 0x0010/0x0019，往这里推页面收不到；
 *           实测保留仅供观察，真正上报走 0x0010）
 *   0x001B  App→设备第二写通道（米家部分版本用它补发）
 */
#ifndef GATT_MIOT_H
#define GATT_MIOT_H

#include <stdint.h>
#include <stddef.h>
#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册服务配置。必须在 nimble_port_init() 之后、host 同步前调用。 */
void gatt_miot_register_svcs(void);

/** 绑定状态机实例。之后所有写事件都投给它们。 */
void gatt_miot_bind(miot_auth_t *auth, miot_device_t *dev);

/** 注入当前型号的 PID —— 0x0002 只读特征值要按型号回真实值 */
void gatt_miot_set_pid(uint16_t pid);

/** 注入空中序 MAC —— legacy RC4 认证（夜灯2 等）的 mixA/mixB 要用 */
void gatt_miot_set_mac(const uint8_t mac_air[6]);
/** 同一 MAC 的人读序（mixA/mixB 正态用的是这一序） */
void gatt_miot_set_mac_human(const uint8_t mac_human[6]);

void gatt_miot_on_connect(uint16_t conn_handle, uint16_t mtu);
void gatt_miot_on_disconnect(void);
void gatt_miot_on_mtu(uint16_t mtu);
void gatt_miot_on_subscribe(uint16_t handle, int notify, int indicate);
int  gatt_miot_notify_token(const uint8_t *d, size_t len);
int  gatt_miot_notify_broadcast(const uint8_t *d, size_t len);

/** Nordic UART（6E400001）TX 通道回包 —— 飞利浦系插件（philips.light.nlight）
 *  的会话数据走这里，串口 `nus <hex>` 可直接发一帧做实验。 */
int  gatt_miot_nus_notify(const uint8_t *d, size_t len);

/** nlight 文本协议（Nordic UART）：本机模拟一条 App 命令 / 打印当前状态。
 *  串口 `nl <text>`、`nlstat`。用于没有手机时自测收发。 */
void gatt_miot_nl_input(const char *cmd);
void gatt_miot_nl_dump(void);

/** GETRAND 应答首帧延时（ms，默认 40）。无 hook 时靠它抢在 App 断连之前送达。
 *  串口 `grd <ms>` 可在不重烧的情况下扫描最优值。 */
void gatt_miot_set_gr_delay(int ms);
int  gatt_miot_get_gr_delay(void);

/** 打印四个特征的实际 attribute handle（排查 App 订阅到哪个通道用） */
void gatt_miot_dump_handles(void);
/** 倒出 RAM 里的事件环形缓冲（串口 `log`），用于事后定位 App 走到哪一步 */
void gatt_miot_dump_evt(void);

/** 打印当前认证身份：s_ltok / NVS token / bind_key / 最近 rand（串口 `tok`） */
void gatt_miot_dump_token(void);

/** 当前是否有手机连着（决定要不要继续广播） */
int gatt_miot_is_connected(void);

/** 主会话连接句柄（BLE_HS_CONN_HANDLE_NONE=无），多连接旁听时分辨断的是哪条 */
uint16_t gatt_miot_conn_handle(void);

/** 最后一次收到手机写入的时刻（esp_timer ms），看门狗用 */
uint32_t gatt_miot_rx_ms(void);

/** 设备侧主动断开（会话收尾实验用，见 gatt_miot.c 实现） */
void gatt_miot_disconnect(void);

/** spec 通道 notify 出口，直接塞给 miot_port_t.send_notify */
int gatt_miot_notify_spec(void *user, const uint8_t *d, size_t len);

/** 认证通道 notify 出口，塞给 miot_auth_port_t.notify_0010 / notify_0019 */
int gatt_miot_notify_auth_0010(void *user, const uint8_t *d, size_t len);
int gatt_miot_notify_auth_0019(void *user, const uint8_t *d, size_t len);

/** legacy GETRAND 应答帧数：0=静默（判定用），1=单帧，18=多假设（默认）。
 *  串口 `gr N` 可运行时切换，不必重新烧录。 */
void gatt_miot_set_gr_reply(int n);
void gatt_miot_set_strat(int n);
int  gatt_miot_get_strat(void);
void gatt_miot_set_spec_probe(int on);
int  gatt_miot_get_spec_probe(void);
int  gatt_miot_get_gr_reply(void);

#ifdef __cplusplus
}
#endif
#endif /* GATT_MIOT_H */
