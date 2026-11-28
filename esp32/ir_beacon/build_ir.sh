#!/usr/bin/env bash
# 编译（并可选烧录）红外接收固件，ESP32-C3 / IDF v6.0
#
# 用法：
#   ./build_ir.sh          # 只编译
#   ./build_ir.sh flash    # 编译 + 烧到 COM6
set -euo pipefail
cd "$(dirname "$0")"

HOST_ROOT="$(cygpath -m "$(cd "$(dirname "$0")/../.." && pwd)")"
mkdir -p build

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$HOST_ROOT:/project" \
  -w /project/esp32/ir_beacon espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/build.log 2>&1; rc=\$?; \
           grep -E 'error|warning: |Project build' build/build.log | tail -8; exit \$rc"

if ! grep -q 'Project build complete' build/build.log; then
  echo "!! 构建未完成，末尾日志：" >&2
  tail -30 build/build.log >&2
  exit 1
fi

if [[ "${1:-}" == "flash" ]]; then
  PORT="${2:-COM6}"
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/ir_beacon.bin 2>&1 | tail -3
fi
