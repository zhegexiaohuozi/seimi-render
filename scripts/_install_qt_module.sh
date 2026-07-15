#!/usr/bin/env bash
# Helper: download + extract a Qt 6.7.2 addon module archive into the Qt tree.
# Works around the aqtinstall v3.3.0 metadata bug for addon modules
# (qtwebsockets / qtwebchannel / qtpdf / qtpositioning) by fetching the
# real .7z directly from the Qt repo mirror.
#
# Qt installer archives extract to a path RELATIVE to the Qt install prefix
# (e.g. they contain "6.7.2/gcc_64/lib/..."). We extract into a temp dir and
# then merge the contents under <prefix>/6.7.2/gcc_64/ up into <prefix>/,
# matching the layout aqt produces.
#
# Usage: _install_qt_module.sh <module-short-name>
#   e.g. _install_qt_module.sh qtwebchannel
# 兼容：若被 sh(dash) 调用，自动用 bash 重跑自身（脚本用了 pipefail 等 bash 特性）。
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -euo pipefail

MOD="${1:?usage: $0 <module-short-name> e.g. qtwebchannel}"

QT_PREFIX="${QT_PREFIX:-/opt/Qt}"
QT_VERSION="${QT_VERSION:-6.7.2}"
QT_ARCH="${QT_ARCH:-gcc_64}"
QT_REPO="${QT_REPO:-https://download.qt.io/online/qtsdkrepository/linux_x64/desktop}"
QT_INSTALL_DIR="$QT_PREFIX/$QT_VERSION/$QT_ARCH"

# 6.7.2 -> qt6_672
QT_REPO_VER="qt6_$(echo "$QT_VERSION" | tr -d '.')"

# Query the mirror's Updates.xml to get the exact package folder + archive name.
PKG_FOLDER="$(python3 - "$MOD" "$QT_REPO_VER" <<'PY'
import sys, urllib.request, xml.etree.ElementTree as ET
mod = sys.argv[1]
repover = sys.argv[2]
url = f"https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/{repover}/Updates.xml"
data = urllib.request.urlopen(url, timeout=30).read()
root = ET.fromstring(data)
for pkg in root.iter("PackageUpdate"):
    name = pkg.findtext("Name") or ""
    # match addon module e.g. qt.qt6.672.addons.qtwebchannel.linux_gcc_64
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

# The actual file on the mirror is prefixed with the package version string,
# e.g. "6.7.2-0-202406110334qtwebchannel-...7z". DownloadableArchives in the
# XML omits that prefix; the mirror stores it prefixed.
REMOTE_ARCHIVE="$PKG_VER$ARCHIVE"
REMOTE_SHA1="$REMOTE_ARCHIVE.sha1"

echo "  [$MOD] downloading $REMOTE_ARCHIVE ..."
curl -fSL -m 300 -o "$TMP/$REMOTE_ARCHIVE" "$BASE_URL/$REMOTE_ARCHIVE"
curl -fsSL -m 60  -o "$TMP/$REMOTE_SHA1" "$BASE_URL/$REMOTE_SHA1" || true
ARCHIVE="$REMOTE_ARCHIVE"

# sha1 check
if [ -s "$TMP/$ARCHIVE.sha1" ]; then
    EXPECTED="$(awk '{print $1}' "$TMP/$ARCHIVE.sha1")"
    ACTUAL="$(sha1sum "$TMP/$ARCHIVE" | awk '{print $1}')"
    if [ "$EXPECTED" = "$ACTUAL" ]; then
        echo "  [$MOD] sha1 OK"
    else
        echo "  [$MOD] [WARN] sha1 mismatch (expected=$EXPECTED actual=$ACTUAL)" >&2
    fi
fi

# Extract into a temp staging dir, then merge the inner 6.7.2/gcc_64/ up to the prefix.
STAGE="$TMP/stage"
7z x -y -o"$STAGE" "$TMP/$ARCHIVE" >/dev/null

# Archives contain "<version>/<arch>/lib/..." — find the inner root and merge.
INNER="$STAGE/$QT_VERSION/$QT_ARCH"
if [ ! -d "$INNER" ]; then
    # Some archives may be relative to prefix already; fall back to STAGE.
    INNER="$STAGE"
fi
echo "  [$MOD] merging $INNER -> $QT_INSTALL_DIR"
mkdir -p "$QT_INSTALL_DIR"
cp -rn "$INNER"/. "$QT_INSTALL_DIR"/ 2>/dev/null || true
echo "  [$MOD] done"
