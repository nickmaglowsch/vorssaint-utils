#!/usr/bin/env bash
# WP-04: build a complete, self-contained AppDir for the WP-01 Qt 6 Quick
# spike *by hand* -- no linuxdeploy, no linuxdeploy-plugin-qt.
#
# Why by hand: github.com releases are blocked from the spike container, so
# linuxdeploy cannot be fetched there. Doing it by hand is also the honest
# way to learn what the port actually has to bundle; CI (which does have
# github access) builds the same AppDir *and* a linuxdeploy one, and the
# report compares them.
#
#   build-appdir.sh <built-binary> [<appdir>]
#
# Defaults to ./AppDir next to this script. Everything else is discovered
# from `qmake6 -query`, so the same script works on jammy's Qt 6.2 and
# noble's Qt 6.4.
#
# Layout produced (the linuxdeploy/appimage convention):
#
#   AppDir/AppRun                      entry point, sets Qt env
#   AppDir/<appid>.desktop             also copied to usr/share/applications
#   AppDir/<appid>.png                 also copied to usr/share/icons/...
#   AppDir/usr/bin/<binary>
#   AppDir/usr/bin/qt.conf             belt-and-braces for the env in AppRun
#   AppDir/usr/lib/*.so*               the ldd closure minus the excludelist
#   AppDir/usr/lib/qt6/plugins/...     platform + friends
#   AppDir/usr/lib/qt6/qml/...         only the QML modules actually imported
#
# RUNPATH, not LD_LIBRARY_PATH
# ---------------------------
# Every ELF in the AppDir gets `patchelf --set-rpath '$ORIGIN/<rel>'` pointing
# at AppDir/usr/lib. LD_LIBRARY_PATH would be inherited by every child process
# the app spawns -- xdg-open, the portal helpers, `ddcutil`, PackageKit's
# client, a user's shell command -- and those host binaries would then load our
# bundled Qt/ICU/ffmpeg instead of the host's and crash or silently misbehave.
# LD_LIBRARY_PATH also outranks a library's own RUNPATH, so it poisons
# dlopen()ed host plugins too (Mesa drivers, GIO modules, PipeWire SPA
# plugins). RUNPATH is per-ELF: it affects our binaries and nothing else.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${1:?usage: build-appdir.sh <built-binary> [<appdir>]}"
APPDIR="${2:-$HERE/AppDir}"
APPID="${APPID:-com.example.vorssaint-linux-spike}"
QML_SRC="${QML_SRC:-$HERE/../../wp01-toolkit/qt-quick/qml}"
EXCLUDES="$HERE/excludelist"

BIN="$(readlink -f "$BIN")"
BINNAME="$(basename "$BIN")"

QMAKE="${QMAKE:-qmake6}"
QT_LIBS="$($QMAKE -query QT_INSTALL_LIBS)"
QT_PLUGINS="$($QMAKE -query QT_INSTALL_PLUGINS)"
QT_QML="$($QMAKE -query QT_INSTALL_QML)"
QT_LIBEXEC="$($QMAKE -query QT_INSTALL_LIBEXECS)"
QT_VERSION="$($QMAKE -query QT_VERSION)"
SCANNER="$QT_LIBEXEC/qmlimportscanner"

echo "== Qt $QT_VERSION"
echo "   libs    $QT_LIBS"
echo "   plugins $QT_PLUGINS"
echo "   qml     $QT_QML"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/lib/qt6/plugins" "$APPDIR/usr/lib/qt6/qml" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$BIN" "$APPDIR/usr/bin/$BINNAME"

