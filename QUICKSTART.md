# QUICKSTART —— 一页纸跑通

> 给人类看的。给 AI 看的执行手册在 [AGENTS.md](AGENTS.md)。

## 你要什么

一块 10 块钱的 ESP32-C3，烧上本项目后，能被**官方原版米家 App** 发现并绑定成
一台「蓝牙温湿度计2」——不需要 root、不需要装模块、不需要改 App。

## 1. 买东西

| 件 | 说明 | 价格 |
|---|---|---|
| ESP32-C3 开发板 | 随便哪家，4MB Flash 够 | 10 元左右 |
| USB 数据线 | 要能传数据，不是纯充电线 | 家里有 |

## 2. 装环境（一次）

装 ESP-IDF **v6.0**：

- Windows：官方 ESP-IDF Installer 离线包，装完桌面会生成一个 ESP-IDF 6.0 的命令行快捷方式
- macOS / Linux：`git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git`
  → `./install.sh esp32c3` → `. ./export.sh`

目标芯片**不用选**：工程里的 `sdkconfig.defaults` 已经写死 `esp32c3`。
装完打开 ESP-IDF 的终端，`idf.py --version` 能出版本号就行。

## 3. 编译烧录（三条命令）

```bash
git clone https://github.com/haohaozi328-arch/miot-ble-sdk.git
cd miot-ble-sdk/esp32/multi_dev
idf.py build
idf.py -p COM5 flash monitor        # Windows: COM5；macOS/Linux: /dev/tty.usbmodem*
```

- 串口不确定？Windows 在设备管理器看「USB JTAG/serial debug unit」那一行；macOS 用 `ls /dev/tty.usb*`
- 烧不进去？按住板子上的 **BOOT** 键，点一下 **RST**，再松开 BOOT，然后重跑最后一条命令
- `idf.py build` 报 `CMake Error`，多半是 IDF 版本不对（必须是 v6.0）

默认启动的是**体脂秤 S400**。想换成温湿度计2 等其他设备，烧完之后在 `monitor` 里
直接敲 `dev` 看序号表，`dev 1` 切过去（设备会擦身份自动重启）。

## 4. 验收

**烧完先看串口这一行**（每 5 秒一次；`bound=0` 表示「未绑定、可被添加」，这是正常状态）：

```
I (405) MIOT: host synced, 米家体脂秤 S400  PID=0x30D9  MAC 10:00:3B:CE:42:03  bound=0
```

**然后在米家里「添加设备」，绑定过程中串口会出现下面几行（顺序固定）：**

```
ECDH keypair ready
REGISTER COMPLETE (result 0x11 on 0x0010)
REGISTER: did stored (utc=...)          ← 最关键的一行
*** STD REGISTER: bind result OK (bound=1) ***
*** STD LOGIN COMPLETE (0x21 on 0x0010) ***
```

**手机（官方原版米家 App，国区账号）：**

1. 首页「+」→ 添加设备 → 扫到「米家体脂秤 S400」（蓝牙名 `MJJ`）
2. 点它 → 绑定 → 出现设备页
3. 点页面上的「测量」，提示上秤时**短按一下板载 BOOT 键** → 页面出现体重并跳报告页

> 换过型号的人看这里：`dev 1` 是蓝牙温湿度计2，添加时扫到的是它，页面上看温湿度。
> 每台设备的验收点见 [docs/clean-app-matrix.md](docs/clean-app-matrix.md)。

## 5. 不成怎么办

| 现象 | 先看 |
|---|---|
| 扫不到设备 | [troubleshooting Q2](docs/troubleshooting.md) |
| 提示「手机网络异常」/ `(104,did=…)` | [troubleshooting Q1](docs/troubleshooting.md) —— **不是网络问题** |
| 一直「正在连接」后超时 | 广播超 31B 了，[Q2](docs/troubleshooting.md) |
| 绑定成功但页面没数 | [Q5](docs/troubleshooting.md) |

## 6. 别做的事

- ❌ 别在固件里填 did —— did 是云端发的，填了必报 104
- ❌ 别用海外 PID
- ❌ 别装 Xposed/Magisk/任何模块 —— 本项目不需要，也不提供
- ❌ 别期待它变成量产产品 —— 这是协议研究项目（[NOTICE.md](NOTICE.md)）
