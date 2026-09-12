#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Package the built shell as an AppImage with linuxdeploy + linuxdeploy-plugin-qt
# (WP-P1 slice). The hand-built AppDir of `spikes/wp04-packaging/appdir/
# build-appdir.sh` stays as the audit implementation -- it is what explains
# what a bundle must contain -- and this is the one CI ships.
#
#   linux/packaging/build-appimage.sh <build-dir> [<out.AppImage>]
#
# `build-dir` is a configured CMake build of `linux/shell`; the AppDir is
# staged with `cmake --install`, so the `.desktop` file and the icon come from
# the same install rules a distro package would use.
#
# Tools are downloaded from their GitHub release pages unless they are already
# on PATH or named in LINUXDEPLOY/LINUXDEPLOY_PLUGIN_QT/APPIMAGETOOL. Build
# this on the oldest glibc you intend to support (WP-04: Ubuntu 22.04) and
# test it on the newest.
set -euo pipefail

BUILD_DIR="${1:?usage: build-appimage.sh <build-dir> [<out.AppImage>]}"
OUT="${2:-$PWD/Vorssaint-x86_64.AppImage}"
APPID="com.vorssaint.VorssaintLinux"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHELL_DIR="$(cd "$HERE/../shell" && pwd)"
WORK="${WORK:-$PWD/appimage-work}"
APPDIR="$WORK/AppDir"

# FUSE is unavailable in most containers and on most CI runners; every tool
# here is itself an AppImage, so they are told to extract and run.
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH="${ARCH:-x86_64}"

# fetch <variable-name> <file-name> <url>. The file name matters: linuxdeploy
# finds its plugins by looking for `linuxdeploy-plugin-<name>*` on PATH, so a
# download saved under any other name is a plugin it cannot see ("ERROR: Could
# not find plugin: qt", run 34722872604).
fetch() {
    local var="$1" name="$2" url="$3" dest="$WORK/$2"
    if [ -n "${!var:-}" ]; then echo "${!var}"; return; fi
    if [ ! -x "$dest" ]; then
        curl -fsSL -o "$dest" "$url"
        chmod +x "$dest"
    fi
    echo "$dest"
}

mkdir -p "$WORK"
rm -rf "$APPDIR"

echo "== staging the AppDir with cmake --install"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null
find "$APPDIR" -type f | sort | sed 's/^/   /'

LINUXDEPLOY="$(fetch LINUXDEPLOY linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage)"
LINUXDEPLOY_PLUGIN_QT="$(fetch LINUXDEPLOY_PLUGIN_QT linuxdeploy-plugin-qt-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage)"
APPIMAGETOOL="$(fetch APPIMAGETOOL appimagetool-x86_64.AppImage \
  https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage)"
# linuxdeploy resolves `--plugin qt` by searching PATH, not its own directory.
PATH="$(dirname "$LINUXDEPLOY_PLUGIN_QT"):$PATH"
export PATH
# The type-2 runtime is fetched separately and passed with --runtime-file:
# appimagetool otherwise downloads it at package time, which fails on a runner
# with no network and silently produces an image with no runtime at all.
RUNTIME="$WORK/runtime-$ARCH"
[ -f "$RUNTIME" ] || curl -fsSL -o "$RUNTIME" \
  "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH"

# Debian/Ubuntu's /usr/bin/qmake is the qtchooser wrapper with no default, so
# the plugin has to be pointed at the Qt 6 one by name (WP-04 finding).
export QMAKE="${QMAKE:-/usr/bin/qmake6}"
# The QML sources are inside the binary's resources, so the plugin cannot scan
# them from the AppDir; point it at the tree they came from.
export QML_SOURCES_PATHS="${QML_SOURCES_PATHS:-$SHELL_DIR/qml}"
# Wayland is the port's target session and its sub-plugins are dlopen()ed, so
# ldd never sees them; platformthemes is what carries the portal file dialogs.
export EXTRA_QT_PLUGINS="${EXTRA_QT_PLUGINS:-wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration;platformthemes}"
export QT_QPA_PLATFORM=offscreen

echo "== linuxdeploy + plugin qt"
"$LINUXDEPLOY" --appdir "$APPDIR" --plugin qt \
    -d "$APPDIR/usr/share/applications/$APPID.desktop" \
    -i "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APPID.png"

echo "== appimagetool"
"$APPIMAGETOOL" --runtime-file "$RUNTIME" "$APPDIR" "$OUT"
chmod +x "$OUT"

echo "== $OUT"
ls -l "$OUT"
du -sh "$APPDIR"
echo "== bundled libraries: $(find "$APPDIR/usr/lib" -maxdepth 1 -name '*.so*' | wc -l)"
