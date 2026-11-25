/**
 * @file print_channel.h
 * @brief 米家标签打印机私有打印通道（0x001F 写 / 0x0020 notify）
 *
 * 协议来源（公开逆向工程，实测可用）：
 *   github.com/jasiek/xiaomi-mjbqdyj1-wc-reveng      —— 协议笔记 + Python/TS 实现
 *   github.com/ArtrixTech/mijia-label-printer        —— Bleak 实机验证（打印成功）
 *
 * 数据面：service 0xFE95 之下
 *   - 0x001F Write Without Response   ← 主机发命令（A3 帧切片）
 *   - 0x0020 Notify                   → 设备回 ACK / 状态 / 心跳
 * 帧格式：
 *   A3 20 <密文长度 LE16> <AES-128-CBC 密文> <CRC32 LE>   （主机→设备，加密）
 *   A3 00 <明文长度 LE16> <明文>            <CRC32 LE>   （设备→主机，明文心跳）
 *   - KEY  99B829436CDD5647AADB8816F73E8644
 *   - IV   0001020F3CF899ABABCD25318DF446B1
 *   - 零填充到 16 字节倍数（非 PKCS#7）
 *   - CRC32 = zlib.crc32(buf, 0x76953521)，追加小端 4 字节
 * 内层命令：11 <grp> <cmd> <len LE16> <body>；响应 marker 21，自发通知 10。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/** 0x001F 收到数据（BLE 写切片，内部自行组帧）。在 NimBLE 回调线程调用。 */
void print_channel_on_rx(const uint8_t *data, size_t len);

/** 复位会话状态（连接建立/断开时调用）。 */
void print_channel_reset(void);

/** 调试用：已完整解出的命令帧计数。 */
uint32_t print_channel_cmd_count(void);
