#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0

# =============================================================
# seimi-render Linux 自包含打包脚本
#
# 对标 macOS 的 package.sh（macdeployqt bundle），产出可拷贝分发的 tar：
#   build/seimi-render-linux-x64.tar.gz
#
# 原理（linuxdeployqt 已弃用且不支持 Qt6，社区标准做法是手动 bundle）：
#   1) 先用 build-linux.sh 编出裸 ELF
#   2) ldd 抽取 Qt6 相关 .so（递归到不动点），拷进 bundle/lib/
#   3) 从 Qt 安装目录拷 QtWebEngineProcess、resources、locales、offscreen 插件
#   4) patchelf 把主程序与 QtWebEngineProcess 的 RPATH 设为 $ORIGIN/../lib
#   5) 生成 wrapper 脚本：设 QTWEBENGINE_*_PATH + LD_LIBRARY_PATH + exec
#   6) tar 打包
#
# 系统库（glibc/libnss3/libstdc++/libGL 等）不打包——目标机需自行满足，
# 见 README「目标机最小依赖」。这是 Linux tar 分发的标准做法，避免 glibc 不兼容。
#
# 用法:
#   bash scripts/package-linux.sh                      # 默认 Release
#   bash scripts/package-linux.sh Debug
#   bash scripts/package-linux.sh clean                # 清空 build 后全量重建（Release）
#   bash scripts/package-linux.sh clean Debug          # clean 可与配置组合，顺序不限
#
# 可调环境变量：QT_PREFIX、BUILD_DIR、BUILD_TYPE、DIST_NAME
# =============================================================
# 兼容：若被 sh(dash) 调用，自动用 bash 重跑自身（脚本用了 pipefail 等 bash 特性）。
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

# 参数解析：逐个解析，支持 clean（清空 build 目录全量重建）+ CONFIG 任意顺序组合。
# 未知参数直接报错退出，避免误用（如把 clean 当成 CONFIG 传给 cmake）。
# 对齐 package-windows.bat 的解析风格。
CONFIG=""
DO_CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        clean) DO_CLEAN=1 ;;
        Release|Debug|RelWithDebInfo|MinSizeRel) CONFIG="$1" ;;
        *)
            echo "ERROR: 未知参数 '$1'（应为 clean | Release | Debug | RelWithDebInfo | MinSizeRel）" >&2
            echo "       正确用法：bash scripts/package-linux.sh Release" >&2
            echo "                 bash scripts/package-linux.sh Release clean" >&2
            exit 1
            ;;
    esac
    shift
done
[[ -z "$CONFIG" ]] && CONFIG="${BUILD_TYPE:-Release}"

QT_PREFIX="${QT_PREFIX:-/opt/Qt/6.7.2/gcc_64}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
DIST_ROOT="${DIST_ROOT:-$BUILD_DIR/dist}"

# 产物名带版本 + OS + 架构（对齐 macOS package.sh）。
# 版本从 CMakeLists.txt 文本解析；架构在构建后从主二进制 ELF 头读（见 [1/5] 后），
# 避免 uname 骗人（32 位容器/交叉编译）。DIST_NAME 可整体覆盖。
APP_VERSION="$(tr '\n' ' ' < "$ROOT_DIR/CMakeLists.txt" \
    | grep -oE 'project\([^)]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' \
    | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[[ -z "$APP_VERSION" ]] && APP_VERSION="unknown"
DIST_NAME="${DIST_NAME:-seimi-render-${APP_VERSION}-linux}"  # 架构后缀在 [1/5] 构建后追加
DIST_DIR="$DIST_ROOT/$DIST_NAME"

echo "== seimi-render packaging (Linux) =="
echo "  root      : $ROOT_DIR"
echo "  config    : $CONFIG"
echo "  qt prefix : $QT_PREFIX"
echo "  dist dir  : $DIST_DIR"

# 前置：patchelf 必须可用
if ! command -v patchelf >/dev/null 2>&1; then
    echo "ERROR: patchelf not found. Install: sudo apt-get install -y patchelf" >&2
    exit 1
fi
if ! command -v ldd >/dev/null 2>&1; then
    echo "ERROR: ldd not found." >&2
    exit 1
