#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render 打包脚本（macOS）：产出可独立分发的 .app bundle + zip。
#
# 用法:
#   scripts/package.sh                 # 默认 Release，增量
#   scripts/package.sh Debug
#   scripts/package.sh clean           # 清空 build 后全量重建（默认 Release）
#   scripts/package.sh clean Debug     # clean 可与配置组合，顺序不限
# 环境变量:
#   QT_PREFIX     Qt 安装前缀（默认自动探测 homebrew qt）
#   FORCE_DEPLOY=1 强制重跑 macdeployqt（跳过指纹缓存）
#   NO_ZIP=1      跳过 zip 打包
#   ZIP_DIR=...   zip 输出目录（默认 build/）
# 前置: 已安装 Qt6 (brew install qt)。
# 兼容：sh 调用时自动 exec bash（脚本用了 pipefail、[[ ]]、进程替换等 bash 特性；
# 以 sh 名字启动的 bash 会进 POSIX 模式禁用这些扩展，必须 exec 成真 bash）。
case "${0##*/}" in
    sh|-sh|sh.exe) exec bash "$0" "$@" ;;
esac
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

# 参数解析：clean + CONFIG 任意顺序组合；未知参数直接报错（历史上 CONFIG=clean
# 一路传给 cmake 跳过 macdeployqt，产出带绝对路径的不可分发包）。
CONFIG=""
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        clean) CLEAN=1 ;;
        Release|Debug|RelWithDebInfo|MinSizeRel) CONFIG="$1" ;;
        *)
            echo "ERROR: 未知参数 '$1'（应为 clean | Release | Debug | RelWithDebInfo | MinSizeRel）" >&2
            exit 1
            ;;
    esac
    shift
done
[[ -z "$CONFIG" ]] && CONFIG="Release"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BUNDLE_APP="$BUILD_DIR/seimi-render.app"

# Qt prefix 自动检测：优先 arm64 Homebrew (/opt/homebrew)，回退 Intel (/usr/local)。
# macdeployqt 架构必须和机器一致，否则 arm64 Mac 上跑 x86_64 macdeployqt 会全程
# Rosetta 翻译，[2/3] 步骤从几秒拖到 3-5 分钟（看似卡死）。
if [[ -z "${QT_PREFIX:-}" ]]; then
    if [[ -d /opt/homebrew/opt/qt ]]; then
        QT_PREFIX="/opt/homebrew/opt/qt"
    else
        QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo /usr/local/opt/qt)"
    fi
fi
MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"

echo "== seimi-render packaging =="
echo "  root      : $ROOT_DIR"
echo "  config    : $CONFIG"
echo "  qt prefix : $QT_PREFIX"
if [[ "$CLEAN" == "1" ]]; then
    echo "  mode      : clean (full rebuild)"
else
    echo "  mode      : incremental"
fi

if [[ ! -x "$MACDEPLOYQT" ]]; then
    echo "ERROR: macdeployqt not found at $MACDEPLOYQT" >&2
    exit 1
fi

# 1. 构建 bundle（MACOSX_BUNDLE 生成 .app）
if [[ "$CLEAN" == "1" && -d "$BUILD_DIR" ]]; then
    echo "== [0/3] clean build dir =="
    rm -rf "$BUILD_DIR"
fi
echo "== [1/3] cmake build ($CONFIG) =="
cmake -G Ninja -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" --target seimi-render

if [[ ! -d "$BUNDLE_APP" ]]; then
    echo "ERROR: bundle not produced at $BUNDLE_APP" >&2
    exit 1
fi

