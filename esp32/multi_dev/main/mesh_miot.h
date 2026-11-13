/**
 * @file mesh_miot.h
 * @brief 米家 BLE Mesh 承载层（SIG Mesh + Mi Vendor Model 0x038f0000）
 *
 * 背景（docs/mesh-ref/）：
 *  - 云端把本产品注册为「蓝牙 Mesh」设备（yunmai.scales.ms103），
 *    Mesh 网关（小爱音箱等）对 Mesh 设备**只扫描广播、从不发起 GATT 连接**，
 *    spec 消息由网关转成 Mesh 消息经广播下发，上行同理走 Mesh 广播。
 *  - 入网走「小米私有 Provision」，载体仍是 SIG Mesh 的未配网 beacon
 *    （PB-ADV AD type 0x2B）与 PB-GATT（0x1827）；网关和米家 App 都能配网。
 *  - 应用层：MIOT Spec Server (vendor model 0x038f0000, CID 0x038F)，
 *    自定义 Service 经它透传 siid/piid/value。
 *
 * 本模块职责：
 *  1. 挂载 NimBLE Mesh 协议栈（cfg_srv + health_srv + MIOT Spec vendor model）
 *  2. 未配网时发出未配网 beacon（Device UUID 按米家候选格式，可串口切换）
 *  3. 一切配网/vendor PDU 事件打串口日志 —— 抓包窗口
 */
#ifndef MESH_MIOT_H
#define MESH_MIOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * host 启动前调用一次（gatt_miot_register_svcs 之后、host 同步之前）。
 * 关键：bt_mesh_init 会注册 Mesh Proxy/Prov GATT 服务，必须赶在
 * ble_gatts_start()（host 启动时建表）之前，否则服务落不到表里，
 * pb_gatt_srv.c 的 resolve_svc_handles 断言崩溃（实测复现过）。
 * pid=产品 ID，mac_human=6 字节 MAC（人读序，MSB 在前）。
 */
void mesh_miot_early_init(uint16_t pid, const uint8_t mac_human[6]);

/** host 同步后调用：未配网则开启配网承载（未配网 beacon + 0x1827 广播）。 */
void mesh_miot_on_sync(void);

/** true = Mesh 协议栈正在占用广播实例，app 不得再 ble_gap_adv_start（会 busy）。 */
bool mesh_miot_adv_owns_radio(void);

/** 手动开关配网承载（PB-ADV + PB-GATT）。1=发未配网 beacon，0=停。 */
void mesh_miot_set_prov(bool on);

/** 直接改写 16 字节 Device UUID（下一帧 beacon 生效）。 */
void mesh_miot_set_uuid(const uint8_t u[16]);

/** 状态串（配网与否 / unicast 地址 / prov 开关 / UUID 十六进制）。 */
void mesh_miot_status(char *out, size_t cap);

bool mesh_miot_provisioned(void);

#endif /* MESH_MIOT_H */
