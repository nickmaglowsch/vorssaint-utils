#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Headless smoke for the shell: run it under Xvfb on a private session bus
# with the WP-01 fake StatusNotifierWatcher, and fail unless the tray item
# really registers over D-Bus. A tray icon that fell back to X11 XEmbed, or
# that never appeared, produces no REGISTERED line and this exits non-zero.
#
#   linux/shell/tests/smoke-tray.sh <binary-or-AppImage> <out.png> [extra args...]
#
# Needs: Xvfb, dbus-launch, ImageMagick's `import`, and a python3 with
# dbus-python and PyGObject (python3-dbus, python3-gi on Debian/Ubuntu).
set -uo pipefail

BIN="${1:?usage: smoke-tray.sh <binary> <out.png> [args...]}"
OUT="${2:?usage: smoke-tray.sh <binary> <out.png> [args...]}"
shift 2

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WATCHER="${WATCHER:-$HERE/../../../spikes/wp01-toolkit/sni-watcher.py}"
DISPLAY_NUM="${DISPLAY_NUM:-:97}"
RUN_FOR="${RUN_FOR:-12}"
SHOT_AFTER="${SHOT_AFTER:-6}"
LOGDIR="${LOGDIR:-$(mktemp -d)}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$LOGDIR/xdg}"
mkdir -p "$XDG_RUNTIME_DIR" "$(dirname "$OUT")"
chmod 700 "$XDG_RUNTIME_DIR"

Xvfb "$DISPLAY_NUM" -screen 0 1280x800x24 >"$LOGDIR/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 2
export DISPLAY="$DISPLAY_NUM"

# Not simply `python3`: the interpreter first on PATH may be one dbus-python
# is not installed for, and a watcher that failed to start would look exactly
# like an app that never registered.
PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
    for candidate in python3 /usr/bin/python3 /usr/bin/python3.12 /usr/bin/python3.11 \
                     /usr/bin/python3.10; do
        if command -v "$candidate" >/dev/null 2>&1 \
           && "$candidate" -c "import dbus, dbus.mainloop.glib, gi" >/dev/null 2>&1; then
            PYTHON="$candidate"; break
        fi
    done
fi
if [ -z "$PYTHON" ]; then
    echo "FAIL: no python3 with dbus-python and PyGObject (install python3-dbus python3-gi)"
    kill "$XVFB_PID" 2>/dev/null
    exit 2
fi
echo "watcher interpreter: $PYTHON"

eval "$(dbus-launch --sh-syntax)"
"$PYTHON" "$WATCHER" "$RUN_FOR" >"$LOGDIR/sni.log" 2>&1 &
sleep 2

"$BIN" --screen panel --quit-after $((SHOT_AFTER * 1000 + 3000)) "$@" >"$LOGDIR/app.log" 2>&1 &
APP_PID=$!
sleep "$SHOT_AFTER"

import -window root -display "$DISPLAY_NUM" "$OUT" 2>"$LOGDIR/import.log" || true
rc=0; wait "$APP_PID" || rc=$?
kill "$XVFB_PID" 2>/dev/null || true
[ -n "${DBUS_SESSION_BUS_PID:-}" ] && kill "$DBUS_SESSION_BUS_PID" 2>/dev/null

echo "=== app log (exit=$rc)"; cat "$LOGDIR/app.log"
echo "=== StatusNotifierWatcher"; cat "$LOGDIR/sni.log"
echo "=== screenshot"; ls -l "$OUT" 2>&1 || true

fail=0
[ "$rc" -eq 0 ] || { echo "FAIL: the app exited $rc"; fail=1; }
grep -q '^WATCHER UP' "$LOGDIR/sni.log" || { echo "FAIL: the fake watcher never claimed the name"; fail=1; }
grep -q '^REGISTERED ' "$LOGDIR/sni.log" \
    || { echo "FAIL: no StatusNotifierItem registered"; fail=1; }
grep -q 'tray: visible=true' "$LOGDIR/app.log" || { echo "FAIL: the tray item is not visible"; fail=1; }
# A screenshot of an empty root window is a few hundred bytes; the panel is
# tens of kilobytes. This catches a run that drew nothing at all.
size=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
[ "$size" -gt 4000 ] || { echo "FAIL: screenshot is $size bytes, nothing was drawn"; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "SMOKE OK: tray registered over D-Bus, panel drawn ($size bytes)"
fi
exit "$fail"
