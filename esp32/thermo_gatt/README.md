# 蓝牙温湿度计2（LYWSD03MMC）可绑定固件 —— ESP32-C3

**这是实测通过、可被米家 App 正常绑定并显示读数的完整固件工程**（2026-11-07 实测：
原版米家 App 添加成功，设备页实时读数正常）。克隆本仓库后无需改动任何路径即可编译。

- 目标芯片：ESP32-C3（USB-Serial/JTAG 直连）
- 模拟型号：蓝牙温湿度计2 `LYWSD03MMC`，PID `0x055B`，标准认证（非 PSK）
- 功能：MiBeacon V5 加密广播（温/湿/电量轮播）+ 0xFE95 GATT 服务
  （REGISTER/LOGIN 标准认证，spec 属性读写）

## 编译

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.0/esp32c3/get-started/) **v6.0**（其他版本未验证）。

```bash
# 本机装有 IDF 6.0 时
cd esp32/thermo_gatt
idf.py set-target esp32c3
idf.py build

# 或者用 Docker（不需要装 IDF）
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v6.0 idf.py set-target esp32c3
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v6.0 idf.py build
```

Windows Docker 用户挂载路径写法：`-v "D:/xxx/miot-ble-sdk:/project"`。

## 烧录（首次）

**首次烧录必须整片擦除**，否则 NVS 里残留的旧身份会让设备以"已绑定"状态广播，
米家 App 的添加列表会过滤掉它（表现为扫描不到）：

```bash
idf.py -p COM6 erase-flash
idf.py -p COM6 flash monitor
```

`monitor` 里能看到的关键日志：

```
host synced, MAC xx:xx:xx:xx:xx:xx  bound=0     ← 全新待绑定
wr 0010 #1 len=4 cmd=A2                          ← App 发起注册
REGISTER: did stored (utc=...) bound=1           ← 云端下发 did，绑定成功
*** bind state -> 1 (did stored, adv switches to registered) ***
*** session established: spec channel ready ***  ← LOGIN 成功
spec properties pushed (25.xC / 58.x% / batt 88%)
```

## 改参数

全部集中在 `main/app_main.c` 顶部，注释已标明：

| 想改什么 | 位置 |
|---|---|
| 模拟的型号 / PID | `THERMO_PID`（换型号后需同步 `src/miot_profiles.c` 的 spec 属性表） |
| 广播里的设备名 | `THERMO_NAME` |
| 温/湿/电量读数 | `v_temp / v_hum / v_batt` 与 `sim_tick()` |
| 广播间隔 | `ADV_PERIOD_MS` |
| 绑定后长按 BOOT 5 秒 | 恢复出厂（擦 NVS 重绑），见 `app_main.c` 的按键循环 |

## 实测踩过的坑（代码里有详细注释，改代码前先读）

1. **广播 MAC 字段必须是小端（LSB）序** —— 云端/网关用扫描到的 BLE 地址按 LSB 拼
   nonce 解密广播帧，写成正序则永远解不开，现象是"设备在线、广播正常、页面永远没读数"
   （`app_main.c` 的 `on_sync()`）。
2. **绑定成功必须当场置 bound 并切换广播帧** —— 云端下发 did 后若仍广播未绑定心跳帧，
   App/网关收不到任何属性（`src/miot_auth.c` 的 `register_on_encrypted_did()`）。
3. **未绑定帧必须带 Capability 字节且 registered=0** —— App 添加列表按 Frame Control
   位过滤，缺位直接无视（`src/miot_beacon.c` 的 `miot_beacon_frame_hello()`）。
4. **spec 属性编号必须与米家 spec 一致** —— LYWSD03MMC 用 `2.1/2.2/2.3`，不是某些
   型号的 `2.1001` 私有编号（`src/miot_profiles.c` 的 `thermometer_*`）。
5. **模拟值要缓慢波动** —— 网关对"值没变"的属性只上报一次，写死的值会导致设备页
   顶部有读数但历史图表一直"数据为空"（`sim_tick()`）。
6. IDF v6.0 / mbedTLS 4.x：CCM/ECP/ECDH 的 legacy 接口需要
   `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` 与显式开启 `CONFIG_MBEDTLS_*_C`
   （`main/CMakeLists.txt` 与 `sdkconfig.defaults`）。

## 免责声明

见仓库根目录 [NOTICE.md](../../NOTICE.md)。本项目仅用于学习研究，设备读数为模拟数据；
不得用于冒充真实商品设备、绕过米家平台安全机制或其他违反平台协议的用途。
