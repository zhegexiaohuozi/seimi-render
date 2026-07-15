#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0

# =============================================================
# seimi-render Linux/WSL 构建环境准备脚本（Ubuntu 22.04）
#
# 做三件事：
#   1) apt 安装系统构建依赖 + WebEngine 运行时所需系统库（Chromium 的 X11/ALSA/NSS 等）
#      + 字体（CJK 页面如 sohu 需要 noto-cjk）
#   2) 用 aqtinstall 装一份独立的 Qt 6.7.x（含 qtwebengine）到 /opt/Qt
#   3) 校验 third_party/cpp-mcp 已就位（cpp-mcp 已 vendored 进本仓库，
#      不再是 git submodule，无需 git submodule update）
#
# 为什么用 aqtinstall 而非 apt：Ubuntu 22.04 官方源 Qt 仅 6.2.4，
# 与 macOS brew 的较新 Qt 对齐，且 6.7+ 满足 qt_standard_project_setup()（6.3+）。
#
# 已知坑（已处理）：aqtinstall v3.x 对 Qt6 部分附加模块（qtwebsockets/
# qtwebchannel/qtpdf/qtpositioning）的元数据解析有 bug，`-m <module>` 会报
# "packages [...] were not found while parsing XML"。本脚本对 base + webengine
# 用 aqt 的 --archives 显式拉取（绕过模块元数据），对其它附加模块用
# _install_qt_module.sh 直接下载镜像里的 7z 归档（带 sha1 校验）。
#
# 用法（在 WSL / Ubuntu 22.04 内）:
#   bash scripts/setup-linux.sh
#   # 或以 root（WSL 常见），apt 步骤无需 sudo 提示：
#   sudo bash scripts/setup-linux.sh
#
# 可调环境变量：
#   QT_VERSION=6.7.2 QT_PREFIX=/opt/Qt bash scripts/setup-linux.sh
#
# 幂等：已安装的依赖/Qt/模块会跳过。
# =============================================================
# 兼容：若被 sh(dash) 调用，自动用 bash 重跑自身（脚本用了 pipefail 等 bash 特性）。
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

# ---- 1. 系统依赖（apt）----
# 分两类：构建工具 + Qt WebEngine/Chromium 运行时系统库（链接期与运行期都需要）。
APT_PKGS=(
    # 构建工具
    cmake ninja-build build-essential pkg-config patchelf python3-pip p7zip-full curl
    # Qt base 链接依赖
    libgl1-mesa-dev
    # Qt WebEngine / Chromium 运行时系统库（链接 libQt6WebEngineCore 需要）
    libnss3-dev libnss3 libxkbcommon-dev libxkbcommon0 libdbus-1-3
    libasound2 libasound2-dev          # Chromium 音频（WebEngineCore NEEDED）
    libxkbfile1 libxkbfile-dev         # XkbRF_GetNamesProp
    libxcomposite-dev libxcursor-dev libxi-dev libxrandr-dev libxtst-dev
    libxshmfence-dev libxrender-dev libxext-dev libx11-dev libx11-xcb-dev
    libxcb1-dev libxcb-keysyms1-dev libxcb-image0-dev libxcb-icccm4-dev
    libxcb-render-util0-dev libxcb-util-dev libxcb-shape0-dev libxcb-xfixes0-dev
    libxcb-randr0-dev libxcb-cursor-dev
    libcups2-dev libpci-dev libpulse-dev libfreetype6-dev libfontconfig1-dev
    # 字体：offscreen 渲染需要可用的字体文件，否则页面全是方块
    fonts-liberation fonts-noto-cjk
)

echo "== [1/3] apt install system deps =="
NEED_INSTALL=()
for pkg in "${APT_PKGS[@]}"; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        NEED_INSTALL+=("$pkg")
    fi
