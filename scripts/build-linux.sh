#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render Linux 裸编译脚本（Ninja + Qt6）
#   bash scripts/build-linux.sh [Release|Debug]
# 环境变量: QT_PREFIX, BUILD_DIR, BUILD_TYPE
# 若被 sh 调用，自动用 bash 重跑（脚本用了 pipefail 等 bash 特性）。
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

CONFIG="${1:-${BUILD_TYPE:-Release}}"
QT_PREFIX="${QT_PREFIX:-/opt/Qt/6.7.2/gcc_64}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BIN="$BUILD_DIR/seimi-render"

echo "== seimi-render build (Linux) =="
echo "  root      : $ROOT_DIR"
echo "  config    : $CONFIG"
echo "  qt prefix : $QT_PREFIX"
echo "  build dir : $BUILD_DIR"

if [[ ! -x "$QT_PREFIX/bin/qmake" ]]; then
    echo "ERROR: Qt not found at $QT_PREFIX/bin/qmake" >&2
    echo "       run scripts/setup-linux.sh first, or set QT_PREFIX=<qt-install-dir>." >&2
    exit 1
fi

# 脏缓存检测：项目目录移动/拷贝后 CMakeCache 记录的源码路径失效，CMake 会拒绝复用。
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CACHE_SRC="$(grep -m1 '^CMAKE_HOME_DIRECTORY:' "$BUILD_DIR/CMakeCache.txt" \
                 | sed 's/^CMAKE_HOME_DIRECTORY:[^=]*=//')"
    if [[ -n "$CACHE_SRC" && "$CACHE_SRC" != "$ROOT_DIR" ]]; then
        echo "[WARN] stale CMake cache detected, wiping $BUILD_DIR to reconfigure."
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
    fi
fi

echo "== [1/2] cmake configure =="
cmake -G Ninja -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_BUILD_TYPE="$CONFIG"

echo "== [2/2] cmake build =="
cmake --build "$BUILD_DIR" --target seimi-render

if [[ -x "$BIN" ]]; then
    echo ""
    echo "== DONE =="
    echo "  binary: $BIN"
    echo ""
    echo "  运行（WSL2/容器/root 下建议加 --no-sandbox）:"
    echo "    $BIN --no-sandbox --http-port 8088 --ws-port 8089"
else
    echo "ERROR: binary not produced at $BIN" >&2
    exit 1
fi