fi

# ---- 0. (可选) 彻底清空 build 目录 ----
# clean 比 build-linux.sh 的增量构建更彻底：直接 rm -rf 整个 build 目录
# （含 dist/、旧 tar、CMake 缓存、所有中间产物），强制从零重建。
if [[ "$DO_CLEAN" == "1" ]]; then
    echo "== [0/5] clean build dir =="
    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"
        if [[ -d "$BUILD_DIR" ]]; then
            echo "ERROR: rm -rf 失败，build 目录仍存在（可能被进程占用）" >&2
            exit 1
        fi
        echo "  build dir removed."
    else
        echo "  build dir absent, nothing to clean."
    fi
fi

# ---- 1. 先构建 ELF（复用 build-linux.sh）----
echo "== [1/5] build ELF =="
"$SCRIPT_DIR/build-linux.sh" "$CONFIG"

BIN_SRC="$BUILD_DIR/seimi-render"
if [[ ! -x "$BIN_SRC" ]]; then
    echo "ERROR: binary not found at $BIN_SRC" >&2
    exit 1
fi

# 从主二进制 ELF 头读架构（比 uname 可靠：32 位容器/交叉编译下 uname 会骗人）。
# file 输出形如 "... ELF 64-bit LSB ... x86-64" 或 "... aarch64"。
case "$(file -b "$BIN_SRC" 2>/dev/null)" in
    *x86-64*)   ARCH="x64" ;;
    *aarch64*|*arm64*) ARCH="arm64" ;;
    *)          ARCH="$(uname -m)" ;;
esac
# 仅当 DIST_NAME 是默认值（未整体覆盖）时才追加架构后缀。
[[ "$DIST_NAME" == seimi-render-${APP_VERSION}-linux ]] && DIST_NAME="${DIST_NAME}-${ARCH}"
DIST_DIR="$DIST_ROOT/$DIST_NAME"

# ---- 2. 准备 bundle 目录结构 ----
echo "== [2/5] assemble bundle layout =="
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/bin" "$DIST_DIR/lib" \
         "$DIST_DIR/libexec" \
         "$DIST_DIR/plugins/platforms" \
         "$DIST_DIR/resources" \
         "$DIST_DIR/translations"

# 主程序
cp "$BIN_SRC" "$DIST_DIR/bin/seimi-render"

# 管理 UI 静态资源（随二进制分发，目标机免额外文件）
# 必须放在主二进制同级（$DIST_DIR/bin/admin-ui）：main.cpp 用 applicationDirPath()+"/admin-ui"
# 查找，applicationDirPath() 返回 binary 所在的 bin/。放包根会找不到 → 根路由 fallback 成
# health JSON，管理界面不出现。对齐 macOS package.sh 的做法（Contents/MacOS/admin-ui）。
if [[ -d "$ROOT_DIR/admin-ui" ]]; then
    cp -r "$ROOT_DIR/admin-ui" "$DIST_DIR/bin/admin-ui"
    echo "  bundled admin-ui/ -> bin/admin-ui (binary sibling, for main.cpp lookup)"
fi

# 运行时加载的 JS（必须放 binary 同级 third_party/，各加载点统一按
# applicationDirPath()/third_party/<sub>/<file> 查找，对齐 macOS package.sh）：
#   - stealth/stealth.js    : 浏览器指纹统一（StealthProfile::loadStealthJs）。缺失会 WARNING
#                              并禁用指纹统一 → 反爬检测可能触发（Google 4xx 等）。
#   - readability/extract.js : md_algorithm=readability 正文提取。缺失静默降级。
#   - readability/simplify.js: md_algorithm=conservative（默认）DOM 简化。缺失静默降级。
#   - readability/Readability.js: extract.js 依赖（Mozilla Readability 主体）。
#   - serp/*.js             : 搜索引擎结果页结构化提取（baidu/bing/google）。缺失静默降级。
mkdir -p "$DIST_DIR/bin/third_party"
for sub in stealth readability serp; do
    if [[ -d "$ROOT_DIR/third_party/$sub" ]]; then
        cp -r "$ROOT_DIR/third_party/$sub" "$DIST_DIR/bin/third_party/"
        echo "  bundled third_party/$sub/ -> bin/third_party/$sub/"
    else
        echo "  [WARN] third_party/$sub not found in source tree" >&2
    fi
