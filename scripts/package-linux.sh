#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render Linux 自包含打包脚本（对标 macOS package.sh）
#
# linuxdeployqt 已弃用且不支持 Qt6，社区标准做法是手动 bundle：
#   1) build-linux.sh 编裸 ELF
#   2) ldd 递归抽取 Qt6 .so（到不动点）→ bundle/lib/
#   3) 拷 QtWebEngineProcess + resources + locales + offscreen 插件
#   4) patchelf 设 RPATH=$ORIGIN/../lib
#   5) 生成 wrapper 脚本
#   6) tar 打包
#
# 系统库（glibc/libnss3/libstdc++/libGL）不打包——目标机需自行满足（见 README）。
# 用法: bash scripts/package-linux.sh [clean] [Release|Debug]
# 环境变量: QT_PREFIX, BUILD_DIR, BUILD_TYPE, DIST_NAME
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

CONFIG=""
DO_CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        clean) DO_CLEAN=1 ;;
        Release|Debug|RelWithDebInfo|MinSizeRel) CONFIG="$1" ;;
        *)
            echo "ERROR: 未知参数 '$1'（应为 clean | Release | Debug | RelWithDebInfo | MinSizeRel）" >&2
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

# 产物名带版本 + 架构（对齐 macOS package.sh）。
# 版本从 CMakeLists.txt 文本解析；架构在构建后从主二进制 ELF 头读（避免 uname 骗人）。
APP_VERSION="$(tr '\n' ' ' < "$ROOT_DIR/CMakeLists.txt" \
    | grep -oE 'project\([^)]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' \
    | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[[ -z "$APP_VERSION" ]] && APP_VERSION="unknown"
DIST_NAME="${DIST_NAME:-seimi-render-${APP_VERSION}-linux}"
DIST_DIR="$DIST_ROOT/$DIST_NAME"

echo "== seimi-render packaging (Linux) =="
echo "  root      : $ROOT_DIR"
echo "  config    : $CONFIG"
echo "  qt prefix : $QT_PREFIX"
echo "  dist dir  : $DIST_DIR"

# 前置：patchelf + ldd
command -v patchelf >/dev/null 2>&1 || { echo "ERROR: patchelf not found. sudo apt-get install -y patchelf" >&2; exit 1; }
command -v ldd >/dev/null 2>&1 || { echo "ERROR: ldd not found." >&2; exit 1; }

# ---- 0. (可选) clean ----
if [[ "$DO_CLEAN" == "1" ]]; then
    echo "== [0/5] clean build dir =="
    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"
        [[ -d "$BUILD_DIR" ]] && { echo "ERROR: rm -rf 失败（可能被进程占用）" >&2; exit 1; }
        echo "  build dir removed."
    else
        echo "  build dir absent, nothing to clean."
    fi
fi

# ---- 1. 构建 ELF ----
echo "== [1/5] build ELF =="
"$SCRIPT_DIR/build-linux.sh" "$CONFIG"

BIN_SRC="$BUILD_DIR/seimi-render"
if [[ ! -x "$BIN_SRC" ]]; then
    echo "ERROR: binary not found at $BIN_SRC" >&2
    exit 1
fi

# 从主二进制 ELF 头读架构（比 uname 可靠：32 位容器/交叉编译不骗人）。
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

cp "$BIN_SRC" "$DIST_DIR/bin/seimi-render"

# 管理 UI（必须放主二进制同级 bin/admin-ui：main.cpp 按 applicationDirPath()/admin-ui 查找）
if [[ -d "$ROOT_DIR/admin-ui" ]]; then
    cp -r "$ROOT_DIR/admin-ui" "$DIST_DIR/bin/admin-ui"
    echo "  bundled admin-ui/ -> bin/admin-ui"
fi

# 运行时 JS（放 binary 同级 third_party/，各加载点统一按 applicationDirPath()/third_party/<sub> 查找）
mkdir -p "$DIST_DIR/bin/third_party"
for sub in stealth readability serp; do
    if [[ -d "$ROOT_DIR/third_party/$sub" ]]; then
        cp -r "$ROOT_DIR/third_party/$sub" "$DIST_DIR/bin/third_party/"
        echo "  bundled third_party/$sub/"
    else
        echo "  [WARN] third_party/$sub not found in source tree" >&2
    fi
