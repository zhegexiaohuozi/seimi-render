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
    echo "== [0/6] clean build dir =="
    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"
        [[ -d "$BUILD_DIR" ]] && { echo "ERROR: rm -rf 失败（可能被进程占用）" >&2; exit 1; }
        echo "  build dir removed."
    else
        echo "  build dir absent, nothing to clean."
    fi
fi

# ---- 1. 构建 ELF ----
echo "== [1/6] build ELF =="
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
echo "== [2/6] assemble bundle layout =="
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
# Markdown conversion is required at runtime; fail packaging if a dependency is missing.
mkdir -p "$DIST_DIR/bin/third_party/markdown"
for file in convert.js turndown.js turndown-plugin-gfm.js LICENSE.turndown LICENSE.turndown-plugin-gfm; do
    cp "$ROOT_DIR/third_party/markdown/$file" "$DIST_DIR/bin/third_party/markdown/"
done
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

echo "== [3/6] collect Qt6 shared libs (recursive to fixpoint) =="
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
echo "== [4/6] copy WebEngine process + resources + offscreen plugin =="
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

# 说明：--gui 导航图标为 qrc 内 Font Awesome SVG，经 C++ FaIconProvider
# （QSvgRenderer 直连渲染，Qt6::Svg 随主二进制链接）光栅化——不依赖
# imageformats/qsvg 插件，也无需 QtQuick/Effects 模块；libQt6Svg.so 由
# 下方 libQt6* 全量兜底拷贝覆盖。

# ---- 5. QML 模块（--gui 控制台运行时 import 解析）----
# QML 源码编在 qrc 里，但 import 的模块实现（QtQuick/Controls/Layouts 的 qmldir+插件）
# 运行时按 import path 找 —— 漏拷 = 分发版 --gui 白屏，构建期无任何报错（Linux 特有坑）。
# 双保险：qmlimportscanner 按源码 import 精确收集 + 静态必备清单兜底并集 + 打包后加载冒烟。
echo "== [5/6] collect QML modules (scanner + static union) =="

QML_IMPORT_ROOT="$QT_PREFIX/qml"
if [[ ! -d "$QML_IMPORT_ROOT" ]]; then
    for cand in "$QT_PREFIX/lib/qt6/qml" "$QT_PREFIX/share/qt/qml" /usr/lib/qt6/qml; do
        [[ -d "$cand" ]] && QML_IMPORT_ROOT="$cand" && break
    done
fi
if [[ ! -d "$QML_IMPORT_ROOT" ]]; then
    echo "  [FATAL] QML import root not found (looked under $QT_PREFIX)" >&2
    exit 1
fi

QMLIMPORTSCANNER="$QT_PREFIX/bin/qmlimportscanner"
if [[ ! -x "$QMLIMPORTSCANNER" ]]; then
    for cand in "$QT_PREFIX/lib/qt6/bin/qmlimportscanner" "$QT_PREFIX/libexec/qmlimportscanner" /usr/lib/qt6/bin/qmlimportscanner; do
        [[ -x "$cand" ]] && QMLIMPORTSCANNER="$cand" && break
    done
fi

QML_LIST="$BUILD_DIR/qml-modules.tsv"
: > "$QML_LIST"
QML_DECLARED=""
if [[ -x "$QMLIMPORTSCANNER" ]] && command -v python3 >/dev/null 2>&1; then
    "$QMLIMPORTSCANNER" -rootPath "$ROOT_DIR/qml" -importPath "$QML_IMPORT_ROOT" \
        > "$BUILD_DIR/qml-modules.json" 2>/dev/null || true
    while IFS=$'\t' read -r rel src; do
        [[ -z "$rel" || -z "$src" ]] && continue
        printf '%s\t%s\n' "$rel" "$src" >> "$QML_LIST"
        QML_DECLARED="$QML_DECLARED $rel "
    done < <(python3 - "$BUILD_DIR/qml-modules.json" <<'PY' || true
import json, os, sys
try:
    mods = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    mods = []
seen = set()
for m in mods if isinstance(mods, list) else []:
    p = m.get("path", "")
    if not p or not os.path.isdir(p):
        continue
    rel = m.get("name") or os.path.basename(p.rstrip("/"))
    if rel in seen:
        continue
    seen.add(rel)
    print(f"{rel}\t{p}")
PY
    )