# 2. macdeployqt：拷贝 Qt 框架 + 插件 + WebEngine 资源。
#    不加 -codesign：含第三方 dylib 的 WebEngine bundle 做 ad-hoc 签名时，macdeployqt
#    会因子组件未签中断，主二进制残留未签名 → cocoa QApplication 被 Gatekeeper 阻塞
#    初始化、进程卡死。改在 [2d] 统一自底向上签名。
#
# === 增量跳过优化 ===
# macdeployqt 即便原生 arm64 也要 ~78s（全量重拷 206MB WebEngine 资源 + 对 23 个框架
# 逐个 fork install_name_tool）。但增量构建时只有主二进制会变，Qt 框架/WebEngine 资源
# 在 Qt 没升级时不变。当指纹一致且主二进制 Qt 依赖已就位，直接跳过——主二进制经 CMake
# 链接已写成 @loader_path/../Frameworks 形态。
# 指纹 = Qt 版本 + bundle 框架清单 + Helper/插件存在性 + Qt 安装目录 mtime。
DEPLOY_CACHE="$BUILD_DIR/.macdeployqt_fingerprint"
compute_qt_fingerprint() {
    local qt_ver fw_list proc_ok offscreen_ok qt_mtime
    qt_ver="$("$QT_PREFIX/bin/qmake6" -query QT_VERSION 2>/dev/null \
              || "$QT_PREFIX/bin/qmake" -query QT_VERSION 2>/dev/null \
              || echo unknown)"
    fw_list="$(ls "$BUNDLE_APP/Contents/Frameworks/" 2>/dev/null \
              | grep '\.framework$' | sort | tr '\n' ',')"
    [[ -f "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess" ]] \
        && proc_ok=1 || proc_ok=0
    [[ -f "$BUNDLE_APP/Contents/PlugIns/platforms/libqoffscreen.dylib" ]] \
        && offscreen_ok=1 || offscreen_ok=0
    qt_mtime="$(stat -f '%m' "$QT_PREFIX/lib/QtCore.framework/QtCore" 2>/dev/null || echo 0)"
    echo "${qt_ver}|${fw_list}|proc=${proc_ok}|offscreen=${offscreen_ok}|mtime=${qt_mtime}"
}

# 主二进制的 Qt 依赖是否全部已就位于 bundle，且不残留构建机绝对路径。
# 返回 0=就位，1=有缺失/残留。
# 用命令替换收集列表再在当前 shell 遍历（管道会开子 shell，return 影响不到调用者）。
main_binary_deps_ready() {
    local main="$BUNDLE_APP/Contents/MacOS/seimi-render" deps dep missing=0
    deps="$(otool -L "$main" 2>/dev/null \
            | grep -oE 'Qt[A-Za-z0-9]+\.framework' | sort -u)"
    for dep in $deps; do
        [[ -z "$dep" ]] && continue
        [[ ! -d "$BUNDLE_APP/Contents/Frameworks/$dep" ]] && { missing=1; break; }
    done
    # 主二进制不得残留构建机绝对路径的 Qt 引用：增量重链接会把 Qt6 keg-only bottle
    # 的 install name 写回成 /opt/homebrew/opt/qtXXX/... 绝对路径。若此时仍命中跳过缓存，
    # 主二进制就带着本机路径被打包分发，别的机器 dyld: Library not loaded。
    # 正则陷阱：历史写成 '/(usr/local|/opt/|Cellar)/' —— /opt/ 分支多余 / 展开成 //opt/，
    # 永远匹配不到单斜杠 /opt/homebrew/...，导致检测失效、macdeployqt 被错误跳过。
    if [[ "$missing" -eq 0 ]] && \
       otool -L "$main" 2>/dev/null | grep -qE '/(usr/local|opt|homebrew|Cellar)/.*(Qt[A-Za-z0-9]+\.framework)'; then
        missing=1
    fi
    return $missing
}

if [[ "$CLEAN" == "1" || "${FORCE_DEPLOY:-0}" == "1" ]]; then
    rm -f "$DEPLOY_CACHE"
    SKIP_DEPLOY=0
else
    CURRENT_FP="$(compute_qt_fingerprint)"
    CACHED_FP="$(cat "$DEPLOY_CACHE" 2>/dev/null || true)"
    if [[ "$CURRENT_FP" == "$CACHED_FP" ]] && main_binary_deps_ready; then
        SKIP_DEPLOY=1
    else
        SKIP_DEPLOY=0
    fi
