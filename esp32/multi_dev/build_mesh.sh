#!/usr/bin/env bash
# 带 Mesh 移植补丁的构建脚本（IDF v6.0 / ESP32-C3 multi_dev）
#
# 为什么需要补丁：IDF v6.0 的 NimBLE Mesh 移植层有 4 处 bug，
# 直接用官方文件会崩溃或 beacon 发不出去。补丁文件在 ./patch/，
# 构建时在容器内覆盖到 IDF 源文件上（只影响本次临时容器，不动镜像）。
#
# 用法：
#   ./build_mesh.sh            # 只编译
#   ./build_mesh.sh flash      # 编译 + 烧录 0x10000 应用分区到 COM6
set -euo pipefail

cd "$(dirname "$0")"
# mesh 源码树是三层 nimble，porting 是两层，别混用
MESH=/opt/esp/idf/components/bt/host/nimble/nimble/nimble
PORTING=/opt/esp/idf/components/bt/host/nimble/nimble/porting

# 补丁文件 -> IDF 内相对路径
P1="patch/adv_legacy.c:$MESH/host/mesh/src/adv_legacy.c"
P2="patch/adv.c:$MESH/host/mesh/src/adv.c"
P3="patch/glue.c:$MESH/host/mesh/src/glue.c"
P4="patch/npl_os_freertos.c:$PORTING/npl/freertos/src/npl_os_freertos.c"
# P5：proxy_srv.c 断连时 client==NULL 解引用（Load access fault，手机一断开就重启）
P5="patch/proxy_srv.c:$MESH/host/mesh/src/proxy_srv.c"
# P6：net.c 未配网时丢弃网络 PDU —— 加一行明文头打印（NID/TTL/SEQ/SRC）
P6="patch/net.c:$MESH/host/mesh/src/net.c"

# 强制重编：删掉对应 .obj（mtime 判断不可靠，补丁文件可能比 obj 旧）
OBJ=build/esp-idf/bt/CMakeFiles/__idf_bt.dir
rm -f "$OBJ/host/nimble/nimble/nimble/host/mesh/src/adv_legacy.c.obj" \
      "$OBJ/host/nimble/nimble/nimble/host/mesh/src/adv.c.obj" \
      "$OBJ/host/nimble/nimble/nimble/host/mesh/src/glue.c.obj" \
      "$OBJ/host/nimble/nimble/nimble/host/mesh/src/proxy_srv.c.obj" \
      "$OBJ/host/nimble/nimble/nimble/host/mesh/src/net.c.obj" \
      "$OBJ/host/nimble/nimble/nimble/porting/npl/freertos/src/npl_os_freertos.c.obj"

# 踩过的坑（勿改回）：
#  1) 只挂载单个文件（-v xxx.c:...:ro）在 Git Bash + Docker Desktop 下会
#     时灵时不灵：路径 /d/... 不认、cygpath 反斜杠被 "D:" 截断，都会
#     "构建成功"但固件是未打补丁的原版。现在改为容器内 cp 覆盖，稳定可靠。
#  2) 镜像自带 ccache，会命中旧对象把补丁顶回原版，必须 CCACHE_DISABLE=1。
#  3) 不要把 idf.py build 的输出接 `| head -N`：head 提前退出会让构建进程
#     收到 SIGPIPE 半途被杀，留下旧/半成品对象。
INNER=""
for m in "$P1" "$P2" "$P3" "$P4" "$P5" "$P6"; do
  src="${m%%:*}"; dst="${m##*:}"
  INNER="$INNER cp -f /project/esp32/multi_dev/$src $dst &&"
done
INNER="$INNER echo -n 'patched adv_legacy hits: ' && grep -c ev_inited $MESH/host/mesh/src/adv_legacy.c && echo -n 'patched proxy_srv null-guards: ' && grep -c 'if (!client)' $MESH/host/mesh/src/proxy_srv.c && echo -n 'patched net.c hdr-log: ' && grep -c 'NET PDU if=' $MESH/host/mesh/src/net.c"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "D:/workspace/temp/miot-ble-sdk":/project \
  -w /project/esp32/multi_dev espressif/idf:v6.0 \
  bash -c "$INNER && source /opt/esp/idf/export.sh >/dev/null 2>&1 \
           && CCACHE_DISABLE=1 idf.py build > build/mesh_build.log 2>&1; rc=\$?; \
           grep -E 'error|Project build' build/mesh_build.log | tail -4; exit \$rc"

if ! grep -q 'Project build complete' build/mesh_build.log; then
  echo "!! 构建未完成，见 build/mesh_build.log" >&2
  exit 1
fi

if [[ "${1:-}" == "flash" ]]; then
  python -m esptool --chip esp32c3 -b 460800 \
    --before default-reset --after hard-reset \
    write-flash 0x10000 build/multi_dev.bin 2>&1 | tail -2
fi
