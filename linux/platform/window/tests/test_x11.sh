#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# X11 backend against a real X server and a real window manager: Xvfb + openbox
# with a few xterm windows. Everything asserted here is read back from the
# server, never inferred from a call returning success.

set -euo pipefail

VS_WINDOW=${1:?usage: test_x11.sh /path/to/vs-window}
DISPLAY_NUMBER=${VS_TEST_DISPLAY:-:97}
WORK=$(mktemp -d)
PIDS=()

cleanup() {
    for pid in "${PIDS[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

require() {
    command -v "$1" >/dev/null 2>&1 || { echo "SKIP: $1 not installed"; exit 77; }
}
require Xvfb
require openbox
require xterm
require xmessage

fail() { echo "FAIL: $*" >&2; exit 1; }

Xvfb "$DISPLAY_NUMBER" -screen 0 1280x800x24 >"$WORK/xvfb.log" 2>&1 &
PIDS+=($!)
export DISPLAY=$DISPLAY_NUMBER
for _ in $(seq 1 50); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.2
done
xdpyinfo >/dev/null 2>&1 || fail "Xvfb did not come up on $DISPLAY_NUMBER"

openbox >"$WORK/openbox.log" 2>&1 &
PIDS+=($!)
sleep 1

# The backend refuses a display with no EWMH window manager, so probing before
# any window exists already proves the window manager was detected.
"$VS_WINDOW" --backend x11 probe >"$WORK/probe.txt" || fail "probe failed"
grep -q '^backend	x11$' "$WORK/probe.txt" || fail "probe did not select x11"
for capability in can_list can_activate can_close can_minimize can_move_resize \
                  can_workspace_switch has_live_events; do
    grep -q "^${capability}	yes$" "$WORK/probe.txt" || fail "x11 should advertise $capability"
done

for name in alpha beta gamma; do
    xterm -T "vorssaint-$name" -e "sleep 600" >/dev/null 2>&1 &
    PIDS+=($!)
done
sleep 2

# ---------------------------------------------------------------------- list
"$VS_WINDOW" --backend x11 list >"$WORK/list.txt" || fail "list failed"
cat "$WORK/list.txt"
for name in alpha beta gamma; do
    grep -q "vorssaint-$name" "$WORK/list.txt" || fail "list is missing vorssaint-$name"
done
# pid, app_id and geometry must be real, not placeholders.
awk -F'\t' '$3 ~ /vorssaint-alpha/ {
    if ($4 <= 0) { print "pid not reported"; exit 1 }
    if ($2 != "xterm") { print "app_id is " $2; exit 1 }
    if ($5 !~ /[0-9]+x[0-9]+$/) { print "geometry is " $5; exit 1 }
    if ($6 !~ /has-geometry/) { print "flags are " $6; exit 1 }
    if ($6 !~ /has-pid/) { print "flags are " $6; exit 1 }
}' "$WORK/list.txt" || fail "alpha's record is incomplete"

# _NET_CLIENT_LIST_STACKING is a real stacking order, so it must be marked valid.
grep -q '?$' "$WORK/list.txt" && fail "x11 stacking should be marked valid"

ID_ALPHA=$(awk -F'\t' '$3 ~ /vorssaint-alpha/ {print $1; exit}' "$WORK/list.txt")
ID_BETA=$(awk -F'\t' '$3 ~ /vorssaint-beta/ {print $1; exit}' "$WORK/list.txt")
ID_GAMMA=$(awk -F'\t' '$3 ~ /vorssaint-gamma/ {print $1; exit}' "$WORK/list.txt")
[[ -n "$ID_ALPHA" && -n "$ID_BETA" && -n "$ID_GAMMA" ]] || fail "could not read window ids"

# ------------------------------------------------------------------ activate
"$VS_WINDOW" --backend x11 activate "$ID_ALPHA" || fail "activate failed"
sleep 1
ACTIVE=$(xprop -root _NET_ACTIVE_WINDOW | awk '{print $NF}')
ACTIVE_DECIMAL=$((ACTIVE))
[[ "$ACTIVE_DECIMAL" == "$ID_ALPHA" ]] || fail "active window is $ACTIVE_DECIMAL, wanted $ID_ALPHA"
"$VS_WINDOW" --backend x11 list | awk -F'\t' -v id="$ID_ALPHA" \
    '$1 == id && $6 ~ /focused/ { found = 1 } END { exit !found }' \
    || fail "alpha is not reported focused after activate"

# --------------------------------------------------------------- move/resize
# Read back through the backend and, independently, through xwininfo.
# xmessage has no resize increments, so an obeyed request lands exactly.
xmessage -name vorssaint-box "vorssaint" >/dev/null 2>&1 &
PIDS+=($!)
sleep 2
ID_BOX=$("$VS_WINDOW" --backend x11 list | awk -F'\t' '$2 == "vorssaint-box" {print $1; exit}')
[[ -n "$ID_BOX" ]] || fail "xmessage window never appeared"

MOVED=$("$VS_WINDOW" --backend x11 move "$ID_BOX" 120 90 640 480 2) \
    || fail "move failed: $MOVED"
