#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render Linux/WSL 构建环境准备（Ubuntu 22.04）
#   1) apt 装系统构建依赖 + WebEngine 运行时库 + CJK 字体
#   2) aqtinstall 装 Qt 6.7.x（含 qtwebengine）到 /opt/Qt
#   3) 校验 third_party/cpp-mcp（已 vendored，非 submodule）
#
# 用法: bash scripts/setup-linux.sh  （或 sudo ...，WSL 常以 root 跑）
# 环境变量: QT_VERSION, QT_PREFIX。幂等。
#
# 已知坑：aqtinstall v3.x 对 Qt6 附加模块（qtwebsockets 等）元数据解析有 bug，
# `-m <module>` 报 "not found while parsing XML"。故 base+webengine 用 --archives
# 显式拉取，其它模块用 _install_qt_module.sh 直接下载 7z 归档。
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.7.2}"
QT_ARCH="${QT_ARCH:-gcc_64}"
QT_PREFIX="${QT_PREFIX:-/opt/Qt}"
QT_REPO="${QT_REPO:-https://download.qt.io/online/qtsdkrepository/linux_x64/desktop}"
QT_INSTALL_DIR="$QT_PREFIX/$QT_VERSION/$QT_ARCH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "== seimi-render Linux setup =="
echo "  qt version : $QT_VERSION"
echo "  qt prefix  : $QT_PREFIX"

# ---- 1. 系统依赖（构建工具 + WebEngine/Chromium 运行时库 + 字体）----
APT_PKGS=(
    cmake ninja-build build-essential pkg-config patchelf python3-pip p7zip-full curl
    libgl1-mesa-dev
    libnss3-dev libnss3 libxkbcommon-dev libxkbcommon0 libdbus-1-3
    libasound2 libasound2-dev
    libxkbfile1 libxkbfile-dev
    libxcomposite-dev libxcursor-dev libxi-dev libxrandr-dev libxtst-dev
    libxshmfence-dev libxrender-dev libxext-dev libx11-dev libx11-xcb-dev
    libxcb1-dev libxcb-keysyms1-dev libxcb-image0-dev libxcb-icccm4-dev
    libxcb-render-util0-dev libxcb-util-dev libxcb-shape0-dev libxcb-xfixes0-dev
    libxcb-randr0-dev libxcb-cursor-dev
    libcups2-dev libpci-dev libpulse-dev libfreetype6-dev libfontconfig1-dev
    fonts-liberation fonts-noto-cjk
)

echo "== [1/3] apt install system deps =="
NEED_INSTALL=()
for pkg in "${APT_PKGS[@]}"; do
    dpkg -s "$pkg" >/dev/null 2>&1 || NEED_INSTALL+=("$pkg")
done
if [[ ${#NEED_INSTALL[@]} -gt 0 ]]; then
    echo "  installing: ${NEED_INSTALL[*]}"
    SUDO=""
    [[ $EUID -ne 0 ]] && SUDO="sudo"
    $SUDO apt-get update -y
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "${NEED_INSTALL[@]}"
else
    echo "  all apt deps already installed"
fi

# ---- 2. aqtinstall + Qt 6.7.x ----
echo "== [2/3] install Qt $QT_VERSION (base + webengine + addon modules) =="
if ! command -v aqt >/dev/null 2>&1 && ! pip3 show aqtinstall >/dev/null 2>&1; then
    pip3 install aqtinstall
fi
export PATH="$HOME/.local/bin:$PATH"
if ! command -v aqt >/dev/null 2>&1; then
    echo "ERROR: aqt not found after pip install (try 'pip3 install aqtinstall' manually)" >&2
    exit 1
fi

if [[ -x "$QT_INSTALL_DIR/bin/qmake" ]]; then
    echo "  Qt base already installed at $QT_INSTALL_DIR — skipping base"
else
    # base + webengine：用 --archives 显式列 base 归档绕开 aqt 模块元数据 bug。
    # 6.7.x 官方 Qt 是 RHEL_8_8 构建（glibc 2.28，在 Ubuntu 22.04 的 2.35 上兼容）。
    echo "  installing Qt base + webengine via aqt..."
    aqt install-qt -b https://download.qt.io/ linux desktop "$QT_VERSION" "$QT_ARCH" \
        -m qtwebengine \
        --archives qtbase qtsvg qtdeclarative qttools qttranslations qtwayland icu \
        -O "$QT_PREFIX"
    if [[ ! -x "$QT_INSTALL_DIR/bin/qmake" ]]; then
        echo "ERROR: Qt base install dir not found at $QT_INSTALL_DIR" >&2
        exit 1
    fi
fi

# WebEngine 附加模块依赖用 _install_qt_module.sh 直接拉镜像 7z（aqt -m 对它们解析失败）。
install_module_if_missing() {
    local mod="$1" cmake_mod="$2"
    if [[ -f "$QT_INSTALL_DIR/lib/cmake/$cmake_mod/${cmake_mod}Config.cmake" ]]; then
        echo "  $mod already present — skipping"
    else
        QT_PREFIX="$QT_PREFIX" QT_VERSION="$QT_VERSION" QT_ARCH="$QT_ARCH" \
        QT_REPO="$QT_REPO" bash "$SCRIPT_DIR/_install_qt_module.sh" "$mod"
    fi
}
echo "  ensuring WebEngine dependency modules..."
install_module_if_missing qtwebsockets  Qt6WebSockets
install_module_if_missing qtwebchannel  Qt6WebChannel
install_module_if_missing qtpdf         Qt6Pdf
install_module_if_missing qtpositioning Qt6Positioning

# ---- 3. cpp-mcp 校验（已 vendored，正常 clone 即包含）----
echo "== [3/3] ensure third_party/cpp-mcp present =="
if [[ ! -f "$ROOT_DIR/third_party/cpp-mcp/src/mcp_server.cpp" ]]; then
    echo "ERROR: third_party/cpp-mcp/src/mcp_server.cpp not found." >&2
    echo "       cpp-mcp 已 vendored 进仓库，正常 clone 即应包含。" >&2
    echo "       若缺失，请重新 clone 本仓库（勿用浅克隆裁剪该路径）。" >&2
    exit 1
fi
echo "  third_party/cpp-mcp present (vendored) — ok"

echo ""
echo "== DONE =="
echo "Qt 安装于: $QT_INSTALL_DIR"
echo ""
echo "下一步 —— 设置 CMAKE_PREFIX_PATH（可加入 ~/.bashrc）:"
echo "  export CMAKE_PREFIX_PATH=$QT_INSTALL_DIR"
echo "然后构建:"
echo "  bash scripts/build-linux.sh"
echo ""
echo "⚠ WSL2 提示：Chromium 在 WSL2 下常因 IPv6/代理路由失败导致渲染报"
echo "  loadFinished(ok=false)。如遇此问题，启动时设置（详见 README）："
echo "  export QTWEBENGINE_CHROMIUM_FLAGS=\"--no-sandbox --disable-ipv6 --no-proxy-server\""