else
    echo "  [INFO] qmlimportscanner/python3 unavailable; static list only"
fi

# 静态必备并集：Material 样式等由 Controls 运行时动态加载，不出现在源码 import 里，
# scanner 扫不到；Templates/Window 是 Controls 的 QML 级依赖。整目录拷贝天然带上样式。
for mod in QtQml QtQuick QtQuick/Controls QtQuick/Templates QtQuick/Layouts QtQuick/Window QtQuick/Dialogs QtQuick/Dialogs/quickimpl; do
    case " $QML_DECLARED " in *" $mod "*) continue ;; esac
    src="$QML_IMPORT_ROOT/$mod"
    [[ -d "$src" ]] || continue
    printf '%s\t%s\n' "$mod" "$src" >> "$QML_LIST"
done

mkdir -p "$DIST_DIR/qml"
QML_MODULE_COUNT=0
while IFS=$'\t' read -r rel src; do
    [[ -z "$rel" || -z "$src" ]] && continue
    dest="$DIST_DIR/qml/$rel"
    if [[ ! -d "$dest" ]]; then
        mkdir -p "$dest"
        cp -rL "$src/." "$dest/"
        QML_MODULE_COUNT=$((QML_MODULE_COUNT + 1))
    fi
done < "$QML_LIST"
rm -f "$QML_LIST" "$BUILD_DIR/qml-modules.json"

# QML 插件 .so 的 Qt 依赖也进 lib/（运行时靠 wrapper 的 LD_LIBRARY_PATH 解析）。
find "$DIST_DIR/qml" -type f -name '*.so' 2>/dev/null | while read -r qso; do
    collect_qt_libs "$qso"
done
echo "  bundled $QML_MODULE_COUNT QML module(s) -> qml/"
[[ -d "$DIST_DIR/qml/QtQuick/Controls" ]] \
    && echo "  [OK]   QtQuick/Controls present" \
    || { echo "  [FATAL] QtQuick/Controls missing -- --gui would be blank" >&2; exit 1; }

# ---- 5b. patchelf RPATH + wrapper + qt.conf ----
echo "== [5b/6] patchelf RPATH + wrapper + qt.conf =="
patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/bin/seimi-render"
[[ -f "$DIST_DIR/libexec/QtWebEngineProcess" ]] && \
    patchelf --set-rpath '$ORIGIN/../lib' "$DIST_DIR/libexec/QtWebEngineProcess"

# wrapper 脚本：把 Qt/WebEngine/QML 查找路径都指向 bundle 内部。
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

# --gui 控制台的 QML 模块（QtQuick/Controls 等）。
export QML2_IMPORT_PATH="$HERE/qml"
export QML_IMPORT_PATH="$HERE/qml"

exec "$HERE/bin/seimi-render" "$@"
WRAPPER
chmod +x "$DIST_DIR/seimi-render.sh"