fi

# 清理上次打包残留的非 Qt 资源（避免 macdeployqt 的 codesign 扫到它们卡死）。
# data/ 含机器绑定的密钥/cookie（seimi.key 是 cookie vault 盐，绝不该进分发版）。
rm -rf "$BUNDLE_APP/Contents/MacOS/admin-ui" 2>/dev/null || true
rm -rf "$BUNDLE_APP/Contents/MacOS/third_party" 2>/dev/null || true
rm -rf "$BUNDLE_APP/Contents/MacOS/data" 2>/dev/null || true

if [[ "$SKIP_DEPLOY" == "1" ]]; then
    echo "== [2/3] macdeployqt SKIPPED (Qt 指纹未变，主二进制依赖已就位) =="
    echo "  [INFO] 增量构建无需重新部署 Qt（FORCE_DEPLOY=1 可强制全量重做）。"
else
    echo "== [2/3] macdeployqt (bundle Qt + WebEngine resources) =="
    if [[ "$(uname -m)" == "arm64" && "$QT_PREFIX" == /usr/local/* ]]; then
        echo "  [INFO] Qt is x86_64 under Rosetta on arm64 Mac; macdeployqt will be slow (~3-5 min)."
    fi

    # 过滤 macdeployqt stderr 的已知噪声（真正的 fatal 错误仍会显示）：
    #   - "Cannot resolve rpath @rpath/Qt(Svg|VirtualKeyboard)..." 是运行时按需 dlopen
    #     的可选模块，不在 LC_LOAD_DYLIB 里，Qt 工具误报 ERROR，部署照常完成。
    MDQ_ERR_TMP="$(mktemp -t seimi_mdq_err.XXXXXX)"
    "$MACDEPLOYQT" "$BUNDLE_APP" \
        -always-overwrite \
        -verbose=1 \
        -no-codesign \
        -executable="$BUNDLE_APP/Contents/MacOS/seimi-render" 2>"$MDQ_ERR_TMP"
    MDQ_RC=$?
    # grep -v 无匹配行时返回 1，被 set -e 误杀，用 || true 吞掉。
    grep -vE '^ERROR: Cannot resolve rpath "@rpath/Qt(Svg|VirtualKeyboard|VirtualKeyboardQml)\.framework' "$MDQ_ERR_TMP" \
        | grep -vE '^ERROR: +using QList' >&2 || true
    if [[ "$MDQ_RC" -ne 0 ]]; then
        echo "[WARN] macdeployqt exited with code $MDQ_RC; full stderr saved: $MDQ_ERR_TMP" >&2
    else
        rm -f "$MDQ_ERR_TMP"
    fi
    compute_qt_fingerprint > "$DEPLOY_CACHE"
fi

# 2a. 管理 UI 静态资源（main.cpp 按 applicationDirPath()/admin-ui 找）
if [[ -d "$ROOT_DIR/admin-ui" ]]; then
    echo "== [2a] bundle admin-ui resources =="
    cp -r "$ROOT_DIR/admin-ui" "$BUNDLE_APP/Contents/MacOS/admin-ui"
fi

# 2a. markdown 算法依赖的 JS（RenderPool 按 applicationDirPath()/third_party/readability/<file> 加载）
if [[ -d "$ROOT_DIR/third_party/readability" ]]; then
    echo "== [2a] bundle readability JS (extract.js, simplify.js) =="
    mkdir -p "$BUNDLE_APP/Contents/MacOS/third_party/readability"
    cp "$ROOT_DIR/third_party/readability/extract.js"  "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
    cp "$ROOT_DIR/third_party/readability/simplify.js" "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
    cp "$ROOT_DIR/third_party/readability/Readability.js" "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
fi

# 2a. stealth.js + serp/*.js（运行时从 binary 同级 third_party/ 加载）
for sub in stealth serp; do
    if [[ -d "$ROOT_DIR/third_party/$sub" ]]; then
        echo "== [2a] bundle $sub JS =="
        mkdir -p "$BUNDLE_APP/Contents/MacOS/third_party/$sub"
        cp "$ROOT_DIR/third_party/$sub/"*.js "$BUNDLE_APP/Contents/MacOS/third_party/$sub/" 2>/dev/null || true
    fi
done

# 2a. offscreen 平台插件：macdeployqt 默认只部署 cocoa（链接期可见），但本项目运行时
#     通过 qputenv("QT_QPA_PLATFORM","offscreen") 做无头渲染，必须手动拷入。
OFFSCREEN_SRC="$QT_PREFIX/share/qt/plugins/platforms/libqoffscreen.dylib"
if [[ -f "$OFFSCREEN_SRC" ]]; then
    echo "== [2a] bundle offscreen platform plugin =="
    mkdir -p "$BUNDLE_APP/Contents/PlugIns/platforms"
    cp "$OFFSCREEN_SRC" "$BUNDLE_APP/Contents/PlugIns/platforms/"
else
    echo "== [2a] WARN: offscreen plugin not found at $OFFSCREEN_SRC =="
    echo "    无头渲染将不可用。请确认 Qt 安装包含 offscreen 插件。"
fi

# 2b. 修复 QtWebEngineProcess 子进程依赖：macdeployqt 不重写它内部的 Qt 框架引用，
#     残留 brew 绝对路径会导致子进程加载到系统 Qt，与 bundle 内 Qt 并存 → 类冲突。
WEBENGINE_PROC="$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
if [[ -f "$WEBENGINE_PROC" ]]; then
    echo "== [2b] fix QtWebEngineProcess library paths -> @rpath =="
    install_name_tool -add_rpath "@loader_path/../../../../../../../Frameworks" "$WEBENGINE_PROC" 2>/dev/null || true

    # 把 otool -L 中 /abs/.../QtXxx.framework/Versions/A/QtXxx 重写为 @rpath/...
    otool -L "$WEBENGINE_PROC" \
        | awk '/Qt[A-Za-z0-9]+\.framework\/Versions\/A\// {print $1}' \
        | while IFS= read -r dep; do
            case "$dep" in
                @rpath/*|@loader_path/*|/System/*|/usr/lib/*) continue ;;
            esac
            fwname=$(echo "$dep" | sed -nE 's|.*/(Qt[A-Za-z0-9]+)\.framework/.*|\1|p')
            [[ -z "$fwname" ]] && continue
            new="@rpath/${fwname}.framework/Versions/A/${fwname}"
            if install_name_tool -change "$dep" "$new" "$WEBENGINE_PROC" 2>/dev/null; then
                echo "    rewrote $fwname -> $new"
            fi
        done

    # 改动令签名失效，重签子进程（不要 --force --deep 整个 bundle，会破坏框架嵌套签名）
    echo "    re-signing QtWebEngineProcess after path fixes..."
    codesign --force --sign - "$WEBENGINE_PROC" 2>/dev/null || true

    if otool -L "$WEBENGINE_PROC" | grep -qE "/(usr/local|/opt/|Cellar)/"; then
        echo "    [WARN] QtWebEngineProcess still has absolute Qt paths"
    else
        echo "    [OK] QtWebEngineProcess deps are @rpath-relative"
    fi
fi

# 2c. bundle-wide 第三方 dylib install_name + rpath 规范化。
#     macdeployqt 只重写 Qt framework 引用，不碰第三方 dylib（brotli/jasper/dbus/jpeg
#     等）——它们的 install_name 残留 /opt/homebrew/... 绝对路径，分发到别的机器 dyld
#     加载失败。这里全量扫描改写为 @rpath 相对。必须在签名（2d）之前。
echo "== [2c] bundle-wide install_name + rpath normalization =="

ABS_RE='/(usr/local|opt/homebrew|Users|Cellar)/'

is_macho() { file -b "$1" 2>/dev/null | grep -q "Mach-O"; }

# 按文件所在目录算出到 Contents/Frameworks 的 @loader_path rpath。
rpath_for_file() {
    case "$1" in
        "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/"*) return 1 ;;
        "$BUNDLE_APP/Contents/MacOS/"*)                   echo "@loader_path/../Frameworks" ;;
        "$BUNDLE_APP/Contents/Frameworks/"*.framework/*)  echo "@loader_path/../../Frameworks" ;;
        "$BUNDLE_APP/Contents/Frameworks/"*)              echo "@loader_path" ;;
        "$BUNDLE_APP/Contents/PlugIns/"*)                 echo "@loader_path/../../Frameworks" ;;
        *)                                                echo "@loader_path/../Frameworks" ;;
    esac
}

# bundle 内已有 Mach-O basename 列表（判断引用目标是否在 bundle 内）。
BUNDLED_BASENAMES_FILE="$(mktemp)"
find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/Versions/A/*" -o -path "*/MacOS/*" \) 2>/dev/null \
    | while IFS= read -r bf; do is_macho "$bf" && basename "$bf"; done \
    | sort -u > "$BUNDLED_BASENAMES_FILE"
trap 'rm -f "$BUNDLED_BASENAMES_FILE"' EXIT

find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/MacOS/*" -o -path "*/Versions/A/*" \) 2>/dev/null \
    | while IFS= read -r f; do
        is_macho "$f" || continue
        bn="$(basename "$f")"

        # 1. 自身 ID（otool -D）：brew 绝对路径 → @rpath/<相对路径>
        #    - Frameworks/*.dylib              → @rpath/<bn>
        #    - Frameworks/*.framework/.../Xxx → @rpath/Xxx.framework/Versions/A/Xxx
        #    （framework ID 标准化：macdeployqt 某些 Qt 版本会漏，残留 keg-only bottle 绝对路径）
        case "$f" in
            "$BUNDLE_APP/Contents/Frameworks/"*.dylib)
                cur_id="$(otool -D "$f" 2>/dev/null | tail -1)"
                if echo "$cur_id" | grep -qE "$ABS_RE"; then
                    install_name_tool -id "@rpath/$bn" "$f" 2>/dev/null && echo "    $bn: id -> @rpath/$bn"
                fi
                ;;
            "$BUNDLE_APP/Contents/Frameworks/"*.framework/Versions/[A-Z]/*)
                cur_id="$(otool -D "$f" 2>/dev/null | tail -1)"
                if echo "$cur_id" | grep -qE "$ABS_RE"; then
                    fw_rel="$(echo "$cur_id" | sed -nE 's#.*/((Qt|lib)[^/]*/Versions/[A-Z]/[^/]*)$#\1#p')"
                    [[ -n "$fw_rel" ]] && install_name_tool -id "@rpath/$fw_rel" "$f" 2>/dev/null \
                        && echo "    $bn: id -> @rpath/$fw_rel"
                fi
                ;;
        esac

        # 2. 依赖引用（otool -L）：第 1 行是文件自身标识，依赖从第 2 行起，故跳 1 行。
        #    （历史 bug：tail -n +3 跳 2 行，漏掉主二进制第 2 行恰好是 brew 路径的依赖）
        otool -L "$f" 2>/dev/null | tail -n +2 | while IFS= read -r line; do
            dep="$(echo "$line" | awk '{print $1}')"
            case "$dep" in
                @rpath/*|@loader_path/*|@executable_path/*|/System/*|/usr/lib/*) continue ;;
            esac
            echo "$dep" | grep -qE "$ABS_RE" || continue
            dep_bn="$(basename "$dep")"
            grep -qx "$dep_bn" "$BUNDLED_BASENAMES_FILE" || continue
            # framework 保留完整路径形态（dyld 按 framework 路径解析），普通 dylib 用 @rpath/<bn>
            case "$dep" in
                *.framework/Versions/*/*) new_dep="@rpath/$(echo "$dep" | sed -E 's#.*/([^/]+\.framework/Versions/[A-Z]/[^/]+)$#\1#')" ;;
                *)                         new_dep="@rpath/$dep_bn" ;;
            esac
            install_name_tool -change "$dep" "$new_dep" "$f" 2>/dev/null && echo "    $bn: dep $dep_bn -> @rpath"
        done

        # 3. 删 brew 绝对路径 rpath，加按位置的 @loader_path rpath
        otool -l "$f" 2>/dev/null | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' \
            | while IFS= read -r rp; do
                case "$rp" in @*) continue ;; esac
                echo "$rp" | grep -qE "$ABS_RE" || continue
                install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null && echo "    $bn: -rpath $rp"
            done
        new_rp="$(rpath_for_file "$f")" || continue
        install_name_tool -add_rpath "$new_rp" "$f" 2>/dev/null || true
    done
