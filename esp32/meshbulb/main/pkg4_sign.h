/* Mesh 配网 pkg4 设备签名（2026-11-20）
 *
 * 背景：App 收齐 pkg7/3/8 后等设备回 pkg4（64B），原样塞进
 * `/v2/blemesh/bind` 的 `sign` 字段由**服务器**验签（App 不验）。
 * 参照 PSK 流程的同类结论（docs/psk-auth.md §5）：
 *   「云端拿 dev_pub 验签，App 只做 Base64 上报」
 * 本轮实测也证实服务器不把 dev_mesh_pub 与证书绑定（真证书 + 我们的
 * 公钥照样过了 /v2/blemesh/auth），所以只要签名内容对我们就能过 bind。
 *
 * 未知项只剩「签什么明文」。这里给出候选表，运行时按会话轮换（见
 * gatt_miot.c 的 s_pkg4_variant），一次实测就能把正确项钉死。
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* 用 g_eph_priv 对 SHA-256(msg) 做 ECDSA-P256，输出 64B r||s（大端定长）。
 * 返回 0 成功。 */
int mesh_sign_sha256(const uint8_t *msg, size_t len, uint8_t out64[64]);

/* 开机自检：签名后用 g_eph_pub 验签，返回 0 表示签名链路正确。 */
int mesh_sign_selftest(void);

/* ECDH：用 g_eph_priv 与对方 64B 公钥（X||Y，无 04 前缀）算共享密钥，
 * 输出 32B X 坐标（大端），与 Java SecretKey.getEncoded() 一致。
 * 返回 0 成功。 */
int mesh_ecdh_shared(const uint8_t peer_pub64[64], uint8_t out32[32]);

/* 裸 SHA-256（给候选明文用）。 */
void mesh_sha256(const uint8_t *msg, size_t len, uint8_t out32[32]);

/* 直接对 32B 摘要签名（不再做 SHA-256 预处理）。 */
int mesh_sign_digest(const uint8_t digest[32], uint8_t out64[64]);
