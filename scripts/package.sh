#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0

# =============================================================
# seimi-render 打包脚本（macOS）
#
# 产出可独立分发的 .app bundle：把 Qt 框架、WebEngine 资源、
# QtWebEngineProcess 子进程全部用 macdeployqt 复制进 bundle。
#
# 用法:
#   scripts/package.sh                 # 默认 Release，增量
#   scripts/package.sh Debug
#   scripts/package.sh clean           # 清空 build 后全量重建（默认 Release）
#   scripts/package.sh clean Debug     # clean 可与配置组合，顺序不限
#
# 前置: 已安装 Qt6 (brew install qt)，且 QT_PREFIX 可由 brew 解析。
# =============================================================
# 兼容：若被 sh 调用（脚本用了 pipefail、[[ ]]、进程替换等 bash 特性），自动用 bash 重跑自身。
# 注意：macOS 的 /bin/sh 本身就是 bash 3.2（仅以 POSIX 模式运行），此时 BASH_VERSION
# 仍会被设置（如 3.2.57），所以不能仅凭 [ -z "$BASH_VERSION" ] 判断——那样 sh 调用
# 时条件为假，重跑不触发，脚本继续以 POSIX 模式运行，撞上不支持的语法就报 syntax error。
# 这里额外检测 $0 的 basename 是否为 sh：以 sh 名字启动的 bash 会进入 POSIX 模式，
# 关闭进程替换等扩展，必须 exec 成真正的 bash 才行。
case "${0##*/}" in
    sh|-sh|sh.exe) exec bash "$0" "$@" ;;
esac
# 兜底：若 BASH_VERSION 仍为空（被 dash/ash 等非 bash 的 sh 调用），也重跑。
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

# 参数解析：逐个解析，支持 clean（清空 build 目录全量重建）+ CONFIG 任意顺序组合。
# 未知参数直接报错退出，避免误用（如把 clean 当成 CONFIG 传给 cmake，历史上踩过的坑：
# CONFIG=clean 一路传给 cmake -DCMAKE_BUILD_TYPE=clean，增量构建 + 跳过 macdeployqt，
# 产出主二进制带 /opt/homebrew 绝对路径的不可分发包）。对齐 package-linux.sh /
# package-windows.bat 的解析风格，三平台用法一致。
CONFIG=""
CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        clean) CLEAN=1 ;;
        Release|Debug|RelWithDebInfo|MinSizeRel) CONFIG="$1" ;;
        *)
            echo "ERROR: 未知参数 '$1'（应为 clean | Release | Debug | RelWithDebInfo | MinSizeRel）" >&2
            echo "       正确用法：scripts/package.sh Release        （增量）" >&2
            echo "                 scripts/package.sh clean Release  （清空后全量重建，顺序不限）" >&2
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
# 关键：macdeployqt 的架构必须和机器架构一致，否则在 arm64 Mac 上跑 Intel 的
# macdeployqt 会全程 Rosetta 翻译，每个 install_name_tool 子进程慢 3-5x，
# [2/3] 步骤从几秒拖到 3-5 分钟（看起来像卡死）。
# 用户可用 QT_PREFIX=... 显式覆盖。
if [[ -z "${QT_PREFIX:-}" ]]; then
    if [[ -d /opt/homebrew/opt/qt ]]; then
        QT_PREFIX="/opt/homebrew/opt/qt"    # arm64 Homebrew
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

# 1. 构建 bundle（MACOSX_BUNDLE 会生成 .app）
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