echo "  [DONE] install_name/rpath normalized (see per-file lines above)"

# 2d. 自底向上 ad-hoc 签名：先叶子（dylib/可执行），再中间（.app/framework），最后顶层 .app。
#     让未签名主二进制能正常初始化 cocoa（QApplication）。必须在 [2c] 之后（install_name_tool 改动令签名失效）。
echo "== [2d] ad-hoc sign bundle (bottom-up) =="
sign_silent() { codesign --force --sign - "$1" 2>/dev/null && return 0 || return 0; }
find "$BUNDLE_APP/Contents/Frameworks" -type f -name "*.dylib" 2>/dev/null | while read -r f; do sign_silent "$f"; done
sign_silent "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
find "$BUNDLE_APP/Contents/PlugIns" -type f \( -name "*.dylib" -o -name "*.so" \) 2>/dev/null | while read -r f; do sign_silent "$f"; done
find "$BUNDLE_APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" 2>/dev/null | while read -r fw; do sign_silent "$fw"; done
sign_silent "$BUNDLE_APP/Contents/MacOS/seimi-render"
sign_silent "$BUNDLE_APP"
if codesign -dv "$BUNDLE_APP/Contents/MacOS/seimi-render" >/dev/null 2>&1; then
    echo "    [OK] main binary signed"
