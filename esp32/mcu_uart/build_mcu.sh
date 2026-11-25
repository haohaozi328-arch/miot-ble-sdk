#!/usr/bin/env bash
# 编译（并可选烧录）假 MCU 固件，ESP32-C3 / IDF v6.0
#
# 用法：
#   ./build_mcu.sh          # 只编译
#   ./build_mcu.sh flash    # 编译 + 烧到 COM6 的 0x10000 应用分区
set -euo pipefail
cd "$(dirname "$0")"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/mcu_uart espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/mcu_build.log 2>&1; rc=\$?; \
           grep -E 'error|warning: |Project build' build/mcu_build.log | tail -6; exit \$rc"

if ! grep -q 'Project build complete' build/mcu_build.log; then
  echo "!! 构建未完成，见 build/mcu_build.log" >&2
  exit 1
fi

if [[ "${1:-}" == "flash" ]]; then
  PORT="${2:-COM6}"
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/mcu_uart.bin 2>&1 | tail -3
fi