# 2. macdeployqt：拷贝 Qt 框架 + 插件 + WebEngine 资源
#    注意：不加 -codesign。macdeployqt 对含第三方 dylib（如 libbrotlicommon）
#    的 WebEngine bundle 做 ad-hoc 签名时会因子组件未签而中断，导致主二进制
#    残留未签名状态 —— macOS 下未签名的 cocoa QApplication 会被 Gatekeeper
#    阻塞初始化，进程卡死。我们改在 2b 之后统一自底向上签名。
#
#    关键：macdeployqt 在 Qt6 下默认会跑 codesign --verify 扫整个 bundle。
#    若 bundle 里有非 Qt 子组件（admin-ui 静态资源、readability JS 等），它会
#    认为"子组件未签名"而卡住（ERROR: Codesign signing error ... app.css）。
#    这些资源是 [2a] 才拷的，但增量构建时上次的残留还在。所以 macdeployqt 前
#    先清掉它们，让 macdeployqt 看到的是纯 Qt bundle；[2a] 稍后重新拷入。
#
# === macdeployqt 跳过优化（arm64 上单步 78s 的根因治理）===
# macdeployqt 即便是原生 arm64 也很慢（实测 ~78s）：每次全量重拷 206MB
# WebEngine 资源、对 23 个框架逐个 fork install_name_tool 改 install name。
# 但增量构建时只有主二进制 seimi-render 会变，Qt 框架/WebEngine 资源在两次
# 构建间几乎不变（Qt 没升级就没变化）。因此：当 bundle 内 Qt 框架指纹与上次
# deploy 一致，且主二进制的所有 Qt 依赖都已存在于 bundle 内时，直接跳过
# macdeployqt——主二进制经 CMake 链接阶段已写成 @loader_path/../Frameworks
# 形态，框架早已就位，无需重新部署。
#
# 指纹 = Qt 版本 + bundle 内框架清单 + WebEngineProcess/offscreen 插件存在性
# + Qt 安装目录的关键二进制 mtime。任一变化（Qt 升级/重装/clean 重建）都触发
# 全量重做，保证正确性。FORCE_DEPLOY=1 可强制跳过缓存。
DEPLOY_CACHE="$BUILD_DIR/.macdeployqt_fingerprint"
compute_qt_fingerprint() {
    local qt_ver fw_list proc_ok offscreen_ok qt_mtime
    qt_ver="$("$QT_PREFIX/bin/qmake6" -query QT_VERSION 2>/dev/null \
              || "$QT_PREFIX/bin/qmake" -query QT_VERSION 2>/dev/null \
              || echo unknown)"
    # bundle 内框架清单（排序，仅文件名）
    fw_list="$(ls "$BUNDLE_APP/Contents/Frameworks/" 2>/dev/null \
              | grep '\.framework$' | sort | tr '\n' ',')"
    # 关键子进程/插件存在性（macdeployqt 负责部署它们）
    [[ -f "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess" ]] \
        && proc_ok=1 || proc_ok=0
    [[ -f "$BUNDLE_APP/Contents/PlugIns/platforms/libqoffscreen.dylib" ]] \
        && offscreen_ok=1 || offscreen_ok=0
    # Qt 安装目录关键文件 mtime：Qt 升级/重装会变（brew upgrade qt）
    qt_mtime="$(stat -f '%m' "$QT_PREFIX/lib/QtCore.framework/QtCore" 2>/dev/null || echo 0)"
    echo "${qt_ver}|${fw_list}|proc=${proc_ok}|offscreen=${offscreen_ok}|mtime=${qt_mtime}"
}
# 主二进制的 Qt 依赖是否全部已就位于 bundle（避免框架清单碰巧一致但缺依赖）。
# 返回标准 Unix 语义：全部就位=0(成功)，有缺失=1(失败)。
# 注意：不能用 `while ... done < <(...)` 进程替换——那是 bash 特性，POSIX sh
# 不支持（macOS /bin/sh 是 bash 的 POSIX 模式会禁用它，报 syntax error near '<'）。
# 也不能用管道 `otool | while read`——管道会开子 shell，里面的 return 无法影响
# 调用者。这里先用命令替换收集框架列表到变量，再在当前 shell 遍历，纯 POSIX。
main_binary_deps_ready() {
    local main="$BUNDLE_APP/Contents/MacOS/seimi-render" deps dep missing=0
    deps="$(otool -L "$main" 2>/dev/null \
            | grep -oE 'Qt[A-Za-z0-9]+\.framework' | sort -u)"
    for dep in $deps; do
        [[ -z "$dep" ]] && continue
        [[ ! -d "$BUNDLE_APP/Contents/Frameworks/$dep" ]] && { missing=1; break; }
    done
    # 关键：主二进制不得残留构建机绝对路径的 Qt 引用。
    # 增量构建重新链接主二进制时，链接器会把 Qt6 keg-only bottle 的 install name
    # 写回成 /opt/homebrew/opt/qtXXX/... 绝对路径。若此时仍命中跳过缓存，
    # macdeployqt 不重跑改写，主二进制就带着本机绝对路径被打包分发，
    # 别的机器 dyld: Library not loaded，去找构建机的 /opt/homebrew → 崩。
    # 检测到这种残留即判 not-ready，强制 macdeployqt 重跑把 LC_LOAD_DYLIB 改成 @rpath。
    if [[ "$missing" -eq 0 ]] && \
       otool -L "$main" 2>/dev/null | grep -qE '/(usr/local|/opt/|Cellar)/.*(Qt[A-Za-z0-9]+\.framework)'; then
        missing=1
    fi
    return $missing
}