else
    echo "    [WARN] main binary NOT signed"
fi

# 3. 验证 bundle 内关键资源
echo "== [3/3] verify bundle contents =="
echo "  -- Qt frameworks --"
ls "$BUNDLE_APP/Contents/Frameworks" 2>/dev/null | grep -iE "Qt.*framework" | head -20 || true
echo "  -- WebEngine resources --"
ls "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Resources" 2>/dev/null | grep -iE "icudtl|\.pak|locales" | head || true
echo "  -- WebEngineProcess helper --"
ls "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess" 2>/dev/null && echo "  [OK] QtWebEngineProcess bundled" || echo "  [WARN] QtWebEngineProcess not found in bundle"
echo "  -- cocoa platform plugin (macOS default) --"
ls "$BUNDLE_APP/Contents/PlugIns/platforms/libqcocoa.dylib" 2>/dev/null && echo "  [OK] cocoa plugin bundled" || echo "  [WARN] cocoa plugin missing"
echo "  -- codesign verification --"
if codesign --verify --deep "$BUNDLE_APP" 2>/dev/null; then
    echo "  [OK] bundle signature valid (ad-hoc)"
else
    echo "  [WARN] codesign verification failed — re-run with -codesign=<Developer ID> for distribution"
fi

# 分发安全护栏：bundle 内所有 Mach-O 不得残留构建机绝对路径（install_name + rpath）。
# 任何一处命中即视为不可分发（打不开的 bundle 比不产出更糟），直接退出。
echo "  -- bundle-wide absolute path guard (install_name + rpath) --"
ABS_GUARD_RE='/(usr/local|opt/homebrew|Users|Cellar)/'
GUARD_FAILED=0
GUARD_HITS=""
GUARD_TMP="$(mktemp)"
find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/MacOS/*" -o -path "*/Versions/A/*" \) 2>/dev/null \
    | while IFS= read -r gf; do
        is_macho "$gf" || continue
        gbn="$(basename "$gf")"
        otool -L "$gf" 2>/dev/null | tail -n +2 | while IFS= read -r gline; do
            gdep="$(echo "$gline" | awk '{print $1}')"
            case "$gdep" in @*|/System/*|/usr/lib/*) continue ;; esac
            echo "$gdep" | grep -qE "$ABS_GUARD_RE" || continue
            echo "[install_name] $gbn -> $gdep"
        done
        otool -l "$gf" 2>/dev/null | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' \
            | while IFS= read -r grp; do
                case "$grp" in @*) continue ;; esac
                echo "$grp" | grep -qE "$ABS_GUARD_RE" || continue
                echo "[rpath] $gbn -> $grp"
            done
    done > "$GUARD_TMP"
if [[ -s "$GUARD_TMP" ]]; then
    echo "  [FATAL] bundle 残留构建机绝对路径（分发到别的机器会 dyld: Library not loaded）："
    cat "$GUARD_TMP"
    echo ""
    echo "  常见原因：macdeployqt 未处理某第三方 dylib，或 [2c] 规范化漏网。"
    rm -f "$GUARD_TMP"
    exit 1
else
    echo "  [OK] bundle-wide absolute path check passed (safe to distribute)"
    rm -f "$GUARD_TMP"
fi

BUNDLE_SIZE=$(du -sh "$BUNDLE_APP" | awk '{print $1}')
echo ""
echo "== DONE =="
echo "  bundle : $BUNDLE_APP"
echo "  size   : $BUNDLE_SIZE"
echo ""
echo "  运行分发版（后台 cocoa 应用，靠 LSUIElement 不显示 Dock）:"
echo "    $BUNDLE_APP/Contents/MacOS/seimi-render --http-port 8088 --ws-port 8089"

# 4. 打包可分发的 zip（包名带版本 + OS + 架构）。
#    - 版本从 CMakeLists.txt 的 project(VERSION) 文本解析读（project() 可能跨行，先压成一行）。
#    - 架构读主二进制 Mach-O 头（lipo -archs），不用 uname（Rosetta 下 uname 骗人）。
#    - ditto 比 zip 更适合 .app：保留符号链接/ACL/xattr（签名信息）。
if [[ "${NO_ZIP:-0}" != "1" ]]; then
    echo ""
    echo "== [4] create distributable zip =="

    APP_VERSION="$(tr '\n' ' ' < "$ROOT_DIR/CMakeLists.txt" \
        | grep -oE 'project\([^)]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    [[ -z "$APP_VERSION" ]] && APP_VERSION="unknown"

    case "$(uname -s)" in
        Darwin) OS_NAME="macos" ;;
        Linux)  OS_NAME="linux" ;;
        *)      OS_NAME="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
    esac

    ARCH="$(lipo -archs "$BUNDLE_APP/Contents/MacOS/seimi-render" 2>/dev/null | awk '{print $1}')"
    [[ -z "$ARCH" ]] && ARCH="$(uname -m)"

    ZIP_DIR="${ZIP_DIR:-$BUILD_DIR}"
    mkdir -p "$ZIP_DIR"
    ZIP_NAME="seimi-render-${APP_VERSION}-${OS_NAME}-${ARCH}.zip"
    ZIP_PATH="$ZIP_DIR/$ZIP_NAME"

    echo "  version : $APP_VERSION"
    echo "  os/arch : $OS_NAME / $ARCH"
    echo "  output  : $ZIP_PATH"

    rm -f "$ZIP_PATH"
    if ditto -c -k --keepParent --sequesterRsrc "$BUNDLE_APP" "$ZIP_PATH" 2>/dev/null; then
        ZIP_SIZE=$(du -h "$ZIP_PATH" | awk '{print $1}')
        echo "  [OK] zip created ($ZIP_SIZE)"
        echo ""
        echo "  分发包 : $ZIP_PATH"
    else
        echo "  [WARN] ditto 打包失败，bundle 本身仍可用"
        echo "         手动重试: ditto -c -k --keepParent '$BUNDLE_APP' '$ZIP_PATH'"
    fi
fi
