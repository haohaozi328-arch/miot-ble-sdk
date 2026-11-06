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

跟着官方装 ESP-IDF **v5.5.x**，目标选 `esp32c3`：

- Windows：ESP-IDF Installer 离线包
- macOS / Linux：`git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git` → `./install.sh esp32c3`

装完打开 "ESP-IDF PowerShell/Terminal"，输入 `idf.py --version` 能出版本号就行。

## 3. 让 AI 干活

把仓库地址和这句话丢给 AI：

```
读 miot-ble-sdk/AGENTS.md，按里面的 S1~S5 做完：
建工程、写 ble_link/store/main、编译、烧到 COM口，把串口日志贴给我。
只用官方原版米家 App，不要用 hook/root/改 App。
```

AI 会自己建工程、编译、烧录。你只需要插上板子、告诉它串口号。

## 4. 验收

**串口应该看到这几行（顺序固定）：**

```
ECDH keypair ready
REGISTER COMPLETE (result 0x11 on 0x0010)
REGISTER: did stored (utc=...)          ← 最关键的一行
*** STD REGISTER: bind result OK (bound=1) ***
*** STD LOGIN COMPLETE (0x21 on 0x0010) ***
```

**手机（官方原版米家 App，国区账号）：**

1. 首页「+」→ 添加设备 → 扫到「蓝牙温湿度计2」
2. 点它 → 绑定 → 出现设备页
3. 页面上有温度/湿度数值 → 成功

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