echo "moved to $MOVED"
[[ "$MOVED" == "120,90 640x480" ]] || fail "backend read back $MOVED, wanted 120,90 640x480"

# Independent confirmation: the client rectangle must sit inside the requested
# frame, inset by exactly the decorations the window manager advertises.
eval "$(xwininfo -id "$ID_BOX" | awk '
    /Absolute upper-left X/ { print "X=" $NF }
    /Absolute upper-left Y/ { print "Y=" $NF }
    /Width:/ { print "W=" $NF }
    /Height:/ { print "H=" $NF }')"
eval "$(xprop -id "$ID_BOX" _NET_FRAME_EXTENTS | sed 's/.*= //' | awk -F', *' \
    '{ print "L=" $1 "; R=" $2 "; T=" $3 "; B=" $4 }')"
echo "xwininfo client rect: $X,$Y ${W}x${H}; extents left=$L right=$R top=$T bottom=$B"
(( X == 120 + L && Y == 90 + T && W == 640 - L - R && H == 480 - T - B )) \
    || fail "client rect $X,$Y ${W}x${H} does not match frame 120,90 640x480"

# A second placement must not inherit the first: this is where a backend that
# reports success without reading back starts lying.
MOVED2=$("$VS_WINDOW" --backend x11 move "$ID_BOX" 300 200 400 300 2) \
    || fail "second move failed: $MOVED2"
[[ "$MOVED2" == "300,200 400x300" ]] || fail "second move read back $MOVED2"

# An app that refuses the size must be reported as a refusal, not a success:
# xterm quantises its size to whole character cells, so an arbitrary frame is
# unreachable and move_resize owes the caller VS_ERR_NOT_APPLIED.
if "$VS_WINDOW" --backend x11 move "$ID_BETA" 120 90 640 480 2 >"$WORK/refused.txt" 2>&1; then
    fail "xterm accepted an off-grid size; the read-back is not being checked"
fi
grep -q "did not change" "$WORK/refused.txt" \
    || fail "wrong error for a size the app refused: $(cat "$WORK/refused.txt")"
# ... and it must still have moved as far as the app allowed.
"$VS_WINDOW" --backend x11 geometry "$ID_BETA" | grep -q '^120,90 ' \
    || fail "xterm did not move to the requested origin"

# --------------------------------------------------------------- minimize
"$VS_WINDOW" --backend x11 minimize "$ID_GAMMA" 1 || fail "minimize failed"
sleep 1
"$VS_WINDOW" --backend x11 list | awk -F'\t' -v id="$ID_GAMMA" \
    '$1 == id { if ($6 ~ /minimized/ && $6 !~ /on-screen/) found = 1 } END { exit !found }' \
    || fail "gamma is not reported minimized"
"$VS_WINDOW" --backend x11 minimize "$ID_GAMMA" 0 || fail "unminimize failed"
sleep 1
"$VS_WINDOW" --backend x11 list | awk -F'\t' -v id="$ID_GAMMA" \
    '$1 == id { if ($6 !~ /minimized/) found = 1 } END { exit !found }' \
    || fail "gamma is still minimized after restore"

# --------------------------------------------------------------- workspaces
DESKTOPS=$(xprop -root _NET_NUMBER_OF_DESKTOPS | awk '{print $NF}')
if (( DESKTOPS > 1 )); then
    "$VS_WINDOW" --backend x11 workspace 1 >"$WORK/workspace.txt" || fail "workspace switch failed"
    [[ "$(cat "$WORK/workspace.txt")" == "1" ]] || fail "workspace did not become 1"
    "$VS_WINDOW" --backend x11 workspace 0 >/dev/null
fi

# ------------------------------------------------------------------- events
# watch in the background, then close a window through the backend; the removal
# must arrive as an event and the window must really be gone.
"$VS_WINDOW" --backend x11 watch 12 >"$WORK/events.txt" 2>&1 &
WATCH_PID=$!
PIDS+=("$WATCH_PID")
sleep 2

xterm -T "vorssaint-delta" -e "sleep 600" >/dev/null 2>&1 &
PIDS+=($!)
sleep 3

ID_DELTA=$("$VS_WINDOW" --backend x11 list | awk -F'\t' '$3 ~ /vorssaint-delta/ {print $1; exit}')
[[ -n "$ID_DELTA" ]] || fail "delta never appeared in the list"
"$VS_WINDOW" --backend x11 close "$ID_DELTA" || fail "close failed"
sleep 3
wait "$WATCH_PID" || true

cat "$WORK/events.txt"
grep -q "^event	added	$ID_DELTA" "$WORK/events.txt" || fail "no added event for delta"
grep -q "^event	removed	$ID_DELTA" "$WORK/events.txt" || fail "no removed event for delta"
"$VS_WINDOW" --backend x11 list | grep -q "vorssaint-delta" \
    && fail "delta is still listed after close"

# Closing an id the server no longer knows must be NOT_FOUND, not a crash.
if "$VS_WINDOW" --backend x11 close "$ID_DELTA" 2>"$WORK/close2.txt"; then
    fail "closing a dead window reported success"
fi
grep -q "no such object" "$WORK/close2.txt" || fail "wrong error for a dead window"

echo "PASS: x11"