done
if [[ ${#NEED_INSTALL[@]} -gt 0 ]]; then
    echo "  installing: ${NEED_INSTALL[*]}"
    # 以 root 运行时跳过 sudo（避免非交互环境卡在密码提示）。
    SUDO=""
    if [[ $EUID -ne 0 ]]; then SUDO="sudo"; fi
    $SUDO apt-get update -y
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "${NEED_INSTALL[@]}"
else
    echo "  all apt deps already installed"
fi

# ---- 2. aqtinstall + Qt 6.7.x ----
echo "== [2/3] install Qt $QT_VERSION (base + webengine + addon modules) =="
if ! command -v aqt >/dev/null 2>&1 && ! pip3 show aqtinstall >/dev/null 2>&1; then
    echo "  installing aqtinstall via pip..."
    pip3 install aqtinstall
fi
# aqt 装到 ~/.local/bin（pip --user），确保在 PATH
export PATH="$HOME/.local/bin:$PATH"
if ! command -v aqt >/dev/null 2>&1; then
    echo "ERROR: aqt not found after pip install (try 'pip3 install aqtinstall' manually)" >&2
    exit 1
fi

if [[ -x "$QT_INSTALL_DIR/bin/qmake" ]]; then
    echo "  Qt base already installed at $QT_INSTALL_DIR — skipping base"
else
    # base + webengine：用 --archives 显式列出 base 归档（绕开 aqt 模块元数据 bug），
    # 配合 -m qtwebengine 拉 webengine（实测该组合可用）。6.7.x 官方 Qt 是 RHEL_8_8 构建
    # （glibc 2.28，在 Ubuntu 22.04 的 2.35 上向后兼容）。
    echo "  installing Qt base + webengine via aqt..."
    aqt install-qt -b https://download.qt.io/ linux desktop "$QT_VERSION" "$QT_ARCH" \
        -m qtwebengine \
        --archives qtbase qtsvg qtdeclarative qttools qttranslations qtwayland icu \
        -O "$QT_PREFIX"
    if [[ ! -x "$QT_INSTALL_DIR/bin/qmake" ]]; then
        echo "ERROR: Qt base install dir not found at $QT_INSTALL_DIR" >&2
        echo "       check aqt output above." >&2
        exit 1
    fi
fi

# WebEngine 的附加模块依赖（qtwebsockets / qtwebchannel / qtpdf / qtpositioning）
# 用 _install_qt_module.sh 直接拉镜像 7z（aqt 的 -m 对这些模块解析失败）。
# 已安装则跳过（按 cmake config 文件判断）。
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

# ---- 3. cpp-mcp 校验（已 vendored，不再是 submodule）----
echo "== [3/3] ensure third_party/cpp-mcp present =="
# cpp-mcp 以前是 git submodule，现已 vendored 为本仓库直接管理的源码
#（含 seimi 定制的接入层鉴权）。普通 git clone 即包含全部内容，无需
# git submodule update。这里只做存在性校验：若文件缺失说明仓库不完整
#（浅克隆出错 / 手动误删），应报错让用户重新 clone 而非继续构建。
if [[ ! -f "$ROOT_DIR/third_party/cpp-mcp/src/mcp_server.cpp" ]]; then
    echo "ERROR: third_party/cpp-mcp/src/mcp_server.cpp not found." >&2
    echo "       cpp-mcp 已 vendored 进仓库，正常 clone 即应包含。" >&2
    echo "       若缺失，请重新 clone 本仓库（勿用浅克隆裁剪该路径）。" >&2
    exit 1
else
    echo "  third_party/cpp-mcp present (vendored) — ok"
fi

echo ""
echo "== DONE =="
echo "Qt 安装于: $QT_INSTALL_DIR"
echo ""
echo "下一步 —— 设置 CMAKE_PREFIX_PATH（可加入 ~/.bashrc）:"
echo "  export CMAKE_PREFIX_PATH=$QT_INSTALL_DIR"
echo ""
echo "然后构建:"
echo "  bash scripts/build-linux.sh"
echo ""
echo "⚠ WSL2 提示：Chromium 在 WSL2 下常因 IPv6/代理路由失败导致渲染报"
echo "  loadFinished(ok=false)。如遇此问题，启动时设置（详见 README）："
echo "  export QTWEBENGINE_CHROMIUM_FLAGS=\"--no-sandbox --disable-ipv6 --no-proxy-server\""
