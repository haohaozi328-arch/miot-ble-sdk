# 米家 Mesh 灯实验工程（meshbulb）

`bulb_gatt` 的 spec 直连形态 + `multi_dev` 的 Mesh 承载（`mesh_miot.c`），
用来验证 **S0/S1 两级**：设备能否被识别为「未配网的 Mesh 灯」，以及网关/App
是否愿意发起配网。

## 与 bulb_gatt 的差别

| 项 | bulb_gatt | meshbulb |
|---|---|---|
| GATT | 0xFE95 spec | 0xFE95 spec **+ 0x1827 Prov + 0x1828 Proxy** |
| Mesh 栈 | 无 | NimBLE Mesh（PB-GATT，Proxy 开，Relay/Friend/LPN 关） |
| MiBeacon | FC 0x70/0x28 | **FC 首字节 bit7 = 1**（宣告自己是 Mesh 设备） |
| 默认 PID | 0x06EB | **0x0926（Yeelight Mesh Bulb M2）** |
| 构建 | `idf.py build` | `./build_mesh.sh`（要打 6 个移植补丁，走 docker） |

## 编译与烧录

```bash
cd esp32/meshbulb
./build_mesh.sh          # 只编译 → build/meshbulb.bin
./build_mesh.sh flash    # 编译 + 烧到 0x10000（COM6）
```

首次烧完若要回到默认 PID，先串口 `factory`（擦 NVS，`pid` 键优先级高于编译默认值）。

## 串口命令（新增）

| 命令 | 作用 |
|---|---|
| `info` | PID/bound/连接/mesh 状态（含 Device UUID） |
| `mesh` | 看 Mesh 状态；`mesh 1` / `mesh 0` 手动开关配网承载 |
| `meshbit 0\|1` | 关/开 MiBeacon 的 mesh 标志位（做 A/B 对照） |
| `muuid <32hex>` | 整帧替换 Mesh Device UUID（试不同候选格式） |
| `pid 0xXXXX` | 换 PID 并重启 |

## 启动应当看到的三行

```
MIOT-MESH: mesh gatt svcs registered (pre-start) cid=0x038F pid=0x0926
MIOT-MESH: bt_mesh_init OK (post-sync) elems=1 models=cfg+health+vnd(0x0000/0x0002)
MIOT-MESH: prov_enable rc=0 (0=OK, beacon 开跑)
```

## 判读（这就是本次实验的全部产出）

| 现象 | 含义 | 下一步 |
|---|---|---|
| 米家「附近设备」不出现 | S0 未过：MiBeacon 的 mesh 形态不对 | 换 `meshbit 0` 对照；再试 `muuid` 候选 |
| 出现但连上后 **零 write** 被断（`reason=0x213`） | PID 层拒绝（同 `bulb_gatt` 的 -27） | 换 PID：`0x06EC` / `0x06EB` |
| 出现且串口打 `*** PROV LINK OPEN bearer=PB-GATT ***` | **S1 达成**，走到了配网握手 | 接着看是否 `Invite` → `-32/-40` |
| 打 `-32` / `-40` 或直接 `PROV LINK CLOSE` | 卡在私有 provision（证书链） | 转 `docs/mesh-ref/10-mesh-plan-from-wifi-firmware.md` §4 三条路 |

## 已知边界（不要在这上面浪费时间）

1. Mesh 设备**不走 GATT 控制**：配网后 spec 命令由网关转成 Mesh 广播，本工程的
   0xFE95 通道只服务于配网前后的身份/兼容路径。
2. `mible_gateway_*` 那套（自己当网关）不复刻：云端按 PID 授权，`nvcsmt.light.wzj201`
   实测没有「蓝牙网关」开关，说明授权在档案层。
3. PB-ADV 默认关：它的 legacy 广播会和 MiBeacon 抢同一个广播实例。要试网关侧主动
   配网（音箱走 PB-ADV）时，改成 `CONFIG_BT_NIMBLE_MESH_PB_ADV=y` 并让
   `mesh_miot_set_prov` 同时开两个 bearer。
