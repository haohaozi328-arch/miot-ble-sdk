#!/usr/bin/env bash
# 编译 / 烧录 ir_bus 的两个工程（ESP32-C3 / ESP-IDF v6.0，走 Docker）
#
# 用法：
#   ./build.sh hub             # 只编译 Hub
#   ./build.sh node            # 只编译 Node
#   ./build.sh hub flash       # 编译 + 烧到 COM6
#   ./build.sh node flash COM7 # 指定端口
set -euo pipefail
cd "$(dirname "$0")"

PROJ="${1:-hub}"
ACTION="${2:-build}"
PORT="${3:-COM6}"

case "$PROJ" in
  hub)  BIN=ir_hub ;;
  node) BIN=ir_node ;;
  node_miot) BIN=ir_node_miot ;;
  *) echo "用法: $0 {hub|node|node_miot} [build|flash] [COMx]" >&2; exit 2 ;;
esac

# ir_bus/<proj> -> ../.. = esp32 ; ../../.. = 仓库根
HOST_ROOT="$(cygpath -m "$(cd ../.. && pwd)")"
mkdir -p "$PROJ/build"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$HOST_ROOT:/project" \
  -w "/project/esp32/ir_bus/$PROJ" espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/build.log 2>&1; rc=\$?; \
           grep -E 'error|warning: |Project build' build/build.log | tail -10; exit \$rc"

if ! grep -q 'Project build complete' "$PROJ/build/build.log"; then
  echo "!! 构建未完成，末尾日志：" >&2
  tail -40 "$PROJ/build/build.log" >&2
  exit 1
fi

echo "[build] $PROJ OK -> $PROJ/build/$BIN.bin"

if [[ "$ACTION" == "flash" ]]; then
  python -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 "$PROJ/build/$BIN.bin" 2>&1 | tail -3
fi
