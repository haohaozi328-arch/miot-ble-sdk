# 一次性研究脚本（不是测试）

这里的程序**不参与构建、不进 ctest**，是逆向过程中的临时工具，留着是为了可复现当时的结论。

| 文件 | 用途 | 怎么跑 |
|---|---|---|
| `key_hunt.c` | 早期为 legacy PSK 认证穷举 (key, iv, nonce 布局) 组合 | `gcc -I include tools/experiments/key_hunt.c src/*.c -o kh && ./kh` |
| `repeller_frame.c` | 把 `repeller_report()` 组出来的明文帧打印出来，逐字段核对 zimi.mosq.v1 | `gcc -I include tools/experiments/repeller_frame.c src/*.c -o rf && ./rf` |

结论已经写进 [docs/psk-auth.md](../../docs/psk-auth.md) 和 [docs/protocol.md](../../docs/protocol.md)，
正常情况下不需要再跑这两个。
