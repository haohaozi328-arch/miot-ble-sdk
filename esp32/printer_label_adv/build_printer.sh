#!/usr/bin/env bash
# 编译（并可选烧录）米家标签打印机假固件，ESP32-C3 / IDF v6.0
# 用法：
#   ./build_printer.sh          # 只编译
#   ./build_printer.sh flash    # 编译 + 烧到 COM6
set -euo pipefail
cd "$(dirname "$0")"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/printer_label_adv espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/printer_build.log 2>&1; rc=\$?; \
           grep -E 'error|Project build' build/printer_build.log | tail -8; exit \$rc"

if ! grep -q 'Project build complete' build/printer_build.log; then
  echo "!! 构建未完成，见 build/printer_build.log" >&2
  exit 1
fi

if [[ "${1:-}" == "flash" ]]; then
  PORT="${2:-COM6}"
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/printer_label_adv.bin 2>&1 | tail -3
fi