# clean 模式或强制部署时，一律跳过缓存判定（清掉旧缓存，强制全量）
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
#   - admin-ui / third_party : [2a] 会重新拷入
#   - data/ : 运行时生成的密钥/cookie 文件（seimi.key 是机器绑定盐，泄露出去等于
#     攻击者拿到 cookie vault 的一部分，虽然单独无法反解，但绝不该出现在分发版里）。
#     首次运行时 CookieStore 会自己 mkpath 重新生成。
rm -rf "$BUNDLE_APP/Contents/MacOS/admin-ui" 2>/dev/null || true
rm -rf "$BUNDLE_APP/Contents/MacOS/third_party" 2>/dev/null || true
rm -rf "$BUNDLE_APP/Contents/MacOS/data" 2>/dev/null || true

if [[ "$SKIP_DEPLOY" == "1" ]]; then
    echo "== [2/3] macdeployqt SKIPPED (Qt 框架指纹未变，主二进制依赖已就位) =="
    echo "  [INFO] 增量构建无需重新部署 Qt（CMake 已将主二进制链接为 @loader_path/../Frameworks）。"
    echo "         若 Qt 升级或部署异常，FORCE_DEPLOY=1 scripts/package.sh 强制全量重做。"
else
    echo "== [2/3] macdeployqt (bundle Qt + WebEngine resources) =="
    # macdeployqt 性能提示：若 Qt 装在 /usr/local（Intel Homebrew）但机器是 arm64，
    # macdeployqt 是 x86_64，全程跑在 Rosetta 下，对每个框架/插件都要 fork install_name_tool
    # 子进程，累计可能要几分钟（看起来像卡死，实际在跑）。这里打印一条提示，避免误判。
    if [[ "$(uname -m)" == "arm64" && "$QT_PREFIX" == /usr/local/* ]]; then
        echo "  [INFO] Qt is x86_64 under Rosetta on arm64 Mac; macdeployqt will be slow (~3-5 min)."
        echo "         根治：改装 arm64 Homebrew 的 Qt 到 /opt/homebrew（见 README）。"
    fi

    # macdeployqt 的 stderr 有两类无害噪声，会刷屏且每次都让人误以为失败，这里压掉：
    #   1) "ERROR: Cannot resolve rpath @rpath/QtSvg.framework/..."
    #      "ERROR: Cannot resolve rpath @rpath/QtVirtualKeyboardQml.framework/..."
    #      "ERROR: Cannot resolve rpath @rpath/QtVirtualKeyboard.framework/..."
    #      —— QtSvg/QtVirtualKeyboard 是运行时按需 dlopen 的可选模块，不在主二进制
    #         LC_LOAD_DYDB 里，macdeployqt 的 rpath 解析器找不到来源就报 ERROR，但
    #         部署照常完成（框架其实已复制进 Frameworks）。这是 Qt 工具的已知误报。
    #   2) "using QList(...)"  —— 上面那行的附属说明，连带压掉。
    # 真正的 fatal 错误（如框架拷贝失败）不会匹配这些模式，仍会显示。
    # 用临时文件保留原始 stderr，便于排查（仅保留过滤后输出到终端）。
    MDQ_ERR_TMP="$(mktemp -t seimi_mdq_err.XXXXXX)"
    # -no-codesign：macdeployqt 默认会对整个 bundle 跑 codesign --verify，遇到任何未签
    # 子组件（包括第三方 dylib）都会反复重试/枚举，在 Rosetta 下尤其慢甚至卡死。
    # 我们在 [2c] 统一自底向上做 ad-hoc 签名，这里跳过，避免重复 + 冲突。
    "$MACDEPLOYQT" "$BUNDLE_APP" \
        -always-overwrite \
        -verbose=1 \
        -no-codesign \
        -executable="$BUNDLE_APP/Contents/MacOS/seimi-render" 2>"$MDQ_ERR_TMP"
    MDQ_RC=$?
    # 过滤掉已知噪声行（"Cannot resolve rpath @rpath/Qt(Svg|VirtualKeyboard...)" + 它的附属 "using QList" 行）。
    # 关键：grep -v 在"没有需要保留的行"时会返回 1（正常行为，非错误），但本脚本顶部
    # 设了 `set -euo pipefail`，会把它当致命错误终止脚本。用 `|| true` 吞掉这个退出码。
    # 这样：macdeployqt 干净（无 ERROR）→ grep 无输出 → exit 1 → || true → 脚本继续；
    #       macdeployqt 有真错误 → grep 输出真错误 → exit 0 → 脚本继续并显示错误。
    grep -vE '^ERROR: Cannot resolve rpath "@rpath/Qt(Svg|VirtualKeyboard|VirtualKeyboardQml)\.framework' "$MDQ_ERR_TMP" \
        | grep -vE '^ERROR: +using QList' >&2 || true
    # 若 macdeployqt 真的非 0 退出且过滤后仍有 ERROR/WARNING 残留，提示用户查 tmp。
    if [[ "$MDQ_RC" -ne 0 ]]; then
        echo "[WARN] macdeployqt exited with code $MDQ_RC; full stderr saved: $MDQ_ERR_TMP" >&2
    else
        rm -f "$MDQ_ERR_TMP"
    fi
    # 部署成功，更新指纹缓存（后续增量构建可命中跳过）
    compute_qt_fingerprint > "$DEPLOY_CACHE"
fi

# 2a. 补管理 UI 静态资源（放到主二进制同级，main.cpp 按 applicationDirPath()/admin-ui 找）。
if [[ -d "$ROOT_DIR/admin-ui" ]]; then
    echo "== [2a] bundle admin-ui resources =="
    cp -r "$ROOT_DIR/admin-ui" "$BUNDLE_APP/Contents/MacOS/admin-ui"
fi

# 2a. 补 markdown 算法依赖的 JS（extract.js / simplify.js）。
#     RenderPool 按 applicationDirPath()/third_party/readability/<file> 加载：
#       - extract.js : md_algorithm=readability 时用（Mozilla Readability 包装器）
#       - simplify.js: md_algorithm=conservative（默认）时用（DOM 简化器，删 script/style 等）
#     缺失时两者都会静默降级（readability→conservative→原始 DOM），但分发版应自带。
if [[ -d "$ROOT_DIR/third_party/readability" ]]; then
    echo "== [2a] bundle readability JS (extract.js, simplify.js) =="
    mkdir -p "$BUNDLE_APP/Contents/MacOS/third_party/readability"
    cp "$ROOT_DIR/third_party/readability/extract.js"  "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
    cp "$ROOT_DIR/third_party/readability/simplify.js" "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
    cp "$ROOT_DIR/third_party/readability/Readability.js" "$BUNDLE_APP/Contents/MacOS/third_party/readability/" 2>/dev/null || true
fi

# 2a. 补 stealth.js + serp/*.js（运行时从 binary 同级 third_party/ 加载，对齐 Linux package-linux.sh）。
#     - stealth/stealth.js : 浏览器指纹统一。缺失会 WARNING 并禁用指纹统一 → 反爬检测可能触发。
#     - serp/*.js          : 搜索引擎结果页结构化提取。缺失静默降级。
#     历史原因之前只拷了 readability，这两个漏了（macOS 上未触发反爬场景没暴露）。
for sub in stealth serp; do
    if [[ -d "$ROOT_DIR/third_party/$sub" ]]; then
        echo "== [2a] bundle $sub JS =="
        mkdir -p "$BUNDLE_APP/Contents/MacOS/third_party/$sub"
        cp "$ROOT_DIR/third_party/$sub/"*.js "$BUNDLE_APP/Contents/MacOS/third_party/$sub/" 2>/dev/null || true
    fi
done

# 2a. 补充 offscreen 平台插件。
#     macdeployqt 默认只部署 cocoa（链接期可见），但本项目运行时通过
#     qputenv("QT_QPA_PLATFORM","offscreen") 使用 offscreen 平台做无头渲染。
#     必须手动把 offscreen 插件复制进 bundle，否则分发版启动即失败。
OFFSCREEN_SRC="$QT_PREFIX/share/qt/plugins/platforms/libqoffscreen.dylib"
if [[ -f "$OFFSCREEN_SRC" ]]; then
    echo "== [2a] bundle offscreen platform plugin =="
    mkdir -p "$BUNDLE_APP/Contents/PlugIns/platforms"
    cp "$OFFSCREEN_SRC" "$BUNDLE_APP/Contents/PlugIns/platforms/"
else
    echo "== [2a] WARN: offscreen plugin not found at $OFFSCREEN_SRC =="
    echo "    无头渲染将不可用。请确认 Qt 安装包含 offscreen 插件。"
fi

# 2b. 修复 QtWebEngineProcess 子进程依赖。
#     macdeployqt 不会重写 QtWebEngineProcess 二进制里的 Qt 框架引用，
#     它们残留为 brew/构建机的绝对路径（如 /usr/local/opt/qtbase/lib/QtCore）。
#     这会导致子进程加载到系统 Qt，与 bundle 内 Qt 并存 → Objective-C 类冲突
#     → WebEngine 渲染不稳定。这里把残留路径改为 @rpath 并指向 bundle Frameworks。
WEBENGINE_PROC="$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
if [[ -f "$WEBENGINE_PROC" ]]; then
    echo "== [2b] fix QtWebEngineProcess library paths -> @rpath =="
    # rpath：从 MacOS 回到 app Contents/Frameworks（已验证 7 个 ".."）。
    install_name_tool -add_rpath "@loader_path/../../../../../../../Frameworks" "$WEBENGINE_PROC" 2>/dev/null || true

    # 通用重写：把 otool -L 中所有 /abs/.../QtXxx.framework/Versions/A/QtXxx
    # 形态的依赖改为 @rpath/QtXxx.framework/Versions/A/QtXxx。
    # 用 awk 抽取路径（otool 行以 tab 开头，路径是第一个空白分隔的字段）。
    # 注意：不能用 `mapfile ... < <(...)`。macOS 自带 /bin/sh 是 bash 3.2 的 POSIX
    # 兼容模式，会禁用进程替换 <(...)（且 mapfile 仅 bash4+ 才有），用 `sh` 调用本脚本
    # 时会报 "syntax error near unexpected token `<'". 改用管道喂给 while read，纯
    # POSIX，sh / bash 3.2 均可运行，行为与原 mapfile+for 等价。
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

    # 改动令签名失效，重新 ad-hoc 签名改动过的子进程二进制。
    # 注意：只签 QtWebEngineProcess 本身，不要对整个 bundle --force --deep，
    # 否则会覆盖 macdeployqt 已对各 Qt 框架做好的嵌套签名，破坏加载链。
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
#     等 Homebrew 依赖）——它们的 install_name 和互相引用残留为 /usr/local/opt/...
#     或 /usr/local/Cellar/... 绝对路径，分发到别的机器 dyld: Library not loaded。
#     这里全量扫描 bundle 内所有 Mach-O，把指向构建机绝对路径的引用重写为 @rpath 相对，
#     并清理 brew 注入的绝对 LC_RPATH，注入按文件位置计算的 @loader_path 相对 rpath。
#     必须在签名（2d）之前：install_name_tool 改动会让 codesign 失效。
echo "== [2c] bundle-wide install_name + rpath normalization =="

# 绝对路径模式：brew 安装位置 + 用户目录（构建机特有路径）。
ABS_RE='/(usr/local|opt/homebrew|Users|Cellar)/'

# 判断文件是否 Mach-O（避免对 .js/.plist 等跑 otool 报噪音）。
is_macho() { file -b "$1" 2>/dev/null | grep -q "Mach-O"; }

# 按文件所在目录算出到 Contents/Frameworks 的相对 @loader_path 路径。
# MacOS/seimi-render                          -> @loader_path/../Frameworks
# Frameworks/*.dylib（dylib 自身就在 Frameworks）-> @loader_path（指向自身目录）
# Frameworks/Qt*.framework/Versions/A/Qt*      -> @loader_path/../../Frameworks
# PlugIns/<grp>/*.dylib                        -> @loader_path/../../Frameworks
# QtWebEngineProcess（Helpers 下深层）          -> 跳过（[2b] 已单独 add）
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

# bundle 内已有的 dylib/framework basename 列表（判断引用目标是否在 bundle 内）。
# 用 find 输出 + grep -qx 匹配，避免关联数组（bash 3.2 不支持）。
BUNDLED_BASENAMES_FILE="$(mktemp)"
find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/Versions/A/*" -o -path "*/MacOS/*" \) 2>/dev/null \
    | while IFS= read -r bf; do is_macho "$bf" && basename "$bf"; done \
    | sort -u > "$BUNDLED_BASENAMES_FILE"
trap 'rm -f "$BUNDLED_BASENAMES_FILE"' EXIT

# 遍历所有 Mach-O，对每个做：改 dylib ID / 改依赖引用 / 规范化 rpath。
# 用 find 喂 while read 管道（POSIX 兼容；macOS bash 3.2 无关联数组/进程替换）。
find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/MacOS/*" -o -path "*/Versions/A/*" \) 2>/dev/null \
    | while IFS= read -r f; do
        is_macho "$f" || continue
        bn="$(basename "$f")"

        # 1. dylib 自身 ID（otool -D）：仅 Frameworks/*.dylib，brew 绝对路径 → @rpath/<bn>
        case "$f" in
            "$BUNDLE_APP/Contents/Frameworks/"*.dylib)
                cur_id="$(otool -D "$f" 2>/dev/null | tail -1)"
                if echo "$cur_id" | grep -qE "$ABS_RE"; then
                    install_name_tool -id "@rpath/$bn" "$f" 2>/dev/null && echo "    $bn: id -> @rpath/$bn"
                fi
                ;;
        esac

        # 2. 依赖引用（otool -L）：brew 绝对路径且 bundle 内有同名文件 → @rpath/<bn>
        #    跳过已相对的（@rpath/@loader_path/@executable_path）和系统库（/System /usr/lib）。
        otool -L "$f" 2>/dev/null | tail -n +3 | while IFS= read -r line; do
            dep="$(echo "$line" | awk '{print $1}')"
            case "$dep" in
                @rpath/*|@loader_path/*|@executable_path/*|/System/*|/usr/lib/*) continue ;;
            esac
            echo "$dep" | grep -qE "$ABS_RE" || continue
            dep_bn="$(basename "$dep")"
            grep -qx "$dep_bn" "$BUNDLED_BASENAMES_FILE" || continue  # bundle 内不存在则不改（护栏告警）
            # 目标形态：Qt framework 保留 @rpath/QtXxx.framework/Versions/A/QtXxx（dyld 按 framework 路径解析），
            # 普通 dylib 用 @rpath/libXXX.dylib。若把 framework 简化成 @rpath/QtXxx 会 dyld找不到（framework 的 install_name ID 含完整路径）。
            case "$dep" in
                *.framework/Versions/*/*) new_dep="@rpath/$(echo "$dep" | sed -E 's#.*/([^/]+\.framework/Versions/[A-Z]/[^/]+)$#\1#')" ;;
                *)                         new_dep="@rpath/$dep_bn" ;;
            esac
            install_name_tool -change "$dep" "$new_dep" "$f" 2>/dev/null && echo "    $bn: dep $dep_bn -> @rpath"
        done

        # 3. rpath：删 brew 绝对路径 rpath（保留已有的 @ 相对 rpath），加按位置的 @loader_path rpath。
        otool -l "$f" 2>/dev/null | awk '/LC_RPATH/{f=1} f && /path /{print $2; f=0}' \
            | while IFS= read -r rp; do
                case "$rp" in @*) continue ;; esac
                echo "$rp" | grep -qE "$ABS_RE" || continue
                install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null && echo "    $bn: -rpath $rp"
            done
        # 注入正确 rpath（若已存在或 [2b] 已加过会报错，忽略）。
        new_rp="$(rpath_for_file "$f")" || continue
        install_name_tool -add_rpath "$new_rp" "$f" 2>/dev/null || true
    done
