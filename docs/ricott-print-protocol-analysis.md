# 米家口袋照片打印机Pro（ricott）打印协议分析

> 2026-11-25 现场逆向 + 公开资料交叉验证。目标读者：想接手把这个假打印机做到
> 「能打印」的后续开发者。绑定链路已在仓库实测通过，打印通道卡在经典蓝牙，
> 本文档给出完整证据链、协议栈细节和实现路线图。

## 1. 设备档案

| 项 | 值 |
|---|---|
| 产品 | 米家口袋照片打印机Pro（热升华 + 色带，Hannto 恒德代工） |
| model | `xiaomi.printer.ricott`（海外版兄弟型号 `xiaomi.printer.ricotg`、`ricotp`） |
| PID | **0x5F6D**（十进制 24429），bt_bind_style=2，resolve_type=3（广播可扫描识别） |
| spec | `urn:miot-spec-v2:device:printer:0000A060:xiaomi-ricott:1`（released） |
| spec 内容 | 极简：siid1 设备信息（5 个 string）+ siid4/siid5 空壳服务——**打印完全不在 spec 里**，全走私有通道 |
| 仓库工程 | [`esp32/printer_ricott_adv`](../esp32/printer_ricott_adv)（ESP32-C3，12 特征值 GATT + MiBeacon，已实测绑定 3/3、设备页完整渲染） |

## 2. 已验证结论（本仓实测）

1. **BLE 绑定链路完全正常**：米家添加 → MiBeacon 扫描识别 → GATT 认证
   （0010/0019 通道，`REGISTER COMPLETE 0x11`）→ did 下发 → 设备页完整渲染。
2. **设备页卡「连接中」**：串口日志显示绑定完成后手机断开（0x213），之后
   40s+ **无任何 BLE 活动**——手机根本没回来连 GATT。
3. **根因（手机 logcat 铁证）**：

   ```
   BluetoothSocketManagerBinder: connectsocket: device=XX:XX:XX:XX:05:08,
       type=1, uuid=00001101-0000-1000-8000-00805f9b34fb   ← SPP UUID
   btsock_rfc_connect: service_uuid:00001101-...（RFCOMM socket）
   CreateClassicConnection: Connection initiated for CLASSIC to remote
   SDP 服务发现（L2CAP PSM 0x0001）
   ```

   插件在绑定后对**同一 MAC** 发起**经典蓝牙（BR/EDR）RFCOMM + SPP** 连接。
   ESP32-C3 只有 BLE、没有经典蓝牙，连接必然超时 → 永远「连接中」。

## 3. 传输层：经典蓝牙 SPP（Hannto 家族共性）

同厂打印机（mizink 逆向的 basil + tuat-yate 逆向的 Pro 海外版）SDP 一律注册
三个 RFCOMM 通道：

| 通道 | 用途 | ricott 是否需要 |
|---|---|---|
| **channel 1** — SPP（UUID `00001101`） | 完整 JSON-RPC + 文件协议（Android/PC 走这条） | ✅ **必须**（logcat 实证） |
| channel 22 — Apple iAP2 | iOS 走的 EA session（`com.hannto.*`） | 可不做（Android 测试不需要） |
| channel 23 — 微信小程序传输 | 不同协议 | 可不做 |

真机行为参考：空闲 ~15s 会断链（需心跳保活）；SPP 就是透明字节流，帧可
粘包（App 一次 write 最多合并 3 帧）。

## 4. 线协议栈（tuat-yate 实机验证，直接可用）

### 4.1 帧格式（0x7E 分帧，总长 22 + bodyLen）

| 偏移 | 长度 | 含义 |
|---|---|---|
| 0 | 1 | `0x7E` 帧头 |
| 1 | 1 | `0x64` 版本 1.01 |
| 2 | 1 | `0x00` 保留 |
| 3 | 1 | channelId：`1` JSON 数据 / `2` 文件 / `3` 加密 JSON / `4` 加密文件 / `255` 认证 |
| 4 | 1 | interactive：`6` 请求 / `7` 响应 / `16`~`19` DH 握手四步 |
| 5 | 1 | encoding：`1` binary / `2` hex / `3` json |
| 6 | 4 | arcMsgSn（一般 0，LE） |
| 10 | 4 | msgSn（每侧自增计数，LE） |
| 14 | 2 | packageTotal（分包总数，LE） |
| 16 | 2 | packageNum（1-based，LE） |
| 18 | 2 | msgAttribute = `bodyLen \| (encType<<10) \| (0x2000 若多包)` |
| 20 | N | body（按需加密） |
| 20+N | 1 | 校验和 = `(sum(帧[0..20+N)) - 0x7E) & 0xFF`（头尾两个 0x7E 抵消） |
| 21+N | 1 | `0x7E` 帧尾 |

**encType**（msgAttribute 高位）：`0` 明文、`4` mijia RC4（basil 用）、
`5` **AES-128-ECB + DH 会话密钥（Pro/ricott 用）**。偏移表见
`ENCRYPT_OFFSET`（如 `101` → +5120）。

