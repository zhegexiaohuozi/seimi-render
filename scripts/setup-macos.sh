#!/bin/sh
# =============================================================
# seimi-render macOS 构建环境准备脚本
#
# 做四件事：
#   1) 检测 Xcode Command Line Tools（CMake/Ninja/编译器链的前提）
#   2) 检测 Homebrew（Qt6/CMake/Ninja 的安装通道）
#   3) brew install Qt6 + CMake + Ninja（幂等：已装的跳过）
#   4) 校验 Qt 版本（需 6.3+，因为 qt_standard_project_setup）
#      + 校验 third_party/cpp-mcp 已就位（vendored，正常 clone 即包含）
#
# 为什么不用 aqtinstall（像 setup-linux.sh / setup-windows.bat 那样）：
#   macOS 上 Homebrew 的 `qt` formula 就是官方 Qt6 开源版，版本紧跟上游，
#   且 brew 装的 Qt 能被 macdeployqt 正确识别框架路径——比 aqt 更省事、
#   更"macOS way"。README 也是这么指引的（brew install qt cmake ninja）。
#   aqt 在 macOS 上也能用（clang_64），但仅在需要精确锁定某个 Qt 版本时才有价值。
#
# 用法:
#   sh scripts/setup-macos.sh
#   # 或：
#   bash scripts/setup-macos.sh
#
# 兼容性：纯 POSIX sh（dash/ash/busybox sh 都能跑），不依赖 bash 特性。
# macOS 的 /bin/sh 是 bash 3.2 的 POSIX 模式，也能直接跑。
#
# 幂等：已安装的依赖会跳过，可重复运行。
# =============================================================
set -eu

# qt_standard_project_setup() 在 Qt 6.3 引入，CMakeLists.txt 用了它，故最低 6.3。
QT_MIN_MINOR=3

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "== seimi-render macOS setup =="
echo "  qt min version : 6.${QT_MIN_MINOR}"

# ---- 1. Xcode Command Line Tools ----
echo "== [1/4] check Xcode Command Line Tools =="
# CMake/Ninja/编译器链的前提。brew install cmake 时若 CLT 缺失，brew 会提示，
# 但提前检测能给更清晰的报错，避免被 brew 的二次提示混淆。
if ! xcode-select -p >/dev/null 2>&1; then
    echo "ERROR: Xcode Command Line Tools 未安装。" >&2
    echo "       请先运行：" >&2
    echo "         xcode-select --install" >&2
    echo "       （弹窗里点「安装」，装完重跑本脚本。）" >&2
    exit 1
fi
echo "  xcode CLT present — ok"

# ---- 2. Homebrew ----
echo "== [2/4] check Homebrew =="
# 不自动安装 brew：brew 的官方安装是交互式（需 sudo/密码、弹窗），脚本里替用户
# 跑体验差；且 brew 是系统级组件，用户应自主决定。检测缺失则给清晰命令让用户自己跑。
if ! command -v brew >/dev/null 2>&1; then
    echo "ERROR: Homebrew 未安装。macOS 上 Qt6 经 brew 安装。" >&2
    echo "       请先安装 Homebrew（一行命令，见 https://brew.sh）：" >&2
    echo '         /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"' >&2
    echo "       装完后按 brew 的提示把 brew 加入 PATH（Apple Silicon 会提示 eval 一句），" >&2
    echo "       再重跑本脚本。" >&2
    exit 1
fi

# Apple Silicon vs Homebrew 架构一致性预警（不阻塞，只提示）。
# arm64 机器应配 arm64 Homebrew（/opt/homebrew）；若误装 Intel Homebrew（/usr/local），
# 后续 macdeployqt 打包会踩坑（见 package.sh 的相关注释）。这里提前暴露。
BREW_PREFIX="$(brew --prefix)"
MACHINE_ARCH="$(uname -m)"
if [ "$MACHINE_ARCH" = "arm64" ] && [ "$BREW_PREFIX" = "/usr/local" ]; then
    echo "  [WARN] 当前是 Apple Silicon (arm64)，但 Homebrew 装在 /usr/local（Intel 版）。" >&2
    echo "         建议改装 arm64 Homebrew（装到 /opt/homebrew），否则后续打包会有架构/路径坑。" >&2
    echo "         （不阻塞本脚本，可稍后处理。）" >&2
fi
echo "  brew at $BREW_PREFIX — ok"