done

# CJK 字体（WebEngine 找不到 lib/fonts 会警告且中文渲染成方块）
mkdir -p "$DIST_DIR/lib/fonts"
for f in /usr/share/fonts/opentype/noto/NotoSansCJK*.ttc \
         /usr/share/fonts/truetype/noto/NotoSansCJK*.ttc \
         /usr/share/fonts/opentype/noto/NotoSerifCJK*.ttc; do
    [[ -f "$f" ]] && cp "$f" "$DIST_DIR/lib/fonts/" 2>/dev/null
done
FONT_COUNT=$(find "$DIST_DIR/lib/fonts" -name '*.ttc' -o -name '*.ttf' 2>/dev/null | wc -l)
if [[ $FONT_COUNT -gt 0 ]]; then
    echo "  bundled $FONT_COUNT font file(s)"
else
    echo "  [WARN] no CJK font found; target may need fonts-noto-cjk" >&2
fi

# ---- 3. 递归收集 Qt6 依赖库（到不动点）----
# 只收 Qt 安装目录下的库，避免误拷系统库。worklist 反复 ldd 直到无新 Qt 库。
collect_qt_libs() {
    local target="$1"
    local dest="$DIST_DIR/lib"
    ldd "$target" 2>/dev/null | awk '{print $3}' | while read -r lib; do
        [[ -z "$lib" || ! -f "$lib" ]] && continue
        case "$lib" in
            "$QT_PREFIX"/*)
                local name; name="$(basename "$lib")"
                if [[ ! -e "$dest/$name" ]]; then
                    cp -L "$lib" "$dest/$name"
                    collect_qt_libs "$dest/$name"
                fi
                ;;
        esac
    done
}

echo "== [3/5] collect Qt6 shared libs (recursive to fixpoint) =="
collect_qt_libs "$DIST_DIR/bin/seimi-render"
# 兜底：显式过一遍整个 QT_PREFIX/lib（libQt6WebEngineCore 等可能被 dlopen 而非 NEEDED）
for so in "$QT_PREFIX"/lib/libQt6*.so.*; do
    [[ -f "$so" ]] || continue
    name="$(basename "$so")"
    [[ -e "$DIST_DIR/lib/$name" ]] || cp -L "$so" "$DIST_DIR/lib/$name"
done
collect_qt_libs "$DIST_DIR/lib/libQt6WebEngineCore.so.6" 2>/dev/null || true

QT_LIB_COUNT=$(find "$DIST_DIR/lib" -maxdepth 1 -name '*.so*' | wc -l | tr -d ' ')
echo "  collected $QT_LIB_COUNT Qt libs"

# ---- 4. WebEngine 资源 + 子进程 + offscreen 插件 ----
echo "== [4/5] copy WebEngine process + resources + offscreen plugin =="
WE_PROC_SRC="$QT_PREFIX/libexec/QtWebEngineProcess"
if [[ -x "$WE_PROC_SRC" ]]; then
    cp -L "$WE_PROC_SRC" "$DIST_DIR/libexec/QtWebEngineProcess"
    collect_qt_libs "$DIST_DIR/libexec/QtWebEngineProcess" || true
else
    echo "  [WARN] QtWebEngineProcess not found at $WE_PROC_SRC" >&2
fi

[[ -d "$QT_PREFIX/resources" ]] && cp -rL "$QT_PREFIX/resources/." "$DIST_DIR/resources/" \
    || echo "  [WARN] resources/ not found under $QT_PREFIX" >&2

if [[ -d "$QT_PREFIX/translations/qtwebengine_locales" ]]; then
    mkdir -p "$DIST_DIR/translations"
    cp -rL "$QT_PREFIX/translations/qtwebengine_locales" "$DIST_DIR/translations/"
else
    echo "  [WARN] qtwebengine_locales not found" >&2
fi

# offscreen 平台插件（无头渲染必需）
OFFSCREEN_SRC="$QT_PREFIX/plugins/platforms/libqoffscreen.so"
if [[ -f "$OFFSCREEN_SRC" ]]; then
    cp -L "$OFFSCREEN_SRC" "$DIST_DIR/plugins/platforms/libqoffscreen.so"
else
    echo "  [WARN] libqoffscreen.so not found" >&2
fi

# ---- 5. patchelf RPATH + wrapper + tar ----
# wrapper 用 POSIX sh：目标机可能无 bash（最小容器/Alpine）。
echo "== [5/5] patchelf RPATH + wrapper + tar =="
patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/bin/seimi-render"
[[ -f "$DIST_DIR/libexec/QtWebEngineProcess" ]] && \
    patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/libexec/QtWebEngineProcess"

# wrapper 脚本：把 Qt/WebEngine 查找路径都指向 bundle 内部。
# POSIX 兼容：不用 pipefail/${BASH_SOURCE}/[[ ]]（目标机可能无 bash）。
cat > "$DIST_DIR/seimi-render.sh" <<'WRAPPER'
#!/bin/sh
# seimi-render 自包含分发版启动器（POSIX sh 兼容）。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# QtWebEngine 经 QLibraryInfo 解析（非相对二进制路径），bundle 重定位后必须显式覆盖。
export QTWEBENGINE_PROCESS_PATH="$HERE/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$HERE/resources"
export QTWEBENGINE_LOCALES_PATH="$HERE/translations/qtwebengine_locales"
export QT_PLUGIN_PATH="$HERE/plugins"

exec "$HERE/bin/seimi-render" "$@"
WRAPPER
chmod +x "$DIST_DIR/seimi-render.sh"

# ---- 验证 ----
echo ""
echo "  -- verify bundle --"
check() {
    [[ -e "$1" ]] && echo "  [OK]   $2" || echo "  [MISS] $2"
}
check "$DIST_DIR/bin/seimi-render"                       "main binary"
check "$DIST_DIR/bin/admin-ui/index.html"                "admin UI"
check "$DIST_DIR/bin/third_party/stealth/stealth.js"     "stealth.js"
check "$DIST_DIR/bin/third_party/readability/simplify.js" "simplify.js"
check "$DIST_DIR/bin/third_party/readability/extract.js"  "extract.js"
check "$DIST_DIR/libexec/QtWebEngineProcess"             "QtWebEngineProcess"
check "$DIST_DIR/resources/icudtl.dat"                   "icudtl.dat"
check "$DIST_DIR/plugins/platforms/libqoffscreen.so"     "offscreen plugin"
check "$DIST_DIR/seimi-render.sh"                        "wrapper script"

# 分发护栏：主二进制 RPATH 必须是 $ORIGIN/../lib（否则分发版找不到 lib/）。
echo "  -- main binary RPATH (must contain \$ORIGIN/../lib) --"
MAIN_RPATH="$(patchelf --print-rpath "$DIST_DIR/bin/seimi-render" 2>/dev/null || true)"
if [[ "$MAIN_RPATH" == *'$ORIGIN/../lib'* ]]; then
    echo "  [OK]   RPATH = $MAIN_RPATH"
else
    echo "  [FATAL] RPATH 缺少 \$ORIGIN/../lib (当前: '$MAIN_RPATH')" >&2
    exit 1
fi

# 在 wrapper 同款环境下 ldd，关键二进制不得有 "not found"。
echo "  -- ldd check (no 'not found' allowed) --"
LDD_FAIL=0
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
    echo "  [FATAL] bundle 内存在缺失依赖。" >&2
    exit 1
fi
echo "  [OK]   deps all resolved"

# ---- 打 tar ----
TAR_PATH="$BUILD_DIR/$DIST_NAME.tar.gz"
echo "  -- packaging tar --"
tar -czf "$TAR_PATH" -C "$DIST_ROOT" "$DIST_NAME"

DIST_SIZE=$(du -sh "$DIST_DIR" | awk '{print $1}')
TAR_SIZE=$(du -sh "$TAR_PATH" | awk '{print $1}')
echo ""
echo "== DONE =="
echo "  bundle : $DIST_DIR   ($DIST_SIZE)"
echo "  tar    : $TAR_PATH   ($TAR_SIZE)"
echo ""
echo "  目标机解压后运行（WSL2/容器/root 下加 --no-sandbox）:"
echo "    tar xzf $DIST_NAME.tar.gz"
echo "    ./$DIST_NAME/seimi-render.sh --no-sandbox --http-port 8088 --ws-port 8089"
