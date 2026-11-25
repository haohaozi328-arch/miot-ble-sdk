#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** relay 目标真灯 MAC。置 "" 关闭 relay（普通伪装模式）。 */
#ifndef RELAY_TARGET_MAC
#define RELAY_TARGET_MAC "14:D8:81:E6:95:3C"
#endif

/** 启动 relay：连接 mac 指定的真灯，并把手机流量原样转发给它。 */
void relay_start(const char *mac);

/** relay 是否已启用（启用后 0010/0016/001A/001B/001C 的写事件都被接管） */
int relay_active(void);

/** 手机写进来的数据 → 真灯。返回 1 表示已接管（原固件逻辑不要再处理）。 */
int relay_forward(uint16_t uuid16, const uint8_t *data, size_t len);

/** 手机（App）连接状态变化。真灯单条连接寿命只有 ~19s，手机一连上就要
 *  把旧链路重建，把整段窗口让给配网流程。 */
void relay_on_phone(int connected);

/** 广播启动前调用：给 central 角色准备一个随机地址（relay 的 public 地址
 *  被强制成真灯 MAC，不能拿同一个地址去连同地址的对端）。 */
void relay_prepare(void);

#ifdef __cplusplus
}
#endif