### 4.2 DH 握手（server 侧视角，无云端参与）

```
App → 设备  channel=255 interactive=16  body="hello"
设备 → App  channel=255 interactive=17  body = G(4B) | P(16B) | RA(16B)   ← 设备出题
App → 设备  channel=255 interactive=18  body = RB(16B) | AES-ECB(key, P)[:16]
设备 → App  channel=255 interactive=19  body = "ok"（明文，补零对齐）
```

- `G/P/RA` 是设备随机出的 DH 参数；`RA` 随机私钥对应的公钥（`RA = G^a mod P`）
- App 计算 `RB = G^b mod P`、共享密钥 `K = RA^b mod P = G^(ab) mod P`
- **密钥取 K 的 ASCII-hex 前 16 字节**（见下面的大数编码！）
- App 把 `AES-ECB(K, P)` 的前 16 字节作为回执，设备侧用同样方法算 K 验证
  解密结果 == 自己的 P，相等则回 `ok`
- 之后所有数据/文件帧用 K 做 AES-128-ECB，**零填充（不是 PKCS#7）**

### 4.3 ★大数 ASCII-hex 编码（Hannto 特有的坑）

DH 的大数不是二进制，而是**「字节数组先当 ASCII 字符串，再把该字符串当十六进制数」**：

```python
def bytes_to_int(buf):  return int(buf.decode("latin1"), 16)   # 字节→ASCII→hex→int
def int_to_bytes(n):    return format(n, "X").encode("ascii")  # int→大写hex→ASCII
# RB 不足 16B：左补 '0'(0x30) 右对齐 pack16_front；K 不足 16B：右补 0x00 pack16_end
```

完整参考实现：`refs/xiaomi-photo-printer/xiaomi_photo_printer/protocol.py`
（`compute_confirm()` 一函数搞定 server 侧验证）。

### 4.4 JSON-RPC 命令集（channel 3，加密）

body 是 `{"method":...,"id":N,"params":{...}}` 加密后的 JSON。tuat-yate 实测
Pro 可用的命令（插件同源，ricott 大概率一致）：

```
→ print_job  {file_size, copies, job_type:0照片/1证件/2证件照/3文档, channel:576}
             ← {"result":{"job_id":N}}
→ (channel 4 加密文件) 分块上传：body = jobId(4B LE) + JPEG(≤988B/块)
             每块独立 AES-ECB 加密，packageTotal/packageNum 标分包
→ job_info   {job_id}  ← [{job_state: downloading/printing/...}]（轮询进度）
```

basil（mizink）的流程多了 `clean_data` / `mixed_status` / `confirm_job`，
且打印尺寸 1040×1560（JPEG）。**ricott 具体命令面以插件 JS 为准**（见 §7）。

## 5. ESP32 经典款实现路线图

### 5.1 硬件要求（硬性）

- **必须 ESP32 经典款**（ESP32-WROOM-32 / WROVER / NodeMCU-32S 等，双模
  BR/EDR + BLE）。S3/C3/C6/H2 均无经典蓝牙，**不可用**。
- MAC 对齐：手机是拿 BLE 广播地址当经典地址去连的（logcat 实证）。ESP32
  默认 BT 经典 MAC = base、BLE MAC = base+2，**要让两者一致**：烧录时设置
  自定义 base MAC 使 BT 经典地址 == BLE 地址（`esp_base_mac_addr_set` +
  `esp_iface_mac_addr_set`，或 efuse 烧 BLK3）。新板新 MAC，需重新绑定。

### 5.2 软件栈

- IDF 换 **Bluedroid**（经典蓝牙必须 Bluedroid；NimBLE 与 Bluedroid 二选一）：
  - BLE GATT 侧：把 `gatt_miot.c`（NimBLE API）移植到 Bluedroid GATT
    （仓库 `esp32/occ_adv` 系列全部是 NimBLE，无现成 Bluedroid 版，是最大工作量）
  - 经典侧：SPP server（官方例程 `examples/bluetooth/bluedroid/classic/spp_server`）
    —— 注册 SPP SDP 记录（UUID 00001101），accept 连接后拿到 RFCOMM fd
- 打印协议：把 §4 的帧格式/DH/ECB 直接 C 移植（mbedtls AES-ECB 现成；
  DH 是模幂，mbedtls `mbedtls_mpi` 现成，P 128bit 很小，毫秒级算完）

### 5.3 工作分解（预估）

| 步骤 | 内容 | 工作量 |
|---|---|---|
| ① | Bluedroid 双栈骨架（SPP server + 最小 GATT）跑通 | 中 |
| ② | GATT 移植：认证 0010/0019、spec 属性、MiBeacon 广播 | **大**（本仓最大单项） |
| ③ | SPP 上 0x7E 帧 + DH 握手 server 侧 | 小（协议已明确） |
| ④ | JSON-RPC 命令面 + 文件接收 + 打印模拟 | 中（需插件 JS 对照） |
| ⑤ | 实测调优（心跳保活/分包/进度上报） | 小 |

