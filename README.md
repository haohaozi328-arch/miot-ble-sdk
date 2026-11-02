# miot-ble-sdk

米家（MIoT）BLE Spec v2 设备端 SDK —— 从实测验证的 S400 体脂秤模拟固件中抽象而来。

核心价值：**所有协议格式均通过米家 App 反汇编（com.miot.spec 包 ud5/gyb/kt4 解析器）+ 空中抓包互证实测验证**，包括三个公开资料从未记载、决定 App 能否解析事件的关键帧格式细节（见 [docs/protocol.md](docs/protocol.md)）。

```
┌────────────────────────────────────────────────────┐
│  你的产品逻辑（体重计 / 温湿度计 / 灯 / 任意 MIoT 设备）│
├────────────────────────────────────────────────────┤
│  miot_device   组合层：加密通道+分派+属性表+主动上报    │
│  ├─ miot_spec   Spec v2 明文帧编解码（纯函数，可host测试）│
│  ├─ miot_props  属性表驱动执行器（get/set 自动处理）     │
│  ├─ miot_ag2    ag2 可靠传输层（START/ACK/DATA 分帧）   │
│  └─ miot_crypto 加密抽象（RC4/HKDF/AES-CCM，后端可替换） │
├────────────────────────────────────────────────────┤
│  miot_port  移植层接口（notify / 时间 / RNG / 日志）    │
├────────────────────────────────────────────────────┤
│  你的平台：ESP32+NimBLE / Zephyr / Linux BlueZ / …    │
└────────────────────────────────────────────────────┘
```

## 特性

- **零依赖核心**：`miot_spec`/`miot_ag2`/`miot_props` 纯 C11，不依赖 OS、BLE 协议栈、加密库
- **加密后端可替换**：通过 `miot_crypto_ops_t` 函数指针对接 mbedTLS / OpenSSL / 任意实现（参考 [port/esp32](port/esp32)）
- **host 单元测试**：用空中抓包实证帧做黄金样本，`cmake && ctest` 即可验证
- **表驱动属性**：注册一张 `miot_prop_t` 表，get/set 自动处理（类型转换/范围夹取/权限校验/副作用回调）

## 快速开始

### 编译 + 测试

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

### 最小设备代码

```c
#include "miot_ble/miot_device.h"

static uint8_t battery = 92;
static miot_prop_t props[] = {
    { 3, 1003, MIOT_TYPE_U8, MIOT_PROP_R, 1, &battery, 0, 0, NULL, "battery" },
};

static int my_notify(void *user, const uint8_t *d, size_t n) {
    return ble_gatt_notify(SPEC_CH, d, n);      /* 你的平台实现 */
}

void app_main(void) {
    miot_port_t port = { .send_notify = my_notify, /* ... */ };
    miot_device_t dev;
    miot_device_init(&dev, &port);
    miot_device_attach(&dev, props, 1, NULL, NULL, NULL);

    /* 标准认证完成后（你实现 0x0019 流程）注入 40B 会话密钥： */
    miot_device_set_session_keys(&dev, keys);

    /* GATT 写回调（0x001A）： */
    void on_ble_write(const uint8_t *d, size_t n) { miot_device_on_rx(&dev, d, n); }

    /* 测量完成 → 发事件（App 实测解析通过）： */
    miot_spec_value_t a = { .piid = 1, .type = MIOT_TYPE_STRING };
    strcpy(a.val.str, "0,0,0,64700,180,0,0,0,0");
    miot_device_send_event(&dev, 8, 2, &a, 1);
}
```

完整示例见 [examples/scale_s400](examples/scale_s400)。

## SDK 能做什么

| 模块 | 职责 | 对应逆向知识 |
|------|------|--------------|
| `miot_spec` | op 0-7 + F0 全套帧编解码 | 「事件参数带 piid16」「op=4 首字节 count」「op=6 仅 respCode」三大坑 |
| `miot_props` | 属性表驱动 get/set | 米家 spec 属性模型（siid/piid/type16） |
| `miot_ag2` | 可靠传输（START/ACK/DATA/SINGLE/SYNC） | 18B 分帧、512B 重组缓冲、status 语义 |
| `miot_crypto` | RC4/mixA/mixB、HKDF-SHA256、k61 AES-CCM | nonce 构造、40B 会话密钥布局 |
| `miot_device` | 组合 + 分派 + 主动上报 | App 分发链、应答时机 |

## 已验证的运行环境

- ESP32-C3 + NimBLE + mbedTLS（S400 体脂秤模拟器，与米家 App 8.x 全流程互通：
  绑定 → 用户资料推送 → spec 读写 → 事件上报 → App 端解析成功）

## 文档

- [docs/protocol.md](docs/protocol.md) —— 协议逆向文档（本文档库最有价值的部分）
- [test/test_spec.c](test/test_spec.c) —— 黄金样本即实测帧

## License

MIT
