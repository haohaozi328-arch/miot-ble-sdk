#!/usr/bin/env bash
# 编译（并可选烧录）领普 ES3 电池款假固件，ESP32-C3 / IDF v6.0
# 用法：
#   ./build_es3.sh          # 只编译
#   ./build_es3.sh flash COM36   # 编译 + 烧到指定串口
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/occ_es3_adv espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/es3_build.log 2>&1; rc=\$?; \
           grep -E 'error|Project build' build/es3_build.log | tail -8; exit \$rc"

if ! grep -q 'Project build complete' build/es3_build.log; then
  echo "!! 构建未完成，见 build/es3_build.log" >&2
  exit 1
fi

if [[ "${1:-}" == "flash" ]]; then
  PORT="${2:-COM36}"
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/occ_es3_adv.bin 2>&1 | tail -3
fi