# qt.conf：QLibraryInfo 层兜底（QML2_IMPORT_PATH 之外的第二条解析路径；双保险，
# 防未来 Qt 版本对 env 变量语义的变更）。相对路径以 bin/seimi-render 所在目录为基准。
cat > "$DIST_DIR/bin/qt.conf" <<'QTC'
[Paths]
Plugins = ../plugins
Qml2Imports = ../qml
QTC

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
check "$DIST_DIR/qml/QtQuick/Controls"                   "QML QtQuick.Controls"
check "$DIST_DIR/bin/qt.conf"                            "qt.conf"
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
# QML 插件抽前 3 个代表（全量在 collect_qt_libs 已按 ldd 不动点收齐依赖）。
echo "  -- ldd check (no 'not found' allowed) --"
LDD_FAIL=0
(
    export LD_LIBRARY_PATH="$DIST_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export QTWEBENGINE_PROCESS_PATH="$DIST_DIR/libexec/QtWebEngineProcess"
    export QTWEBENGINE_RESOURCES_PATH="$DIST_DIR/resources"
    export QTWEBENGINE_LOCALES_PATH="$DIST_DIR/translations/qtwebengine_locales"
    export QT_PLUGIN_PATH="$DIST_DIR/plugins"
    export QML2_IMPORT_PATH="$DIST_DIR/qml"
    LDD_TARGETS="$DIST_DIR/bin/seimi-render $DIST_DIR/libexec/QtWebEngineProcess"
    LDD_TARGETS="$LDD_TARGETS $(find "$DIST_DIR/qml" -type f -name '*.so' 2>/dev/null | head -3)"
    for tgt in $LDD_TARGETS; do
        [[ -x "$tgt" || -f "$tgt" ]] || continue
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

# ---- GUI QML 加载冒烟（打包机执行；防「仅 Linux 白屏」的最后一道闸）----
# offscreen 平台 + Qt Quick 软件渲染：构建机无显示也可跑。判定三重：
#   1) 进程 12s 后仍存活（QML 崩溃/缺模块通常秒退）
#   2) 日志无 "failed to load component"/"FATAL"
#   3) curl 可用时 /health 探活
# SKIP_GUI_SMOKE=1 跳过（无显示且字体缺失的环境可能误报，留逃生口）。
if [[ "${SKIP_GUI_SMOKE:-0}" == "1" ]]; then
    echo "  -- GUI QML smoke SKIPPED (SKIP_GUI_SMOKE=1) --"
else
    echo "  -- GUI QML load smoke (offscreen + software renderer) --"
    SMOKE_LOG="$(mktemp)"
    SMOKE_RC=0
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        "$DIST_DIR/seimi-render.sh" --gui --http-port 18099 --ws-port 18098 \
        --mcp-port 18097 --no-warmup >"$SMOKE_LOG" 2>&1 &
    SMOKE_PID=$!
    sleep 12
    if ! kill -0 "$SMOKE_PID" 2>/dev/null; then
        echo "  [FATAL] GUI 冒烟进程已退出（疑似 QML 模块缺失/崩溃）：" >&2
        tail -20 "$SMOKE_LOG" >&2 || true
        SMOKE_RC=1
    elif grep -qiE "failed to load component|FATAL" "$SMOKE_LOG"; then
        echo "  [FATAL] GUI 冒烟日志含加载失败：" >&2
        grep -iE "failed to load component|FATAL" "$SMOKE_LOG" | head -5 >&2
        SMOKE_RC=1
    elif command -v curl >/dev/null 2>&1; then
        if curl -sf -m 5 "http://127.0.0.1:18099/health" >/dev/null 2>&1; then
            echo "  [OK]   GUI console loaded, /health alive"
        else
            echo "  [FATAL] /health 探活失败（进程活着但服务未起）" >&2
            tail -10 "$SMOKE_LOG" >&2 || true
            SMOKE_RC=1
        fi
    else
        echo "  [OK]   GUI console process alive (curl unavailable, health skipped)"
    fi
    kill "$SMOKE_PID" 2>/dev/null || true
    wait "$SMOKE_PID" 2>/dev/null || true
    rm -f "$SMOKE_LOG"
    if [[ "$SMOKE_RC" != "0" ]]; then
        echo "  [FATAL] GUI QML 冒烟未通过；勿分发此包（可 SKIP_GUI_SMOKE=1 强制跳过）。" >&2
        exit 1
    fi
fi

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