# ---- 3. brew install 依赖 ----
echo "== [3/4] brew install deps (qt cmake ninja) =="
# Qt6 在 brew 上的 formula 名就是 `qt`（不是 qt6）。CMake/Ninja 是构建工具。
# 用 brew list 精细检测（对齐 setup-linux.sh 用 dpkg -s 的幂等风格），避免重复装。
#
# 坑（已踩）：不能写 `brew list ... | grep -qx "$pkg"` 直接进 if。
# 即使没有 pipefail（POSIX sh 没有），grep -x 命中首行后立即退出 0，提前关闭管道；
# brew list 还在往里写后续行 → 收到 SIGPIPE 被杀（rc=141）。虽然 POSIX sh 默认不
# 让管道因 SIGPIPE 失败，但 brew list 异常退出会让其 stderr 漏出来污染输出。
# 把 brew list 输出先缓存再 grep 即可（顺便只调一次 brew list，更快）。
INSTALLED_PKGS="$(brew list --formula -1 2>/dev/null || true)"
NEED_INSTALL=""
for pkg in qt cmake ninja; do
    if echo "$INSTALLED_PKGS" | grep -qx "$pkg"; then
        echo "  $pkg already installed"
    else
        # 用空格分隔的字符串累积待装包（POSIX sh 无数组）。
        NEED_INSTALL="$NEED_INSTALL $pkg"
    fi
done
# 去掉前导空格后判断是否为空。
NEED_INSTALL="$(echo "$NEED_INSTALL" | sed 's/^ *//')"
if [ -n "$NEED_INSTALL" ]; then
    echo "  installing: $NEED_INSTALL"
    # shellcheck disable=SC2086  # 需 here 需 word-splitting（包名是已知安全输入）
    brew install $NEED_INSTALL
else
    echo "  all brew deps already installed"
fi

# ---- 4. Qt 版本 + cpp-mcp 校验 ----
echo "== [4/4] verify Qt version + cpp-mcp =="
# Qt prefix 解析：优先 arm64 Homebrew，回退 Intel Homebrew 或 brew --prefix qt。
# 与 package.sh 的检测逻辑保持一致（两处不能漂移）。
if [ -d /opt/homebrew/opt/qt ]; then
    QT_PREFIX="/opt/homebrew/opt/qt"
else
    QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo /usr/local/opt/qt)"
fi
QMAKE="$QT_PREFIX/bin/qmake"
if [ ! -x "$QMAKE" ]; then
    echo "ERROR: qmake 未找到：$QMAKE" >&2
    echo "       brew install qt 是否成功？请检查上方输出。" >&2
    exit 1
fi

# 取 Qt 版本：qmake -query QT_VERSION 输出纯数字（如 6.8.2），brew 的 qt 完整安装总能取到。
QT_VER="$("$QMAKE" -query QT_VERSION 2>/dev/null || echo "")"
if [ -z "$QT_VER" ]; then
    echo "  [WARN] 无法从 qmake 取 Qt 版本，跳过版本校验（继续）。" >&2
else
    # 解析主/次版本号：用 shell 参数扩展切 '.'。
    # 6.8.2 → major=6, minor=8。仅比较主版本和次版本号。
    QT_MAJOR="${QT_VER%%.*}"
    REST="${QT_VER#*.}"
    QT_MINOR="${REST%%.*}"
    if [ "$QT_MAJOR" != "6" ]; then
        echo "ERROR: Qt 版本 $QT_VER 不是 Qt6（项目要求 Qt6）。" >&2
        echo "       brew 的 qt formula 应为 Qt6，请检查 brew 是否被改过 tap/版本。" >&2
        exit 1
    fi
    if [ "$QT_MINOR" -lt "$QT_MIN_MINOR" ]; then
        echo "ERROR: Qt 版本 $QT_VER 低于 6.${QT_MIN_MINOR}" >&2
        echo "       （项目用了 qt_standard_project_setup，需 Qt 6.3+）。" >&2
        echo "       请 brew upgrade qt。" >&2
        exit 1
    fi
    echo "  Qt $QT_VER at $QT_PREFIX — ok (>= 6.${QT_MIN_MINOR})"
fi

# cpp-mcp 校验：已 vendored 进仓库（不再是 submodule），正常 clone 即包含。
# 与 setup-linux.sh 同源检查，保持两平台一致。
if [ ! -f "$ROOT_DIR/third_party/cpp-mcp/src/mcp_server.cpp" ]; then
    echo "ERROR: third_party/cpp-mcp/src/mcp_server.cpp 未找到。" >&2
    echo "       cpp-mcp 已 vendored 进仓库，正常 clone 即应包含。" >&2
    echo "       若缺失，请重新 clone 本仓库（勿用浅克隆裁剪该路径）。" >&2
    exit 1
else
    echo "  third_party/cpp-mcp present (vendored) — ok"
fi

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
echo ""
echo "⚠ Apple Silicon 用户：确保 Homebrew 是 arm64 版（/opt/homebrew），"
echo "  否则 macdeployqt 打包会有架构/路径坑（详见 package.sh 注释）。"