echo "  [DONE] install_name/rpath normalized (see per-file lines above)"

# 2d. 自底向上 ad-hoc 签名整个 bundle。
#     顺序至关重要：先签叶子（dylib/可执行），再签中间（.app 子 bundle、framework），
#     最后签顶层 .app。否则父签名会因未签子组件而失败。
#     这是让未签名主二进制在 macOS 下正常初始化 cocoa（QApplication）的关键。
#     注：必须在 [2c] 路径规范化之后——install_name_tool 改动会让 codesign 失效。
echo "== [2d] ad-hoc sign bundle (bottom-up) =="
sign_silent() { codesign --force --sign - "$1" 2>/dev/null && return 0 || return 0; }
# 1) 所有叶子 dylib
find "$BUNDLE_APP/Contents/Frameworks" -type f -name "*.dylib" 2>/dev/null | while read -r f; do sign_silent "$f"; done
# 2) QtWebEngineProcess 子 app 内的可执行
sign_silent "$BUNDLE_APP/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
# 3) 每个插件
find "$BUNDLE_APP/Contents/PlugIns" -type f \( -name "*.dylib" -o -name "*.so" \) 2>/dev/null | while read -r f; do sign_silent "$f"; done
# 4) 各 framework 自身（框架目录签名）
find "$BUNDLE_APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" 2>/dev/null | while read -r fw; do sign_silent "$fw"; done
# 5) 顶层主可执行
sign_silent "$BUNDLE_APP/Contents/MacOS/seimi-render"
# 6) 顶层 .app
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

