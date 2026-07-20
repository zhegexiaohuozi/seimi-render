#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# 绕开 aqtinstall v3.x 对 Qt6 附加模块的元数据 bug，直接从 Qt 镜像下载 .7z 归档。
# 归档解压后路径是相对 Qt 安装前缀的（含 6.7.2/gcc_64/），提取后合并到前缀下。
# 用法: _install_qt_module.sh <module-short-name>  (e.g. qtwebchannel)
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

MOD="${1:?usage: $0 <module-short-name> e.g. qtwebchannel}"

QT_PREFIX="${QT_PREFIX:-/opt/Qt}"
QT_VERSION="${QT_VERSION:-6.7.2}"
QT_ARCH="${QT_ARCH:-gcc_64}"
QT_REPO="${QT_REPO:-https://download.qt.io/online/qtsdkrepository/linux_x64/desktop}"
QT_INSTALL_DIR="$QT_PREFIX/$QT_VERSION/$QT_ARCH"

QT_REPO_VER="qt6_$(echo "$QT_VERSION" | tr -d '.')"

# 查镜像的 Updates.xml 拿到包目录名 + 归档名
PKG_FOLDER="$(python3 - "$MOD" "$QT_REPO_VER" <<'PY'
import sys, urllib.request, xml.etree.ElementTree as ET
mod = sys.argv[1]
repover = sys.argv[2]
url = f"https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/{repover}/Updates.xml"
data = urllib.request.urlopen(url, timeout=30).read()
root = ET.fromstring(data)
for pkg in root.iter("PackageUpdate"):
    name = pkg.findtext("Name") or ""
    if name.endswith(f".addons.{mod}.linux_gcc_64"):
        ar = pkg.findtext("DownloadableArchives") or ""
        ver = pkg.findtext("Version") or ""
        print(f"{name}|{ver}|{ar}")
        break
PY
)"
if [ -z "$PKG_FOLDER" ]; then
    echo "ERROR: could not find module '$MOD' in $QT_REPO_VER/Updates.xml" >&2
    exit 1
fi
PKG_NAME="$(echo "$PKG_FOLDER" | cut -d'|' -f1)"
PKG_VER="$(echo "$PKG_FOLDER" | cut -d'|' -f2)"
ARCHIVE="$(echo "$PKG_FOLDER" | cut -d'|' -f3)"

BASE_URL="$QT_REPO/$QT_REPO_VER/$PKG_NAME"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 镜像上的实际文件名带版本前缀（DownloadableArchives 里省略了）
REMOTE_ARCHIVE="$PKG_VER$ARCHIVE"
REMOTE_SHA1="$REMOTE_ARCHIVE.sha1"

echo "  [$MOD] downloading $REMOTE_ARCHIVE ..."
curl -fSL -m 300 -o "$TMP/$REMOTE_ARCHIVE" "$BASE_URL/$REMOTE_ARCHIVE"
curl -fsSL -m 60  -o "$TMP/$REMOTE_SHA1" "$BASE_URL/$REMOTE_SHA1" || true
ARCHIVE="$REMOTE_ARCHIVE"

# sha1 校验
if [ -s "$TMP/$ARCHIVE.sha1" ]; then
    EXPECTED="$(awk '{print $1}' "$TMP/$ARCHIVE.sha1")"
    ACTUAL="$(sha1sum "$TMP/$ARCHIVE" | awk '{print $1}')"
    if [ "$EXPECTED" = "$ACTUAL" ]; then
        echo "  [$MOD] sha1 OK"
    else
        echo "  [$MOD] [WARN] sha1 mismatch (expected=$EXPECTED actual=$ACTUAL)" >&2
    fi
fi

# 解压到暂存目录，合并到 Qt 安装前缀
STAGE="$TMP/stage"
7z x -y -o"$STAGE" "$TMP/$ARCHIVE" >/dev/null

INNER="$STAGE/$QT_VERSION/$QT_ARCH"
[ ! -d "$INNER" ] && INNER="$STAGE"
echo "  [$MOD] merging $INNER -> $QT_INSTALL_DIR"
mkdir -p "$QT_INSTALL_DIR"
cp -rn "$INNER"/. "$QT_INSTALL_DIR"/ 2>/dev/null || true
echo "  [$MOD] done"
