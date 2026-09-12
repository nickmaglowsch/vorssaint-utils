#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# Hyprland backend against fake_hyprland.py. Hyprland needs a DRM device, so it
# cannot run here; this proves the backend speaks the documented protocol
# correctly, not that Hyprland answers the way the fixture does.

set -euo pipefail

VS_WINDOW=${1:?usage: test_hyprland_fake.sh /path/to/vs-window}
PYTHON=${VS_PYTHON:-python3}
SOURCE_DIR=${VS_SOURCE_DIR:-$(dirname "$0")}
WORK=$(mktemp -d)
FAKE_PID=

cleanup() {
    [[ -n "$FAKE_PID" ]] && kill "$FAKE_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

export XDG_RUNTIME_DIR="$WORK/run"
export HYPRLAND_INSTANCE_SIGNATURE="fixture_deadbeef"
SOCKET_DIR="$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE"
mkdir -p "$SOCKET_DIR"

"$PYTHON" "$SOURCE_DIR/fake_hyprland.py" "$SOCKET_DIR" >"$WORK/fake.out" 2>"$WORK/fake.err" &
FAKE_PID=$!
for _ in $(seq 1 50); do
    grep -q ready "$WORK/fake.out" 2>/dev/null && break
    sleep 0.1
done
grep -q ready "$WORK/fake.out" || { cat "$WORK/fake.err"; fail "fake Hyprland did not start"; }

# ------------------------------------------------------------------- probe
# With HYPRLAND_INSTANCE_SIGNATURE set and the sockets answering, the probe must
# pick hyprland even though no Wayland or X display exists.
"$VS_WINDOW" probe >"$WORK/probe.txt" || fail "probe failed"
cat "$WORK/probe.txt"
grep -q '^backend	hyprland$' "$WORK/probe.txt" || fail "probe did not select hyprland"
for capability in can_list can_activate can_close can_move_resize can_workspace_switch \
                  has_live_events; do
    grep -q "^${capability}	yes$" "$WORK/probe.txt" || fail "hyprland should advertise $capability"
done
# Hyprland has no minimized state, and the backend must say so rather than
# faking one with a special workspace.
grep -q '^can_minimize	no$' "$WORK/probe.txt" || fail "hyprland must not claim can_minimize"

# -------------------------------------------------------------------- list
"$VS_WINDOW" --backend hyprland list >"$WORK/list.txt" || fail "list failed"
cat "$WORK/list.txt"
for name in alpha beta gamma; do
    grep -q "vorssaint-$name" "$WORK/list.txt" || fail "list is missing vorssaint-$name"
done

# The address is the id, parsed out of the "0x..." string.
grep -q "^$((0x55a1b2c3d400))	vorssaint-alpha" "$WORK/list.txt" \
    || fail "alpha's id is not its address"
# Monitor index resolved to a name through j/monitors.
awk -F'\t' '$2 == "vorssaint-alpha" {
    if ($4 != 4101) { print "pid is " $4; exit 1 }
    if ($5 != "0,0 1280x1440") { print "frame is " $5; exit 1 }
    if ($8 != "DP-1") { print "output is " $8; exit 1 }
    if ($6 !~ /focused/) { print "alpha should be focused (focusHistoryID 0): " $6; exit 1 }
}' "$WORK/list.txt" || fail "alpha's record is wrong"
# gamma is hidden and on workspace 2, so it is neither on screen nor on the
# current workspace; this is what the switcher greys out.
awk -F'\t' '$2 == "vorssaint-gamma" {
    if ($6 !~ /minimized/) { print "gamma should be minimized: " $6; exit 1 }
    if ($6 ~ /on-screen/) { print "gamma should not be on screen: " $6; exit 1 }
    if ($6 ~ /on-current-workspace/) { print "gamma is on workspace 2: " $6; exit 1 }
    if ($8 != "HDMI-A-1") { print "output is " $8; exit 1 }
}' "$WORK/list.txt" || fail "gamma's record is wrong"

# ---------------------------------------------------------------- activate
"$VS_WINDOW" --backend hyprland activate @vorssaint-beta || fail "activate failed"
"$VS_WINDOW" --backend hyprland list | awk -F'\t' \
    '$2 == "vorssaint-beta" && $6 ~ /focused/ { found = 1 } END { exit !found }' \
    || fail "beta is not focused after activate"

# -------------------------------------------------------------- move/resize
MOVED=$("$VS_WINDOW" --backend hyprland move @vorssaint-beta 300 200 900 700 2) \
    || fail "move failed: $MOVED"
[[ "$MOVED" == "300,200 900x700" ]] || fail "backend read back $MOVED"
"$VS_WINDOW" --backend hyprland geometry @vorssaint-beta | grep -q '^300,200 900x700$' \
    || fail "geometry does not agree with the move"

# ----------------------------------------------------------------- minimize
if "$VS_WINDOW" --backend hyprland minimize @vorssaint-beta 1 2>"$WORK/minimize.txt"; then
    fail "hyprland reported a minimize it cannot do"
fi
grep -q "unsupported" "$WORK/minimize.txt" || fail "wrong error for minimize: $(cat "$WORK/minimize.txt")"

# ---------------------------------------------------------------- workspace
"$VS_WINDOW" --backend hyprland workspace | grep -q '^1$' || fail "current workspace is not 1"
"$VS_WINDOW" --backend hyprland workspace 2 | grep -q '^2$' || fail "workspace did not become 2"
"$VS_WINDOW" --backend hyprland workspace 1 >/dev/null

# ------------------------------------------------------------------- events
# The fake scripts a window appearing, renaming itself and closing, over
# .socket2.sock, in Hyprland's documented line format.
VS_FAKE_HYPR_DELAY=1.0 "$VS_WINDOW" --backend hyprland watch 8 >"$WORK/events.txt" 2>&1 \
    || fail "watch failed"
cat "$WORK/events.txt"
DELTA=$((0x55a1b2c3e000))
grep -q "^event	added	$DELTA	" "$WORK/events.txt" || fail "no added event for delta"
grep -q "^event	changed	$DELTA	" "$WORK/events.txt" || fail "no changed event for delta"
grep -q "^event	removed	$DELTA	" "$WORK/events.txt" || fail "no removed event for delta"

# ------------------------------------------------------------------ errors
if "$VS_WINDOW" --backend hyprland close 0xdeadbeef 2>"$WORK/close.txt"; then
    fail "closing an unknown address reported success"
fi
grep -q "no such object" "$WORK/close.txt" || fail "wrong error for an unknown address"

# A tiled window only obeys an absolute position once it is floating, which is
# why move_resize sends `setfloating` first. Whichever way this particular
# compositor goes, the answer must be honest: either the window really is at the
# requested frame, or the call reports that it did not change.
if "$VS_WINDOW" --backend hyprland move @vorssaint-alpha 10 10 320 240 2 >"$WORK/tiled.txt" 2>&1; then
    grep -q '^10,10 320x240$' "$WORK/tiled.txt" \
        || fail "move reported success but read back $(cat "$WORK/tiled.txt")"
else
    grep -q "did not change" "$WORK/tiled.txt" \
        || fail "wrong error for a tiled window: $(cat "$WORK/tiled.txt")"
fi

echo "PASS: hyprland (against fake_hyprland.py; unverified on a live Hyprland)"
