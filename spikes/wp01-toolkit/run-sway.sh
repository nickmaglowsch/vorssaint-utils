#!/usr/bin/env bash
# Run one candidate screen on a headless sway (wlroots) session, screenshot it
# with grim, and report the tray registrations.
#
#   run-sway.sh <out.png> <command...>
#
# sway is started with the wlroots headless backend and the pixman (software)
# renderer, because the microVM has no DRM device and no GPU:
#   WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
set -u

OUT="$1"; shift
SHOT_AFTER="${SHOT_AFTER:-10}"
RUN_FOR="${RUN_FOR:-18}"
HERE="$(cd "$(dirname "$0")" && pwd)"

export XDG_RUNTIME_DIR=/tmp/wp01/xdg-sway
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=sway

cat > /tmp/wp01/sway.conf <<'CFG'
output HEADLESS-1 mode 1280x800
output HEADLESS-1 background #2a2f3a solid_color
default_border pixel 1
# Float the spike windows so the screenshots show their own size rather than
# sway's tiling; the overlay asks for fullscreen itself and is left alone.
for_window [title="Vorssaint panel"] floating enable
for_window [title="Vorssaint preferences"] floating enable
CFG

eval "$(dbus-launch --sh-syntax)"
/usr/bin/python3.12 "$HERE/sni-watcher.py" "$RUN_FOR" > /tmp/wp01/sni-sway.log 2>&1 &

sway -c /tmp/wp01/sway.conf > /tmp/wp01/sway.log 2>&1 &
SWAY_PID=$!
sleep 4

# sway writes WAYLAND_DISPLAY into XDG_RUNTIME_DIR; find it rather than guess.
export WAYLAND_DISPLAY="$(basename "$(ls -t "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)")"
echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
unset DISPLAY

"$@" > /tmp/wp01/app-sway.log 2>&1 &
APP_PID=$!
sleep "$SHOT_AFTER"

ps -o rss= -p "$APP_PID" > /tmp/wp01/rss-sway.txt 2>/dev/null
grim "$OUT" 2> /tmp/wp01/grim.log
echo "shot: $OUT ($(stat -c %s "$OUT" 2>/dev/null || echo missing) bytes)"
echo "--- sway tree ---"
swaymsg -t get_tree 2>/dev/null | grep -oE '"(app_id|name|layer)": "[^"]*"' | sort -u | head -20

kill "$APP_PID" 2>/dev/null
kill "$SWAY_PID" 2>/dev/null
pkill -f sni-watcher.py 2>/dev/null
kill "${DBUS_SESSION_BUS_PID:-0}" 2>/dev/null

echo "--- app log ---"; cat /tmp/wp01/app-sway.log
echo "--- sni watcher ---"; cat /tmp/wp01/sni-sway.log
echo "--- rss (KiB) ---"; cat /tmp/wp01/rss-sway.txt
echo "--- grim ---"; cat /tmp/wp01/grim.log
exit 0