### 5.4 捷径选项（不做 Bluedroid 移植）——已实现，回环自测通过

模拟服务端已落地：**[`tools/ricott_spp_mock.py`](../tools/ricott_spp_mock.py)**（零硬依赖，
pycryptodome 缺失时自动用内置纯 Python AES，NIST 向量验证）。

```bash
python tools/ricott_spp_mock.py --loopback   # 本机自测（tuat-yate 客户端 ↔ 本 mock）✅ 2026-11-25 全部通过
python tools/ricott_spp_mock.py --tcp 9527   # TCP 隧道模式（server 逻辑在 PC，字节流由蓝牙主机转发）
python tools/ricott_spp_mock.py --rfcomm     # Linux/BlueZ 直起 RFCOMM SPP server（channel 1，UUID 00001101）
```

自测覆盖：DH 握手（出题/回执校验）→ channel 3 JSON-RPC（print_job/job_info）→
channel 4 分块收文件（988B/块）→ 逐字节比对 PASS。

**实现时的关键发现（tuat-yate 没明说，ESP32 移植必踩）**：DH 的 P 字段只有
16 字节 ASCII-hex → 模数上限 **64-bit**（16 个 hex 字符 = 64 bit），不是 128-bit！
本 mock 用 2⁶⁴−59（最大 64-bit 素数）。G 同理最多 4 个 hex 字符。

**MAC 问题的修正分析**：logcat 显示手机是拿「绑定时记录的 BLE 广播地址」直接发
classic page——所以只要有个 **MAC 可改的经典蓝牙适配器**，就能让手机真正连上 mock：

```bash
# Linux + CSR 芯片 USB dongle（约 ¥10，bdaddr 工具可改 MAC）：
sudo bdaddr -i hci0 10:00:3B:CE:05:08   # 改成被绑定设备的 BLE 地址
sudo hciconfig hci0 up
python tools/ricott_spp_mock.py --rfcomm
# 米家插件点「连接」→ SPP page 命中 dongle → SDP 找到 00001101 → RFCOMM 建链
# → mock 收到真实插件的 DH/JSON-RPC 流量 → 协议同版实锤，甚至可能完整走通打印
```

Windows 蓝牙适配器改不了 MAC → 本机只能做协议验证/隧道模式；没有 Linux 环境时，
这是「无 ESP32 验证 ricott 协议」的最短路径。真正量产假设备仍建议 ESP32 经典款
（§5.1），mock 已把 server 侧全部协议逻辑验证完毕，C 移植可对照 `tools/ricott_spp_mock.py`。

## 6. 给接手者的验证清单

1. `adb logcat | grep -E "connectsocket|btsock_rfc|CreateClassicConnection"`——
   复现 §2.3 的 SPP 尝试（应看到 UUID 00001101）
2. ESP32 经典款烧 SPP server → 手机蓝牙设置里对设备「配对」→ 插件点连接 →
   logcat 应出现 RFCOMM 建链成功（state: connected）
3. 抓 SPP 字节流（server 侧直接打印）确认首帧是
   `7E 64 00 FF 10 03 ...`（channel 255 + interactive 16）
4. 按 §4.2 应答 DH，若插件收 `ok` 后开始发 JSON-RPC → 协议同版实锤
5. 对照 `print_job` 参数里的 `channel` 字段（tuat-yate 记录 Pro=576）微调

## 7. 开放问题

- ricott（国内版）encType 是否同为 `5`/DH-AES-ECB（vs basil 的 RC4 token）？
  → 高概率是（同 Pro 平台），步骤 4 一验便知
- 打印文件格式：basil 是 JPEG（1040×1560），Pro 同；ricott 插件是否直接发
  JPEG 还是私有光栅？→ 需要插件 JS（米家 RN 包 `@hbmiot/printer-ricott`，
  可从米家插件 CDN 下载解包）
- `print_job` 的 `channel` 参数具体取值、`job_type` 枚举国内版差异
- iAP2/微信通道国内版是否复用（Android 测试不需要）

## 8. 参考资料（本机路径）

| 资源 | 路径 | 价值 |
|---|---|---|
| **本仓 SPP 模拟服务端** | `tools/ricott_spp_mock.py` | **server 侧协议全实现（回环自测通过）**，C 移植对照 |
| tuat-yate/xiaomi-photo-printer | `refs/xiaomi-photo-printer/` | **Pro 完整协议实现**（帧/DH/命令/文件，真机验证） |
| fliptheweb/mi-portable-photo-printer | `refs/mizink/` | Hannto 家族传输层全景（SDP 三通道、encType 4/5 差异、命令流） |
| 本仓固件（绑定部分） | `esp32/printer_ricott_adv/` | BLE GATT + MiBeacon，已实测 |
| ricott spec | `_spec_xiaomi-ricott.json` | 属性表存档 |
| 本仓标签打印机协议（对照） | `esp32/printer_label_adv/README.md` | 同为打印机品类，BLE 私有通道的另一种做法 |
