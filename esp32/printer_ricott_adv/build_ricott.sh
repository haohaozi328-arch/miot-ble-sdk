#!/usr/bin/env bash
# 编译（并可选烧录）米家口袋照片打印机Pro 假固件，ESP32-C3 / IDF v6.0
# 用法：
#   ./build_ricott.sh          # 只编译
#   ./build_ricott.sh flash    # 编译 + 烧到 COM6
#   ./build_ricott.sh flash COMxx
set -euo pipefail
cd "$(dirname "$0")"
PORT="${2:-COM6}"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/printer_ricott_adv espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/ricott_build.log 2>&1; rc=\$?; \
           grep -E 'error|Project build' build/ricott_build.log | tail -8; exit \$rc"

if [ "${1:-}" = "flash" ]; then
  MSYS_NO_PATHCONV=1 docker run --rm --privileged \
    -v "D:/workspace/temp/miot-ble-sdk":/project \
    -w /project/esp32/printer_ricott_adv espressif/idf:v6.0 \
    bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
             && idf.py -p $PORT flash 2>&1 | tail -3"
fi
