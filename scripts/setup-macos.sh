#!/bin/sh
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render macOS 构建环境准备
#   1) 检测 Xcode Command Line Tools
#   2) 检测 Homebrew（不自动装：交互式需 sudo，应让用户自主决定）
#   3) brew install qt cmake ninja（幂等）
#   4) 校验 Qt 版本（需 6.3+）+ cpp-mcp（vendored）
#
# 用法: sh scripts/setup-macos.sh  （纯 POSIX sh，dash/busybox 也能跑）。幂等。
# macOS 上 brew 的 qt formula 就是官方 Qt6 开源版，比 aqt 更省事（macOS way）。
set -eu

QT_MIN_MINOR=3

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "== seimi-render macOS setup =="
echo "  qt min version : 6.${QT_MIN_MINOR}"

# ---- 1. Xcode Command Line Tools ----
echo "== [1/4] check Xcode Command Line Tools =="
if ! xcode-select -p >/dev/null 2>&1; then
    echo "ERROR: Xcode Command Line Tools 未安装。" >&2
    echo "       请先运行：xcode-select --install" >&2
    exit 1
fi
echo "  xcode CLT present — ok"

# ---- 2. Homebrew ----
echo "== [2/4] check Homebrew =="
if ! command -v brew >/dev/null 2>&1; then
    echo "ERROR: Homebrew 未安装。macOS 上 Qt6 经 brew 安装。" >&2
    echo "       请先安装 Homebrew（见 https://brew.sh）。" >&2
    exit 1
fi

# Apple Silicon 上误装 Intel Homebrew（/usr/local）会让后续 macdeployqt 打包踩坑，提前预警。
BREW_PREFIX="$(brew --prefix)"
MACHINE_ARCH="$(uname -m)"
if [ "$MACHINE_ARCH" = "arm64" ] && [ "$BREW_PREFIX" = "/usr/local" ]; then
    echo "  [WARN] arm64 机器但 Homebrew 装在 /usr/local（Intel 版）。" >&2
    echo "         建议改装 arm64 Homebrew 到 /opt/homebrew，否则打包有架构坑。" >&2
fi
echo "  brew at $BREW_PREFIX — ok"

# ---- 3. brew install 依赖 ----
echo "== [3/4] brew install deps (qt cmake ninja) =="
# brew list 输出先缓存再 grep：grep 命中首行后提前退出会 SIGPIPE 杀掉 brew list，stderr 漏出污染输出。
INSTALLED_PKGS="$(brew list --formula -1 2>/dev/null || true)"
NEED_INSTALL=""
for pkg in qt cmake ninja; do
    if echo "$INSTALLED_PKGS" | grep -qx "$pkg"; then
        echo "  $pkg already installed"
    else
        NEED_INSTALL="$NEED_INSTALL $pkg"
    fi
done
NEED_INSTALL="$(echo "$NEED_INSTALL" | sed 's/^ *//')"
if [ -n "$NEED_INSTALL" ]; then
    echo "  installing: $NEED_INSTALL"
    # shellcheck disable=SC2086  # 需 word-splitting（包名是已知安全输入）
    brew install $NEED_INSTALL
else
    echo "  all brew deps already installed"
fi

# ---- 4. Qt 版本 + cpp-mcp 校验 ----
echo "== [4/4] verify Qt version + cpp-mcp =="
if [ -d /opt/homebrew/opt/qt ]; then
    QT_PREFIX="/opt/homebrew/opt/qt"
else
    QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo /usr/local/opt/qt)"
fi
QMAKE="$QT_PREFIX/bin/qmake"
if [ ! -x "$QMAKE" ]; then
    echo "ERROR: qmake 未找到：$QMAKE" >&2
    exit 1
fi

QT_VER="$("$QMAKE" -query QT_VERSION 2>/dev/null || echo "")"
if [ -z "$QT_VER" ]; then
    echo "  [WARN] 无法从 qmake 取 Qt 版本，跳过版本校验（继续）。" >&2
else
    # 6.8.2 → major=6, minor=8。qt_standard_project_setup 需 6.3+。
    QT_MAJOR="${QT_VER%%.*}"
    REST="${QT_VER#*.}"
    QT_MINOR="${REST%%.*}"
    if [ "$QT_MAJOR" != "6" ]; then
        echo "ERROR: Qt 版本 $QT_VER 不是 Qt6。" >&2; exit 1
    fi
    if [ "$QT_MINOR" -lt "$QT_MIN_MINOR" ]; then
        echo "ERROR: Qt 版本 $QT_VER 低于 6.${QT_MIN_MINOR}（需 qt_standard_project_setup）。" >&2
        echo "       请 brew upgrade qt。" >&2; exit 1
    fi
    echo "  Qt $QT_VER at $QT_PREFIX — ok (>= 6.${QT_MIN_MINOR})"
fi

# cpp-mcp 校验：已 vendored（非 submodule），正常 clone 即包含。
if [ ! -f "$ROOT_DIR/third_party/cpp-mcp/src/mcp_server.cpp" ]; then
    echo "ERROR: third_party/cpp-mcp/src/mcp_server.cpp 未找到。" >&2
    echo "       cpp-mcp 已 vendored 进仓库，若缺失请重新 clone（勿浅克隆）。" >&2
    exit 1
fi
echo "  third_party/cpp-mcp present (vendored) — ok"

echo ""
echo "== DONE =="
echo "Qt 安装于: $QT_PREFIX"
echo ""
echo "下一步 —— 构建（二选一）："
echo "  [开发模式·裸编译]"
echo "    cmake -G Ninja -DCMAKE_PREFIX_PATH=\"$QT_PREFIX\" -DCMAKE_BUILD_TYPE=Release -B build"
echo "    cmake --build build"
echo "  [打包成可分发的 .app bundle]"
echo "    bash scripts/package.sh"
