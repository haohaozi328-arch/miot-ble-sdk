/* pkg4 —— 设备签名（BleMeshRegisterConnector recvDeviceSign）
 *
 * ★2026-11-20 反编译结论（OooO0o.java = BleMeshRegisterConnector）：
 *   App 收到 pkg4 后【不做本地验签】，原样放进 /v2/blemesh/bind 的 "sign"
 *   字段发给云端，由服务器对照 pkg1 里那张设备证书的公钥验签。
 *
 * 签名内容（覆盖哪些字节）目前未知 —— 需要抓一次真机正常配网的 btsnoop，
 * 拿到真机 pkg4 后离线枚举候选消息验签（用真机证书公钥）。
 *
 *   - 若签名只覆盖设备侧静态数据（dev_info / 证书 / 自身公钥）→ 原样重放即可过 bind；
 *   - 若覆盖 pkg8（服务器挑战）→ 每次会话都不同，必须拿到签名私钥（产线钥），
 *     纯重放走不通，只能另寻他路（hook / 官方模组）。
 *
 * 当前为 64B 全零占位：先打通「pkg8 → 发 pkg4 → App 走 bind」的链路，
 * 看 bind 接口回什么错误码，反推服务器到底验什么。
 */
static const uint8_t g_pkg4[64] = { 0 };
#define g_pkg4_len (sizeof(g_pkg4))