done

# 字体：WebEngine 找不到 lib/fonts 会警告且中文渲染成方块。
# 拷一份 Noto CJK 中文字体进去，保证目标机无需另装字体即可渲染中文。
mkdir -p "$DIST_DIR/lib/fonts"
for f in /usr/share/fonts/opentype/noto/NotoSansCJK*.ttc \
         /usr/share/fonts/truetype/noto/NotoSansCJK*.ttc \
         /usr/share/fonts/opentype/noto/NotoSerifCJK*.ttc; do
    if [[ -f "$f" ]]; then
        cp "$f" "$DIST_DIR/lib/fonts/" 2>/dev/null
    fi
done
FONT_COUNT=$(find "$DIST_DIR/lib/fonts" -name '*.ttc' -o -name '*.ttf' 2>/dev/null | wc -l)
if [[ $FONT_COUNT -gt 0 ]]; then
    echo "  bundled $FONT_COUNT font file(s) into lib/fonts/"
else
    echo "  [WARN] no CJK font found to bundle; target may need fonts-noto-cjk" >&2
fi

# ---- 3. 递归收集 Qt6 依赖库（到不动点）----
# 收集策略：以「Qt 安装目录内的库」为收集范围，避免误拷系统库。
# 用一个 worklist：主程序 + 已拷进 bundle/lib 的 .so，反复 ldd 直到没有新的 Qt 库。
collect_qt_libs() {
    local target="$1"
    local dest="$DIST_DIR/lib"
    ldd "$target" 2>/dev/null | awk '{print $3}' | while read -r lib; do
        [[ -z "$lib" || ! -f "$lib" ]] && continue
        # 只收 Qt 安装目录下的库（绝对路径以 QT_PREFIX 开头）
        case "$lib" in
            "$QT_PREFIX"/*)
                local name; name="$(basename "$lib")"
                if [[ ! -e "$dest/$name" ]]; then
                    cp -L "$lib" "$dest/$name"
                    # 递归：新拷的库可能还依赖别的 Qt 库
                    collect_qt_libs "$dest/$name"
                fi
                ;;
        esac
    done
}

echo "== [3/5] collect Qt6 shared libs (recursive to fixpoint) =="
collect_qt_libs "$DIST_DIR/bin/seimi-render"
# 兜底：显式把整个 QT_PREFIX/lib 下用到的 Qt6 库过一遍，避免 ldd 漏判
# （libQt6WebEngineCore 等可能被 dlopen 而非直接 NEEDED）。
for so in "$QT_PREFIX"/lib/libQt6*.so.*; do
    [[ -f "$so" ]] || continue
    name="$(basename "$so")"
    [[ -e "$DIST_DIR/lib/$name" ]] || cp -L "$so" "$DIST_DIR/lib/$name"
done

# WebEngine 的辅助库（libQt6WebEngineCore 依赖）也补全
collect_qt_libs "$DIST_DIR/lib/libQt6WebEngineCore.so.6" 2>/dev/null || true

QT_LIB_COUNT=$(find "$DIST_DIR/lib" -maxdepth 1 -name '*.so*' | wc -l | tr -d ' ')
echo "  collected $QT_LIB_COUNT Qt libs into lib/"

# ---- 4. WebEngine 资源 + 子进程 + offscreen 插件 ----
echo "== [4/5] copy WebEngine process + resources + offscreen plugin =="
WE_PROC_SRC="$QT_PREFIX/libexec/QtWebEngineProcess"
if [[ -x "$WE_PROC_SRC" ]]; then
    cp -L "$WE_PROC_SRC" "$DIST_DIR/libexec/QtWebEngineProcess"
    # QtWebEngineProcess 自身也依赖 Qt 库，补一次
    collect_qt_libs "$DIST_DIR/libexec/QtWebEngineProcess" || true
else
    echo "  [WARN] QtWebEngineProcess not found at $WE_PROC_SRC" >&2
fi

# WebEngine 资源：icudtl.dat + *.pak
if [[ -d "$QT_PREFIX/resources" ]]; then
    cp -rL "$QT_PREFIX/resources/." "$DIST_DIR/resources/"
else
    echo "  [WARN] resources/ not found under $QT_PREFIX" >&2
fi
# 语言 pak
if [[ -d "$QT_PREFIX/translations/qtwebengine_locales" ]]; then
    mkdir -p "$DIST_DIR/translations"
    cp -rL "$QT_PREFIX/translations/qtwebengine_locales" "$DIST_DIR/translations/"
else
    echo "  [WARN] qtwebengine_locales not found under $QT_PREFIX/translations" >&2
fi

# offscreen 平台插件（无头渲染必需）
OFFSCREEN_SRC="$QT_PREFIX/plugins/platforms/libqoffscreen.so"
if [[ -f "$OFFSCREEN_SRC" ]]; then
    cp -L "$OFFSCREEN_SRC" "$DIST_DIR/plugins/platforms/libqoffscreen.so"
    # 顺带拷 cocoa/minimal 等无用，只要 offscreen
else
    echo "  [WARN] libqoffscreen.so not found at $OFFSCREEN_SRC" >&2
fi

# ---- 5. patchelf RPATH + wrapper + tar ----
# wrapper 必须用 POSIX sh（#!/bin/sh）：它跑在目标机（可能无 bash，如最小容器/Alpine）。
# 本打包脚本本身（package-linux.sh）跑在构建机，可用 bash 特性；二者环境不同。
echo "== [5/5] patchelf RPATH + wrapper + tar =="
patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/bin/seimi-render"
[[ -f "$DIST_DIR/libexec/QtWebEngineProcess" ]] && \
    patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/libexec/QtWebEngineProcess"

# 生成 wrapper 脚本
cat > "$DIST_DIR/seimi-render.sh" <<'WRAPPER'
#!/bin/sh
# seimi-render 自包含分发版启动器（POSIX sh 兼容）。
# 目标机仅需 /bin/sh（最小容器/Alpine 可能无 bash），无需装 bash 即可运行。
# 计算 bundle 根目录，把 Qt/WebEngine 的查找路径都指向 bundle 内部，
# 使目标机无需安装 Qt 即可运行。
#
# POSIX 兼容要点（勿引入 bash 特性，否则目标机无 bash 会报错）：
#   - 不用 pipefail / `set -o`（POSIX sh 无），用 `set -eu`（-e/-u 均为 POSIX）。
#   - 不用 ${BASH_SOURCE[0]}，用 $0（POSIX 经 $0 取脚本路径）。
#   - 不用 [[ ]]；本脚本无复杂条件判断，无需。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

# Qt 库：RPATH 已处理，这里再设 LD_LIBRARY_PATH 作双保险
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# QtWebEngine 资源/子进程：Qt 在 Linux 上经 QLibraryInfo 解析（非相对二进制路径），
# bundle 重定位后必须显式覆盖，否则找不到 resources / QtWebEngineProcess。
export QTWEBENGINE_PROCESS_PATH="$HERE/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$HERE/resources"
export QTWEBENGINE_LOCALES_PATH="$HERE/translations/qtwebengine_locales"

# 平台插件目录（让 Qt 找到 bundle 内的 offscreen 插件）
export QT_PLUGIN_PATH="$HERE/plugins"

exec "$HERE/bin/seimi-render" "$@"
WRAPPER
chmod +x "$DIST_DIR/seimi-render.sh"

# ---- 验证 bundle 关键资源 ----
echo ""
echo "  -- verify bundle --"
check() {
    if [[ -e "$1" ]]; then echo "  [OK]   $2"; else echo "  [MISS] $2"; fi
}
check "$DIST_DIR/bin/seimi-render"                  "main binary"
check "$DIST_DIR/bin/admin-ui/index.html"           "admin UI index.html"
check "$DIST_DIR/bin/third_party/stealth/stealth.js"      "stealth.js (fingerprint)"
check "$DIST_DIR/bin/third_party/readability/simplify.js" "simplify.js (markdown)"
check "$DIST_DIR/bin/third_party/readability/extract.js"  "extract.js (markdown)"
check "$DIST_DIR/libexec/QtWebEngineProcess"        "QtWebEngineProcess"
check "$DIST_DIR/resources/icudtl.dat"             "icudtl.dat"
check "$DIST_DIR/plugins/platforms/libqoffscreen.so" "offscreen platform plugin"
check "$DIST_DIR/seimi-render.sh"                   "wrapper script"

# 分发安全护栏：校验主二进制 RPATH + bundle 内无缺失依赖。
# 这是「打包后分发到别的机器找不到依赖」的根因检测，对齐 macOS package.sh 的护栏。
#   1) 主二进制 RPATH 必须是 $ORIGIN/../lib（patchelf 设的，否则分发版 dyld 找不到 lib/）。
#   2) 在 wrapper 的运行环境（LD_LIBRARY_PATH=bundle/lib + WebEngine 路径）下 ldd，
#      关键二进制不得有 "not found"。用 wrapper 同款环境变量，确保校验 = 实际运行条件。
#      这也是验证 wrapper 本身没写错路径的兜底。
echo "  -- main binary RPATH (must contain \$ORIGIN/../lib) --"
MAIN_RPATH="$(patchelf --print-rpath "$DIST_DIR/bin/seimi-render" 2>/dev/null || true)"
if [[ "$MAIN_RPATH" == *'$ORIGIN/../lib'* ]]; then
    echo "  [OK]   main binary RPATH = $MAIN_RPATH"
else
    echo "  [FATAL] main binary RPATH 缺少 \$ORIGIN/../lib (当前: '$MAIN_RPATH')"
    echo "          分发到其他机器会找不到 lib/ 下的 Qt 库。请重跑打包。"
    exit 1
fi

echo "  -- ldd check (no 'not found' allowed, in wrapper env) --"
LDD_FAIL=0
# 用子 shell 隔离环境变量，校验完不影响后续步骤。
(
    export LD_LIBRARY_PATH="$DIST_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export QTWEBENGINE_PROCESS_PATH="$DIST_DIR/libexec/QtWebEngineProcess"
    export QTWEBENGINE_RESOURCES_PATH="$DIST_DIR/resources"
    export QTWEBENGINE_LOCALES_PATH="$DIST_DIR/translations/qtwebengine_locales"
    export QT_PLUGIN_PATH="$DIST_DIR/plugins"
    for tgt in "$DIST_DIR/bin/seimi-render" "$DIST_DIR/libexec/QtWebEngineProcess"; do
        [[ -x "$tgt" ]] || continue
        if ldd "$tgt" 2>/dev/null | grep -q 'not found'; then
            echo "  [FATAL] $tgt 有缺失依赖:" >&2
            ldd "$tgt" | grep 'not found' >&2
            exit 1
        fi
    done
) || LDD_FAIL=1
if [[ "$LDD_FAIL" == "1" ]]; then
    echo "  [FATAL] bundle 内存在缺失依赖，分发版会启动失败。" >&2
    echo "          常见原因：collect_qt_libs 漏拷（Qt 升级后路径变）；系统库（glibc/libnss3 等）目标机缺失。" >&2
    exit 1
fi
echo "  [OK]   main binary + QtWebEngineProcess deps all resolved"

# ---- 打 tar ----
TAR_PATH="$BUILD_DIR/$DIST_NAME.tar.gz"
echo "  -- packaging tar --"
tar -czf "$TAR_PATH" -C "$DIST_ROOT" "$DIST_NAME"

DIST_SIZE=$(du -sh "$DIST_DIR" | awk '{print $1}')
TAR_SIZE=$(du -sh "$TAR_PATH" | awk '{print $1}')
echo ""
echo "== DONE =="
echo "  bundle   : $DIST_DIR   ($DIST_SIZE)"
echo "  tar      : $TAR_PATH   ($TAR_SIZE)"
echo ""
echo "  目标机解压后运行（WSL2/容器/root 下加 --no-sandbox）:"
echo "    tar xzf $DIST_NAME.tar.gz"
echo "    ./$DIST_NAME/seimi-render.sh --no-sandbox --http-port 8088 --ws-port 8089"
