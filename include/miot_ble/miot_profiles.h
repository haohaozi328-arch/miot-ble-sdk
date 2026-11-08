/**
 * @file miot_profiles.h
 * @brief 已验证设备档案 —— 经米家 App 实机互通验证的属性表与上报接口
 *
 * 每个档案 = 一张属性表（miot_prop_t[]）+ 初始化/上报函数。
 * 全部只描述「米家连接与通信」，不包含传感器驱动（实际操作由你的硬件层实现，
 * 通过 on_set 回调或直接改状态变量接入）。
 */
#ifndef MIOT_PROFILES_H
#define MIOT_PROFILES_H

#include "miot_ble/miot_device.h"
#include "miot_ble/miot_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 1. 体脂秤 S400（cgllc.scale.s400 同构） ============
 * 验证状态：绑定/用户资料/spec 读写/事件上报全流程 ✓
 * （事件被 App 解析并广播到插件；插件出数需匹配其订阅 topic，进行中） */
void scale_s400_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);
/** 完整测量上报序列：过程流(8.1×4) → 结果(8.2) → 11.1022 → 属性变更(11.2/11.3)
 *  host/示例用：一坨全发。真机上必须**按步分帧、帧间留空**，
 *  因为 App 的 ag2 接收窗口同一毫秒只吃第一帧（docs/testing.md 关键要点 2）。 */
void scale_s400_report(miot_device_t *dev, uint32_t weight_01g,
                       uint32_t impedance_01, uint32_t ts);
/** 分步版：step = 0..SCALE_S400_STEPS-1，每步只发一帧，由调用方按 gap 表节流 */
#define SCALE_S400_STEPS 7
int  scale_s400_report_step(miot_device_t *dev, int step,
                            uint32_t weight_01g, uint32_t impedance_01, uint32_t ts);
/** siid 扫射（私有 spec 事件 siid 未知时探查） */
void scale_s400_sweep(miot_device_t *dev, const char *s1, const char *s2);

/* ============ 2. 温湿度计（cgllc-cgdk3 / miaomiaoce-t9 双 spec 兼容） ============
 * 验证状态：绑定 + spec 读写 ✓（温度 float 2.1001 / 3.1001，湿度、电量） */
void thermometer_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt);
/** 周期上报温湿度（properties_changed 双 siid 兼容） */
void thermometer_report(miot_device_t *dev, float temp_c, float hum_pct,
                        uint32_t ts);

/* ============ 3. 开关型设备（夜灯 / 驱蚊器 0x040A） ============
 * 验证状态：绑定 + spec 开关下发/回报 ✓（set 后立刻回报 0x1012，
 * 页面开关才停在正确位置） */
void switch_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);

/* ============ 5. 夜灯2（MUE4094RT） ============
 * 验证状态：v54 固件完整打通 ✓（spec 下行：亮度/延时/开关 + 电量回报） */
void night_light_init(miot_device_t *dev, miot_port_t *port,
                      miot_prop_t **tbl, size_t *cnt);

/* ============ 6. 驱蚊器智能版（WX08ZM，PID 0x040A） ============
 * 验证状态：绑定 + 认证 + spec 上报 ✓（zimi.mosq.v1：2.1 药量 / 3.1 电量 / 4.1 开关） */
void repeller_init(miot_device_t *dev, miot_port_t *port,
                   miot_prop_t **tbl, size_t *cnt);
/** 上报 2.1 药量 + 3.1 电量 + 4.1 开关，一帧三条。
 *  ⚠️ PID 0x040A 对应 zimi.mosq.v1，不是 v2：开关在 4.1(bool)，药量在 2.1(uint8)，
 *  且 v1 没有 mode 属性。报错 siid/piid 或类型会让 App 卡在「设备初始化」。 */
void repeller_report(miot_device_t *dev, uint8_t power, uint8_t battery,
                     uint8_t margin, uint32_t ts);
/** 设备侧当前开关状态（App 下发 set 后立即更新），上报前先读它做镜像 */
uint8_t repeller_power(void);

/* ============ 4. 无线按钮（XMWXKG01LM） ============
 * 验证状态：绑定 + 按钮配置属性读写(3.1020) ✓ */
void button_dev_init(miot_device_t *dev, miot_port_t *port,
                     miot_prop_t **tbl, size_t *cnt);

/* ============ 通用：绑定/认证与设备层的标准接线 ============
 * 返回 port 初始化好的 auth 实例（内部已接 notify 回调框架）。
 * 移植方只需提供 0x0010/0x0019 的 notify 实现。 */
void profiles_wire_auth(miot_auth_t *auth, miot_device_t *dev,
                        int (*notify_0010)(void *, const uint8_t *, size_t),
                        int (*notify_0019)(void *, const uint8_t *, size_t),
                        void *user);

#ifdef __cplusplus
}
#endif
#endif /* MIOT_PROFILES_H */
