# Mesh 抓包作战计划（手机绑定 MHCB12G 用）

> 目标：在乐式 MHCB12G 模组米家绑定的全程，抓到完整的 Mesh 配网报文，
> 反推出 0x0926 这类 Mesh 品类到底走什么协议、证书在哪一步上场。
> 手机侧抓 HCI 日志不需要 root，也不需要嗅探硬件。

## 工具（已备好）

| 工具 | 用途 |
|---|---|
| `tools/btsnoop_parse.py` | 解析 Android 蓝牙 HCI 日志，输出 ATT/SMP 时间线（已自测通过） |
| adb | 拉 btsnoop、抓 logcat（一直在用） |
| 模组串口 LOG/TXD/RXD | 模组固件自己的日志（3.3V 电平！） |
| `tools/frida/`、`_v*.log` 流程 | 已有的 logcat 抓法不变 |

## 第一步：手机开启 HCI snoop 日志（到货前就可以开）

MIUI/Android 通用：

```
设置 → 更多设置 → 开发者选项 → 「蓝牙 HCI 信息收集日志」打开
然后关一次蓝牙再开（或开关一次飞行模式）让它生效
```

生效后手机开始把所有蓝牙 HCI 包写进 `/data/log/btsnoop_hci.log`（部分机型在
`/sdcard/MIUI/debug_log/` 或 `/data/log/` 下，文件名可能带日期）。

注意：开启后蓝牙日志会一直增长，抓完记得关掉，否则占存储。

## 第二步：抓绑定全过程

1. 模组上电（接好串口，我先挂着日志）
2. 米家 → `+` → 添加设备 → 找到模组（广播里会带 MESH 标识）
3. **完成绑定**（这是认证模组，理应一路绿灯）
4. 绑定成功后**立刻**停手，然后马上拉日志：

```bash
cd D:/workspace/temp/miot-ble-sdk
adb pull /data/log/btsnoop_hci.log _mesh_snoop.log   # 路径不通就用下面这条
adb bugreport _bugreport.zip   # 再从 zip 里解 FS/data/log/btsnoop_hci.log
```

5. 解析：

```bash
python tools/btsnoop_parse.py _mesh_snoop.log > _snoop_att.txt
```

`--raw` 加原始 hex，`--filter` 按 connection handle 过滤。重点看：
- `ATT WRITE_REQ h=0010 / h=0019` —— 米家直连认证（我们已经很熟的那套）
- **`cid=0x0018`（SM PDU）以外的 GATT Proxy：`h=1827/1828` 上的读写**
- Mesh Provisioning PDU：Proxy 服务 0x1828 的 `PROV` 段（0x0006 proxy PDU）
- SMP 配对：`cid=0x0006` —— 看手机有没有要求 LE Secure Connections

## 第三步：并行抓的另两路

| 路 | 内容 |
|---|---|
| 模组串口 | RTL8762 固件日志，配网窗口、证书加载、mesh beacon 都可能打出来 |
| logcat | `lyra-ble-connect` / 米家主进程，照旧 `adb logcat -v time` |

## 解读重点（对应我们已有的认知）

| 观察点 | 验证什么 |
|---|---|
| 绑定走 0x0010/0x0019 还是 0x1827/0x1828 | MESH2.0 是不是把标准认证扔了，走 Mesh Proxy |
| device info / REGISTER / LOGIN 命令字是否还出现 | 0x0926 死路的 -27 到底拦在哪一步 |
| 有没有证书哈希/证书内容传输 | 三层 PKI 在配网时是否真的逐帧上传 |
| provision 之后 App 下发什么 | appkey/netkey 分发流程 |

## 风险与备选

- btsnoop 只含 HCI 层（连接建立后的），广播帧本身不在里面——广播另用
  ESP32-C3 侧的 `adv print`（`frame` 命令）对照分析
- 若 `/data/log/btsnoop_hci.log` 拉不出来（部分 MIUI 需要 bugreport），走 bugreport
- 若模组绑定期间手机和模组距离太近导致抓包不完整，不影响：HCI 是手机侧视角，完整