# ---------------------------------------------------------------------------
# Qt plugins. Only the groups a menu-bar/overlay app can reach: the two
# platform plugins the port targets (wayland, xcb) plus the offscreen one that
# headless CI uses, the Wayland sub-plugin families (shell integration,
# decorations, client buffer integration), the XCB GL glue, image formats,
# icon engines, the XDG platform theme (this is what turns QSystemTrayIcon
# into a StatusNotifierItem on KDE/GNOME), and TLS backends.
# ---------------------------------------------------------------------------
PLUGIN_GROUPS=(
  platforms platformthemes platforminputcontexts
  wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client
  xcbglintegrations egldeviceintegrations
  imageformats iconengines tls
)
for g in "${PLUGIN_GROUPS[@]}"; do
  [ -d "$QT_PLUGINS/$g" ] || continue
  mkdir -p "$APPDIR/usr/lib/qt6/plugins/$g"
  # Drop platform plugins the port will never select, they only cost size.
  for so in "$QT_PLUGINS/$g"/*.so; do
    [ -e "$so" ] || continue
    b="$(basename "$so")"
    if [ "$g" = platforms ]; then
      case "$b" in
        libqwayland*.so|libqxcb.so|libqoffscreen.so|libqminimal.so) ;;
        *) continue ;;
      esac
    fi
    cp "$so" "$APPDIR/usr/lib/qt6/plugins/$g/$b"
  done
done

# ---------------------------------------------------------------------------
# QML modules. qmlimportscanner (Qt 6 ships it in libexec) reads the .qml
# sources and reports the transitive module set, including the style fallbacks
# QtQuick.Controls pulls in. If it is missing we fall back to grepping the
# `import` lines, which under-reports (no transitive closure) -- the script
# says so loudly rather than producing a quietly broken bundle.
# ---------------------------------------------------------------------------
declare -a QML_PATHS=()
if [ -x "$SCANNER" ]; then
  echo "== qmlimportscanner $SCANNER"
  mapfile -t QML_PATHS < <("$SCANNER" -rootPath "$QML_SRC" -importPath "$QT_QML" \
    | python3 -c 'import json,sys
for i in json.load(sys.stdin):
    p = i.get("path")
    if i.get("type") == "module" and p:
        print(p)')
else
  echo "!! qmlimportscanner not found at $SCANNER -- falling back to parsing"
  echo "!! `import` lines; this does NOT follow transitive imports."
  mapfile -t QML_PATHS < <(grep -h '^[[:space:]]*import ' "$QML_SRC"/*.qml \
    | awk '{print $2}' | grep -v '^"' | sort -u \
    | while read -r m; do d="$QT_QML/${m//.//}"; [ -d "$d" ] && echo "$d"; done)
fi

for p in "${QML_PATHS[@]}"; do
  rel="${p#$QT_QML/}"
  [ "$rel" = "$p" ] && continue          # not under the Qt qml prefix
  mkdir -p "$APPDIR/usr/lib/qt6/qml/$rel"
  # Only the module's own files, not its submodule directories: the scanner
  # lists every submodule separately, so copying recursively would duplicate.
  find "$p" -maxdepth 1 -type f -exec cp {} "$APPDIR/usr/lib/qt6/qml/$rel/" \;
done
echo "== QML modules bundled: ${#QML_PATHS[@]}"

# ---------------------------------------------------------------------------
# Shared-library closure. Start from every ELF already in the AppDir, run ldd,
# copy anything not in the excludelist into usr/lib, repeat until the set
# stops growing (bundled Qt libs pull in more bundled Qt libs).
# ---------------------------------------------------------------------------
is_excluded() {
  grep -qxF "$1" <(grep -v '^[[:space:]]*#' "$EXCLUDES" | grep -v '^[[:space:]]*$')
}

HOST_PROVIDED="$APPDIR/../host-provided.txt"; : > "$HOST_PROVIDED"
round=0
while :; do
  round=$((round + 1))
  added=0
  while IFS= read -r elf; do
    while IFS= read -r line; do
      # "  libFoo.so.1 => /path/to/libFoo.so.1 (0x...)"
      soname="$(awk '{print $1}' <<<"$line")"
      path="$(awk '{print $3}' <<<"$line")"
      [ -f "${path:-}" ] || continue
      if is_excluded "$soname"; then
        echo "$soname" >> "$HOST_PROVIDED"
        continue
      fi
      [ -e "$APPDIR/usr/lib/$soname" ] && continue
      cp -L "$path" "$APPDIR/usr/lib/$soname"
      added=$((added + 1))
    done < <(ldd "$elf" 2>/dev/null | grep ' => /')
  done < <(find "$APPDIR" -type f \( -name '*.so' -o -name '*.so.*' -o -perm -u+x \) \
             -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
  echo "== closure round $round: +$added"
  [ "$added" -eq 0 ] && break
done
sort -u "$HOST_PROVIDED" -o "$HOST_PROVIDED"

# Qt resolves some things by dlopen() with no DT_NEEDED entry, so ldd never
# sees them. The Wayland client stack is the one that matters for this port.
for extra in libQt6WaylandClient.so.6 libQt6WaylandEglClientHwIntegration.so.6 \
             libQt6WlShellIntegration.so.6 libQt6XcbQpa.so.6; do
  [ -e "$APPDIR/usr/lib/$extra" ] && continue
  [ -e "$QT_LIBS/$extra" ] && cp -L "$QT_LIBS/$extra" "$APPDIR/usr/lib/$extra" && echo "== dlopen extra: $extra"
done
# ...and those may need another closure pass.
while IFS= read -r elf; do
  while IFS= read -r line; do
    soname="$(awk '{print $1}' <<<"$line")"; path="$(awk '{print $3}' <<<"$line")"
    [ -f "${path:-}" ] || continue
    is_excluded "$soname" && continue
    [ -e "$APPDIR/usr/lib/$soname" ] || cp -L "$path" "$APPDIR/usr/lib/$soname"
  done < <(ldd "$elf" 2>/dev/null | grep ' => /')
done < <(find "$APPDIR/usr/lib" -maxdepth 1 -name '*.so*')

# ---------------------------------------------------------------------------
# RUNPATH on every ELF: $ORIGIN-relative path to AppDir/usr/lib. See the note
# at the top for why this instead of LD_LIBRARY_PATH.
# ---------------------------------------------------------------------------
LIBDIR="$APPDIR/usr/lib"
while IFS= read -r elf; do
  rel="$(realpath --relative-to="$(dirname "$elf")" "$LIBDIR")"
  if [ "$rel" = "." ]; then rp='$ORIGIN'; else rp="\$ORIGIN/$rel"; fi
  patchelf --set-rpath "$rp" "$elf" 2>/dev/null || echo "!! patchelf failed: $elf"
done < <(find "$APPDIR" -type f -exec sh -c 'file -b "$1" | grep -q "ELF.*\(executable\|shared object\)" && echo "$1"' _ {} \;)

# ---------------------------------------------------------------------------
# qt.conf, AppRun, desktop file, icon.
# ---------------------------------------------------------------------------
cat > "$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = lib/qt6/plugins
Imports = lib/qt6/qml
Qml2Imports = lib/qt6/qml
Libraries = lib
EOF

cat > "$APPDIR/AppRun" <<EOF
#!/bin/sh
# AppRun for $APPID. Deliberately POSIX sh: the host may have no bash.
#
# No LD_LIBRARY_PATH is set here, on purpose -- every ELF in this AppDir
# carries a \$ORIGIN RUNPATH instead, so child processes (xdg-open, portal
# helpers, ddcutil, a user shell command) keep using the host's libraries.
HERE="\$(dirname "\$(readlink -f "\$0")")"
export APPDIR="\${APPDIR:-\$HERE}"

# Wayland first, X11 second: the port's target is Wayland sessions, and Qt
# falls through to the next entry when the first plugin cannot connect.
export QT_QPA_PLATFORM="\${QT_QPA_PLATFORM:-wayland;xcb}"
export QT_PLUGIN_PATH="\$HERE/usr/lib/qt6/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
export QML2_IMPORT_PATH="\$HERE/usr/lib/qt6/qml\${QML2_IMPORT_PATH:+:\$QML2_IMPORT_PATH}"
export QML_IMPORT_PATH="\$QML2_IMPORT_PATH"
export XDG_DATA_DIRS="\$HERE/usr/share:\${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

exec "\$HERE/usr/bin/$BINNAME" "\$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/$APPID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Vorssaint (Linux spike)
Comment=WP-04 packaging proof for the Vorssaint Linux port
Exec=$BINNAME
Icon=$APPID
Categories=Utility;
Terminal=false
X-AppImage-Version=0-wp04
EOF
cp "$APPDIR/$APPID.desktop" "$APPDIR/usr/share/applications/"

# A generated icon rather than a checked-in binary: branding is undecided
# (PLAN.md section 10) and this must not look like a real product icon.
python3 - "$APPDIR/$APPID.png" <<'PY'
import struct, sys, zlib
S = 256
px = bytearray()
for y in range(S):
    px.append(0)
    for x in range(S):
        inside = 24 <= x < S-24 and 24 <= y < S-24
        px += bytes((0x4a, 0x90, 0xd9) if inside else (0x1e, 0x22, 0x2b))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
open(sys.argv[1], 'wb').write(
    b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', S, S, 8, 2, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(bytes(px), 9))
    + chunk(b'IEND', b''))
PY
cp "$APPDIR/$APPID.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
cp "$APPDIR/$APPID.png" "$APPDIR/.DirIcon"

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
echo
echo "== AppDir: $APPDIR"
echo "== total size: $(du -sh "$APPDIR" | cut -f1) ($(du -sb "$APPDIR" | cut -f1) bytes)"
echo "== files: $(find "$APPDIR" -type f | wc -l)   bundled libs: $(find "$APPDIR/usr/lib" -maxdepth 1 -name '*.so*' | wc -l)"
echo "== top 15 largest files:"
find "$APPDIR" -type f -printf '%s %p\n' | sort -rn | head -15 \
  | awk '{printf "%10.2f MiB  %s\n", $1/1048576, $2}'
echo "== libraries left to the host (excludelist hits):"
sed 's/^/   /' "$HOST_PROVIDED"
echo "== RUNPATH of the main binary: $(patchelf --print-rpath "$APPDIR/usr/bin/$BINNAME")"
