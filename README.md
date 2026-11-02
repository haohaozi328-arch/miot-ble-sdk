# miot-ble-sdk

> ⚠️ **本项目仅供学习与研究，禁止商用**（详见 [LICENSE](LICENSE)）。
> 协议格式来自对米家 App 与 BLE 通信的独立逆向研究，仅作互操作性技术记录。

米家（MIoT）BLE Spec v2 设备端 SDK —— 从实测互通验证的 S400 体脂秤模拟固件中抽象而来。

核心价值：**所有协议格式均通过米家 App 反汇编（com.miot.spec 包 ud5/gyb/kt4 解析器）+ 空中抓包互证实测验证**，包括三个公开资料从未记载、决定 App 能否解析事件的关键帧格式细节（见 [docs/protocol.md](docs/protocol.md)）。

```
┌──────────────────────────────────────────────────────────┐
│  你的产品逻辑（体重计 / 温湿度计 / 灯 / 任意 MIoT 设备）      │
├──────────────────────────────────────────────────────────┤
│  miot_profiles 已验证设备档案（S400 秤/温湿度计/开关/按钮）    │
├──────────────────────────────────────────────────────────┤
│  miot_device   组合层：加密通道+分派+属性表+主动上报           │
│  ├─ miot_spec   Spec v2 明文帧编解码（纯函数，可host测试）     │
│  ├─ miot_props  属性表驱动执行器（get/set 自动处理）           │
│  ├─ miot_ag2    ag2 可靠传输层（START/ACK/DATA 分帧）         │
│  └─ miot_crypto 加密抽象（RC4/HKDF/ECDH/AES-CCM，后端可替换）  │
├──────────────────────────────────────────────────────────┤
│  miot_auth     标准认证（REGISTER 绑定 + LOGIN 会话密钥）      │
├──────────────────────────────────────────────────────────┤
│  miot_port  移植层接口（notify / 时间 / RNG / 日志）           │
├──────────────────────────────────────────────────────────┤
│  你的平台：ESP32+NimBLE / Zephyr / Linux BlueZ / …            │
└──────────────────────────────────────────────────────────┘
```

## 特性

- **零依赖核心**：纯 C11，不依赖 OS、BLE 协议栈、加密库
- **加密后端可替换**：`miot_crypto_ops_t` 函数指针对接 mbedTLS / OpenSSL / 任意实现（参考 [port/esp32](port/esp32)）
- **host 单元测试**：用空中抓包实证帧做黄金样本，`cmake && ctest` 即可验证
- **表驱动属性**：注册一张 `miot_prop_t` 表，get/set 自动处理（类型转换/范围夹取/权限校验/副作用回调）
- **完整认证链路**：REGISTER（ECDH P-256 + 绑定）与 LOGIN（纯对称 HKDF）状态机全部内置

## 快速开始

```bash
cmake -B build && cmake --build build && ctest --test-dir build
./build/example_scale.exe     # S400 全链路演示
```

### 最小设备代码

```c
#include "miot_ble/miot_profiles.h"

static int notify_0010(void *u, const uint8_t *d, size_t n) { return ble_notify(0x0010, d, n); }
static int notify_0019(void *u, const uint8_t *d, size_t n) { return ble_notify(0x0019, d, n); }
static int spec_notify (void *u, const uint8_t *d, size_t n) { return ble_notify(0x001A, d, n); }

void app_main(void) {
    miot_crypto_set_ops(&my_crypto_ops);        // 5 个加密原语，参考 port/esp32

    /* 认证层（0x0010 + 0x0019） */
    static miot_auth_t auth;
    profiles_wire_auth(&auth, NULL, notify_0010, notify_0019, NULL);
    miot_auth_prepare(&auth);                   // 开机预生成 ECDH（要 ~390ms）

    /* 设备层（0x001A）：注册档案后 spec 全自动 */
    static miot_device_t dev;
    miot_prop_t *tbl; size_t cnt;
    scale_s400_init(&dev, &port, &tbl, &cnt);

    /* GATT 写回调转发 */
    void on_0010_write(d, n) { miot_auth_on_0010(&auth, d, n); }
    void on_0019_write(d, n) { miot_auth_on_0019(&auth, d, n); }
    void on_001A_write(d, n) { miot_device_on_rx(&dev, d, n); }
    void on_ble_disconnect(void) { miot_auth_on_disconnect(&auth); }

    /* 主循环 50~100ms 一次（LOGIN devConfirm 重发） */
    miot_auth_poll(&auth, now_ms());

    /* LOGIN 成功后接通 spec 会话密钥 */
    if (miot_auth_is_logged_in(&auth))
        miot_device_set_session_keys(&dev, miot_auth_session_keys(&auth));

    /* 测量完成 → 一行代码上报（App 实测解析通过） */
    scale_s400_report(&dev, 64500 /*0.01kg*/, 500 /*0.1Ω*/, unix_ts());
}
```

## 已验证设备档案（miot_profiles）

| 档案 | 对应产品 | 验证状态 |
|------|----------|----------|
| `scale_s400_*` | 体脂秤 S400 同构 | 绑定 / 用户资料 / spec 读写 / 事件上报全流程 ✓（App 解析+广播 ✓，插件出数排查中） |
| `thermometer_*` | 温湿度计 cgdk3 / miaomiaoce-t9 | 绑定 + spec 读写 ✓（双 spec siid 兼容） |
| `switch_dev_*` | 夜灯 / 驱蚊器 0x040A | 绑定 + 开关下发/回报 ✓（set 后立刻回报） |
| `button_dev_*` | 无线按钮 XMWXKG01LM | 绑定 + 配置属性读写 ✓ |

## 接口一览

| 模块 | 职责 | 关键 API |
|------|------|----------|
| `miot_port` | 移植层回调 | `send_notify` / `time_now` / `random` / `log` |
| `miot_auth` | 标准认证 | `init` / `prepare` / `on_0010` / `on_0019` / `poll` / `on_disconnect` |
| `miot_device` | 组合层 | `init` / `attach` / `set_session_keys` / `on_rx` / `send_event` / `send_props_changed` |
| `miot_spec` | 帧编解码 | `build_event` / `build_props_changed` / `build_get/set_reply` / `build_action_reply` / `parse` |
| `miot_props` | 属性表 | `miot_prop_t` 注册表 + `find` / `encode` / `apply` |
| `miot_ag2` | 可靠传输 | `rx` / `send`（18B 分帧，512B 重组） |
| `miot_crypto` | 加密抽象 | `set_ops`（hmac/ccm/ecdh 5 原语）+ rc4/mixA/mixB/hkdf/ccm 工具 |
| `miot_profiles` | 设备档案 | 4 个已验证设备的 init/report 函数 |

## 已验证的运行环境

- ESP32-C3 + NimBLE + mbedTLS（S400 体脂秤模拟器，与米家 Android App 8.x 全流程互通：
  扫描添加 → 绑定（REGISTER）→ 设备初始化 → 登录（LOGIN）→ 用户资料推送 →
  spec 读写 → 事件上报 → App 端解析成功并广播到插件）

## 文档

- [docs/protocol.md](docs/protocol.md) —— 协议逆向文档（认证流程 / ag2 / spec v2 帧格式 / App 分发链）
- [test/test_spec.c](test/test_spec.c) —— 黄金样本即实测帧

## License

非商业许可 —— 仅供学习研究，禁止商用，详见 [LICENSE](LICENSE)。