# 分发安全护栏：bundle 内所有 Mach-O 不得残留构建机绝对路径。
# 检查两类：(a) otool -L 的依赖引用，(b) LC_RPATH。任何一处命中即不可分发。
# 这是"打包后分发到别的机器 dyld: Library not loaded"的根因检测——
# 残留即视为不可分发（打不开的 bundle 比不产出更糟），直接退出。
echo "  -- bundle-wide absolute path guard (install_name + rpath) --"
ABS_GUARD_RE='/(usr/local|opt/homebrew|Users|Cellar)/'
GUARD_FAILED=0
GUARD_HITS=""
# 用临时文件收集命中（管道子 shell 无法设置外层变量）。
GUARD_TMP="$(mktemp)"
find "$BUNDLE_APP/Contents" -type f \( -name "*.dylib" -o -path "*/MacOS/*" -o -path "*/Versions/A/*" \) 2>/dev/null \
    | while IFS= read -r gf; do
        is_macho "$gf" || continue
        gbn="$(basename "$gf")"
        # (a) 依赖引用里的绝对路径
        otool -L "$gf" 2>/dev/null | tail -n +3 | while IFS= read -r gline; do
            gdep="$(echo "$gline" | awk '{print $1}')"
            case "$gdep" in @*|/System/*|/usr/lib/*) continue ;; esac
            echo "$gdep" | grep -qE "$ABS_GUARD_RE" || continue
            echo "[install_name] $gbn -> $gdep"
        done
        # (b) LC_RPATH 里的绝对路径
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
    echo "  排查：对照上面列出的文件/路径，检查为何 install_name_tool -change/-delete_rpath 未生效。"
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
