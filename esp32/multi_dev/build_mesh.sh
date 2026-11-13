#!/usr/bin/env bash
# 带 Mesh 移植补丁的构建脚本（IDF v6.0 / ESP32-C3 multi_dev）
#
# 为什么需要挂载：IDF v6.0 的 NimBLE Mesh 移植层有 4 处 bug，
# 直接用官方文件会崩溃或 beacon 发不出去。补丁文件在 ./patch/，
# 构建期以只读 bind mount 覆盖容器内的原始文件（不改动 IDF 镜像）。
#
# 用法：
#   ./build_mesh.sh            # 只编译
#   ./build_mesh.sh flash      # 编译 + 烧录 0x10000 应用分区到 COM6
set -euo pipefail

cd "$(dirname "$0")"
PROJ="$(pwd)"
ROOT="$(cd ../.. && pwd)"          # 仓库根（docker -v 用）
PATCH="$PROJ/patch"
IDF="/opt/esp/idf/components/bt/host/nimble/nimble"

# 补丁 -> 容器内路径
M1="host/mesh/src/adv_legacy.c"
M2="host/mesh/src/adv.c"
M3="host/mesh/src/glue.c"
M4="porting/npl/freertos/src/npl_os_freertos.c"

# patch/ 里的文件是 IDF 源文件副本，改动后 mtime 可能比 .obj 新/旧不定，
# 保险起见先删掉对应 .obj 强制重编。
OBJ=build/esp-idf/bt/CMakeFiles/__idf_bt.dir
rm -f "$OBJ/host/nimble/nimble/nimble/$M1.obj" \
      "$OBJ/host/nimble/nimble/nimble/$M2.obj" \
      "$OBJ/host/nimble/nimble/nimble/$M3.obj" \
      "$OBJ/host/nimble/nimble/nimble/$M4.obj"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$ROOT":/project \
  -v "$PATCH/adv_legacy.c":$IDF/$M1:ro \
  -v "$PATCH/adv.c":$IDF/$M2:ro \
  -v "$PATCH/glue.c":$IDF/$M3:ro \
  -v "$PATCH/npl_os_freertos.c":$IDF/$M4:ro \
  -w /project/esp32/multi_dev espressif/idf:v6.0 \
  bash -c "source /opt/esp/idf/export.sh >/dev/null 2>&1 && idf.py build 2>&1 | grep -E 'error|warning: .*mesh|Project build' | head -6"

if [[ "${1:-}" == "flash" ]]; then
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/multi_dev.bin 2>&1 | tail -2
fi
