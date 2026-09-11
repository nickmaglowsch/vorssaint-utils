#!/usr/bin/env bash
# WP-04: prove the hand-built AppDir is self-contained, by running it inside a
# root filesystem that has no Qt at all and with LD_LIBRARY_PATH unset.
#
#   verify-selfcontained.sh <appdir> <chroot> <outdir> [x11|wayland|both]
#
# The chroot is a debootstrap'd Ubuntu 24.04 minbase plus only the libraries
# the excludelist deliberately leaves to the host (glibc/libstdc++ come with
# the base; glib, fontconfig/freetype/harfbuzz, the libglvnd GL/EGL dispatch
# and the X11/xcb client libraries are installed explicitly), Xvfb and
# ImageMagick. No qt6 package and no libQt* file exists in it -- the script
# asserts that before every run, so a stray host Qt cannot rescue the bundle.
#
# Creating the chroot (archive.ubuntu.com is reachable, github.com is not):
#
#   debootstrap --variant=minbase --include=ca-certificates noble "$CHROOT" \
#       http://archive.ubuntu.com/ubuntu/
#   printf 'deb http://archive.ubuntu.com/ubuntu/ noble main universe\n' \
#       > "$CHROOT/etc/apt/sources.list"
#   for d in proc sys dev dev/pts; do mount --bind "/$d" "$CHROOT/$d"; done
#   chroot "$CHROOT" apt-get update
#   chroot "$CHROOT" apt-get install -y --no-install-recommends \
#       xvfb x11-utils imagemagick dbus-x11 fonts-dejavu-core \
#       libgl1 libegl1 libopengl0 libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
#       libglib2.0-0t64 libfontconfig1 libfreetype6 libharfbuzz0b \
#       libice6 libsm6 libx11-xcb1 libxcb-glx0 libxcb-shm0 libxcb-sync1 \
#       libxcb-xfixes0 libselinux1 libgpg-error0 libp11-kit0 libcom-err2
#
# Wayland is tested by running sway on the *host* (the chroot has no
# compositor) and bind-mounting its XDG_RUNTIME_DIR into the chroot, so the
# bundled Qt Wayland client plugin has to do the real protocol work while
# every library it loads comes from the AppDir.
set -u

APPDIR="${1:?usage: verify-selfcontained.sh <appdir> <chroot> <outdir> [x11|wayland|both]}"
CHROOT="${2:?}"
OUTDIR="${3:?}"
MODE="${4:-both}"
mkdir -p "$OUTDIR"

# The AppDir itself and the kernel filesystems are not part of "the host root":
# prune them, then assert that nothing Qt-shaped is left.
assert_no_qt() {
  local n
  n=$(find "$CHROOT" \( -path "$CHROOT/proc" -o -path "$CHROOT/sys" \
        -o -path "$CHROOT/dev" -o -path "$CHROOT/opt/AppDir" \) -prune -o \
        \( -name 'libQt*' -o -name 'qmlimportscanner' -o -name 'qt6' \) -print 2>/dev/null | wc -l)
  echo "== Qt files in the chroot outside the AppDir: $n (must be 0)"
  [ "$n" -eq 0 ] || { echo "!! chroot is contaminated with Qt, aborting"; exit 1; }
  echo "== chroot qt6 dpkg entries: $(chroot "$CHROOT" dpkg -l 2>/dev/null | grep -c qt6 || true)"
}

rm -rf "$CHROOT/opt/AppDir"
assert_no_qt
cp -a "$APPDIR" "$CHROOT/opt/AppDir"

run_x11() {
  cat > "$CHROOT/root/wp04-x11.sh" <<'IN'
#!/bin/bash
set -u
unset LD_LIBRARY_PATH
export XDG_RUNTIME_DIR=/run/user/0; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-<unset>}"
Xvfb :93 -screen 0 1280x800x24 >/tmp/xvfb.log 2>&1 & sleep 2
export DISPLAY=:93
eval "$(dbus-launch --sh-syntax)"
/opt/AppDir/AppRun --screen "${SCREEN:-panel}" --quit-after 9000 >/tmp/app.log 2>&1 &
APP=$!; sleep 5
echo "RSS(KiB)=$(ps -o rss= -p $APP | tr -d ' ')"
import -window root -display :93 /tmp/shot.png 2>/tmp/import.log
wait $APP
echo "--- app log ---"; cat /tmp/app.log
echo "--- libraries resolved from the host root (everything else is the AppDir) ---"
ldd /opt/AppDir/usr/bin/vorssaint-qt-spike | grep -v '/opt/AppDir'
IN
  chmod +x "$CHROOT/root/wp04-x11.sh"
  env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin SCREEN="${SCREEN:-panel}" \
    /usr/sbin/chroot "$CHROOT" /bin/bash /root/wp04-x11.sh
  cp "$CHROOT/tmp/shot.png" "$OUTDIR/chroot-x11.png" 2>/dev/null
  echo "shot: $OUTDIR/chroot-x11.png ($(stat -c %s "$OUTDIR/chroot-x11.png" 2>/dev/null || echo missing) bytes)"
}

run_wayland() {
  local RT=/tmp/wp04-sway-xdg
  rm -rf "$RT"; mkdir -p "$RT"; chmod 700 "$RT"
  cat > /tmp/wp04-sway.conf <<'CFG'
output HEADLESS-1 mode 1280x800
output HEADLESS-1 background #2a2f3a solid_color
default_border pixel 1
for_window [title="Vorssaint panel"] floating enable
for_window [title="Vorssaint preferences"] floating enable
CFG
  XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_RENDERER=pixman \
    WLR_LIBINPUT_NO_DEVICES=1 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=sway \
    sway -c /tmp/wp04-sway.conf > /tmp/wp04-sway.log 2>&1 &
  local SWAY=$!
  sleep 4
  local WD
  WD="$(basename "$(ls -t "$RT"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)")"
  echo "== host sway WAYLAND_DISPLAY=$WD"

  mkdir -p "$CHROOT/run/user/0"
  mount --bind "$RT" "$CHROOT/run/user/0"
  cat > "$CHROOT/root/wp04-wl.sh" <<IN
#!/bin/bash
set -u
unset LD_LIBRARY_PATH DISPLAY
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=$WD
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=sway
echo "LD_LIBRARY_PATH=\${LD_LIBRARY_PATH:-<unset>}"
/opt/AppDir/AppRun --screen "\${SCREEN:-panel}" --quit-after 12000 >/tmp/app-wl.log 2>&1 &
APP=\$!; sleep 8
echo "RSS(KiB)=\$(ps -o rss= -p \$APP | tr -d ' ')"
sleep 1
wait \$APP
echo "--- app log ---"; cat /tmp/app-wl.log
IN
  chmod +x "$CHROOT/root/wp04-wl.sh"
  ( sleep 6; XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$WD" grim "$OUTDIR/chroot-wayland.png" 2>/tmp/wp04-grim.log ) &
  local SHOT=$!
  env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin SCREEN="${SCREEN:-panel}" \
    /usr/sbin/chroot "$CHROOT" /bin/bash /root/wp04-wl.sh
  wait "$SHOT" 2>/dev/null
  echo "shot: $OUTDIR/chroot-wayland.png ($(stat -c %s "$OUTDIR/chroot-wayland.png" 2>/dev/null || echo missing) bytes)"
  umount "$CHROOT/run/user/0" 2>/dev/null
  kill "$SWAY" 2>/dev/null
}

case "$MODE" in
  x11)     run_x11 ;;
  wayland) run_wayland ;;
  both)    run_x11; echo; run_wayland ;;
esac
