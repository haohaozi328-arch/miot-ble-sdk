# 驱蚊器智能版（WX08ZM / 0x040A）legacy PSK 固件 —— ESP32-C3

**这是用来验证「legacy PSK 认证路径」的最小工程**（2026-11-07 上真机。
进度与结论见文末「实测状态」，以及 [`docs/clean-app-matrix.md`](../../docs/clean-app-matrix.md) 第 5 项）。

- 目标芯片：ESP32-C3（USB-Serial/JTAG 直连）
- 模拟型号：驱蚊器智能版 `WX08ZM`，PID `0x040A`
- 认证路径：**legacy PSK**（`smartcfg-*` HKDF 标签 + 设备签名，不是标准认证）
- 功能：MiBeacon V5 广播（开关 0x1012 / 余量 0x1013 / 电量 0x100A 轮播）
  + 0xFE95 GATT 服务（PSK 握手 + spec 属性 2.1 开关 / 3.1 电量）

工程与 [`../thermo_gatt`](../thermo_gatt) 同构，差别只有三处：PID、spec 属性表、广播对象。
认证走哪条由 App 按型号决定，所以这里不需要「手动选择 PSK」——设备收到 26B 启动帧会自动切过去。

## 编译

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0/esp32c3/get-started/) **v6.0**（其他版本未验证）。

```bash
cd esp32/wx08zm_gatt
idf.py set-target esp32c3
idf.py build
```

Docker（不用装 IDF）：

```bash
docker run --rm -v "D:/xxx/miot-ble-sdk:/project" -w /project/esp32/wx08zm_gatt \
    espressif/idf:v6.0 idf.py set-target esp32c3
docker run --rm -v "D:/xxx/miot-ble-sdk:/project" -w /project/esp32/wx08zm_gatt \
    espressif/idf:v6.0 idf.py build
```

## 烧录（首次必须整片擦除）

```bash
idf.py -p COM6 erase-flash
idf.py -p COM6 flash monitor
```

整片擦除是为了清掉 NVS 里的旧身份：否则设备以「已绑定」状态广播，App 的添加列表会直接过滤掉它。

## 改参数

全部集中在 `main/app_main.c` 顶部：

| 想改什么 | 位置 | 说明 |
|---|---|---|
| 模拟的型号 / PID | `DEV_PID` | 换型号后要同步改 `src/miot_profiles.c` 的属性表 |
| 广播里的设备名 | `DEV_NAME` | 只是个名字，App 主要按 PID 认型号 |
| 绑定能力位 | `BOND_ABILITY` | 0 无 / 1 前绑定 / 2 后绑定 / 3 Combo。**App 不列出设备时按 2→1→3 试** |
| 开关/电量/余量 | `v_sw / v_batt / v_margin` 与 `sim_tick()` | 接真实器件时替换 |
| PSK trace / 签名变体 | `PSK_TRACE` / `PSK_SIGN_VAR` | 见下一节 |
| 广播间隔 | `ADV_PERIOD_MS` | 绑定后 500ms，未绑定 100~200ms |
| 长按 BOOT 5 秒 | `app_main()` 里的按键循环 | 恢复出厂（擦 NVS 重绑） |

## PSK 实测怎么读串口

PSK 由 **App 主动发起**，所以只要把收到的原始帧打出来，就能对照状态机看走到哪一步。
`PSK_TRACE` 默认 **1**（打印每帧 hex），`PSK_SIGN_VAR` 默认 0。

| 串口输出 | 含义 | 下一步 |
|---|---|---|
| `PSK: start frame 26B captured` | 判定成 PSK 设备，启动帧已留存 | 看下一行 |
| `PSK: ECDH ok` + `handshake sent (info/0D/03/0C/04)` | 五帧全部发出（含签名） | 等 App 反应 |
| 只有 `(info/0D/03/0C)` 没有 `/04` | 后端没实现 `ecdsa_sign` | 检查 `port/esp32/miot_port_esp32.c` |
| `PSK: server pub received` | App 认可握手 | 等绑定结果 |
| `PSK: *** BOUND, did stored` | **成了**，did 已落盘 | 回填矩阵，删掉「待实测」 |
| `PSK: unhandled <n>B (psk_state=…)` | 有帧没识别 | 看上一行 `[psk] 0019` 的 hex 按长度归类 |
| 完全没有 `PSK:` 行 | 没被判成 PSK | 确认 PID 是 `0x040A`；看 0x0019 有没有 26B 写入 |
| App 报错但串口已到 `handshake sent` | 云端拒了签名 | 把 `PSK_SIGN_VAR` 换成 1 重烧，再试 2 |

**签名变体**（`docs/psk-auth.md` §5）：`0` = 签 `devRand‖appRand`，`1` = 只签 `devRand`，
`2` = 签 `shared‖devRand‖appRand`，都是 ECDSA-P256。云端若验签，三次之内必命中；
若云端只做 TOFU（记录首次的 pub + sign），`0` 就直接过。

## 实测状态

> **2026-11-07：待实测。** 本工程编译通过、烧录后能回到待绑定态；
> PSK 握手是否被云端接受，需要一次真机添加才能定论。
> 结论出来后要更新本节和 `docs/clean-app-matrix.md` 第 5 行
> （`⚠️ 待实测` → `✅ 能` 或 `❌ 不能`，并写清卡在哪一步）。

## 免责声明

见仓库根目录 [NOTICE.md](../../NOTICE.md)。本项目仅用于学习研究，设备读数为模拟数据；
不得用于冒充真实商品设备、绕过米家平台安全机制或其他违反平台协议的用途。
