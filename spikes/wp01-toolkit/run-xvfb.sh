#!/usr/bin/env bash
# Run one candidate screen under Xvfb with a private session bus and a fake
# StatusNotifierWatcher, screenshot it, and report the tray registrations.
#
#   run-xvfb.sh <out.png> <command...>
#
# Everything it needs is set here so the report's commands are reproducible:
# no inherited DISPLAY, DBUS_SESSION_BUS_ADDRESS or XDG_RUNTIME_DIR.
set -u

OUT="$1"; shift
DISPLAY_NUM="${DISPLAY_NUM:-:97}"
SHOT_AFTER="${SHOT_AFTER:-6}"
RUN_FOR="${RUN_FOR:-11}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/wp01/xdg}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

Xvfb "$DISPLAY_NUM" -screen 0 1280x800x24 >/tmp/wp01/xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2
export DISPLAY="$DISPLAY_NUM"

eval "$(dbus-launch --sh-syntax)"
/usr/bin/python3.12 "$(dirname "$0")/sni-watcher.py" "$RUN_FOR" >/tmp/wp01/sni-xvfb.log 2>&1 &
sleep 2

"$@" >/tmp/wp01/app-xvfb.log 2>&1 &
APP_PID=$!
sleep "$SHOT_AFTER"

# RSS of the app after it has been up and ticking.
ps -o rss= -p "$APP_PID" > /tmp/wp01/rss-xvfb.txt 2>/dev/null

import -window root -display "$DISPLAY_NUM" "$OUT" 2>/tmp/wp01/import.log
echo "shot: $OUT ($(stat -c %s "$OUT" 2>/dev/null || echo missing) bytes)"

wait "$APP_PID" 2>/dev/null
kill "$XVFB_PID" 2>/dev/null
kill %2 2>/dev/null
[ -n "${DBUS_SESSION_BUS_PID:-}" ] && kill "$DBUS_SESSION_BUS_PID" 2>/dev/null

echo "--- app log ---"; cat /tmp/wp01/app-xvfb.log
echo "--- sni watcher ---"; cat /tmp/wp01/sni-xvfb.log
echo "--- rss (KiB) ---"; cat /tmp/wp01/rss-xvfb.txt
