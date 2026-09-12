#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# GNOME backend against fake_window_bridge.py, which implements
# org.vorssaint.WindowBridge the way the vorssaint-bridge Shell extension
# (WP-C2) must. GNOME Shell cannot run here; this pins the interface and proves
# the client side decodes and read-back-verifies it.
#
# The assertions below are also what proves the real extension interoperable:
# VS_BRIDGE_CMD replaces the Python fake with any other implementation of the
# interface, and WP-C2's `window_gnome_extension` test points it at the
# extension's own D-Bus layer running under gjs. Keep every assertion here
# implementation-neutral, so both halves are held to the same contract.

set -euo pipefail

VS_WINDOW=${1:?usage: test_gnome_fake.sh /path/to/vs-window}
PYTHON=${VS_PYTHON:-python3}
SOURCE_DIR=${VS_SOURCE_DIR:-$(dirname "$0")}
BRIDGE_CMD=${VS_BRIDGE_CMD:-}
BRIDGE_LABEL=${VS_BRIDGE_LABEL:-fake_window_bridge.py}
WORK=$(mktemp -d)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v dbus-run-session >/dev/null 2>&1 || { echo "SKIP: dbus-run-session missing"; exit 77; }
if [[ -z "$BRIDGE_CMD" ]]; then
    "$PYTHON" -c 'import dbus, gi' 2>/dev/null || { echo "SKIP: python dbus/gi missing"; exit 77; }
    BRIDGE_CMD="$PYTHON $SOURCE_DIR/fake_window_bridge.py"
fi
command -v "${BRIDGE_CMD%% *}" >/dev/null 2>&1 \
    || { echo "SKIP: ${BRIDGE_CMD%% *} missing"; exit 77; }

export XDG_RUNTIME_DIR="$WORK/run"
mkdir -p "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE SWAYSOCK

cat >"$WORK/body.sh" <<INNER
set -euo pipefail
fail() { echo "FAIL: \$*" >&2; exit 1; }

start_bridge() {
    $BRIDGE_CMD "\$@" >"$WORK/fake.out" 2>"$WORK/fake.err" &
    BRIDGE_PID=\$!
    for _ in \$(seq 1 60); do
        grep -q ready "$WORK/fake.out" 2>/dev/null && break
        sleep 0.1
    done
    grep -q ready "$WORK/fake.out" || { cat "$WORK/fake.err"; fail "fake bridge did not start"; }
}
stop_bridge() { kill \$BRIDGE_PID 2>/dev/null || true; wait \$BRIDGE_PID 2>/dev/null || true; }

# With no bridge on the bus the GNOME backend must decline, not guess.
if "$VS_WINDOW" --backend gnome probe >"$WORK/noprobe.txt" 2>&1; then
    fail "gnome backend started with no bridge on the bus"
fi

start_bridge
trap stop_bridge EXIT

# ---------------------------------------------------------------- probe
"$VS_WINDOW" probe >"$WORK/probe.txt" || fail "probe failed"
cat "$WORK/probe.txt"
grep -q '^backend	gnome\$' "$WORK/probe.txt" || fail "probe did not select gnome"
for capability in can_list can_activate can_close can_minimize can_move_resize \\
                  can_workspace_switch has_live_events; do
    grep -q "^\${capability}	yes\$" "$WORK/probe.txt" || fail "gnome should advertise \$capability"
done

# ----------------------------------------------------------------- list
"$VS_WINDOW" --backend gnome list >"$WORK/list.txt" || fail "list failed"
cat "$WORK/list.txt"
awk -F'\t' '\$2 == "firefox" {
    if (\$1 != 102) { print "id is " \$1; exit 1 }
    if (\$3 != "Vorssaint on GNOME") { print "title is " \$3; exit 1 }
    if (\$4 != 6102) { print "pid is " \$4; exit 1 }
    if (\$5 != "100,60 1600x900") { print "frame is " \$5; exit 1 }
    if (\$6 !~ /focused/) { print "flags are " \$6; exit 1 }
    if (\$8 != "XWAYLAND0") { print "output is " \$8; exit 1 }
}' "$WORK/list.txt" || fail "firefox's record is wrong"
# app_id and app_name are distinct fields and must not be collapsed.
grep -q 'org.gnome.Nautilus	Home' "$WORK/list.txt" \
    || fail "app_id and title are not both decoded"
# The bridge enumerates in Mutter's stacking order.
grep -q '?\$' "$WORK/list.txt" && fail "gnome stacking should be marked valid"

# ------------------------------------------------------------- activate
"$VS_WINDOW" --backend gnome activate 101 || fail "activate failed"
"$VS_WINDOW" --backend gnome list | awk -F'\t' \\
    '\$1 == 101 && \$6 ~ /focused/ { found = 1 } END { exit !found }' \\
    || fail "101 is not focused after activate"

# ------------------------------------------------------------- minimize
"$VS_WINDOW" --backend gnome minimize 101 1 || fail "minimize failed"
"$VS_WINDOW" --backend gnome list | awk -F'\t' \\
    '\$1 == 101 { if (\$6 ~ /minimized/ && \$6 !~ /on-screen/) found = 1 } END { exit !found }' \\
    || fail "101 is not minimized"

# ----------------------------------------------------------- move/resize
MOVED=\$("$VS_WINDOW" --backend gnome move 102 300 200 900 700 2) || fail "move failed: \$MOVED"
[[ "\$MOVED" == "300,200 900x700" ]] || fail "backend read back \$MOVED"

# Text Editor refuses; the bridge still returns from MoveResize without error,
# so only the read-back can tell the caller the truth.
if "$VS_WINDOW" --backend gnome move 103 10 10 320 240 2 2>"$WORK/refused.txt"; then
    fail "a refused move reported success"
fi
grep -q "did not change" "$WORK/refused.txt" \\
    || fail "wrong error for a refused move: \$(cat "$WORK/refused.txt")"

# ------------------------------------------------------------ workspace
"$VS_WINDOW" --backend gnome workspace | grep -q '^0\$' || fail "current workspace is not 0"
"$VS_WINDOW" --backend gnome workspace 1 | grep -q '^1\$' || fail "workspace did not become 1"

# --------------------------------------------------------------- errors
if "$VS_WINDOW" --backend gnome close 999 2>"$WORK/close.txt"; then
    fail "closing an unknown window reported success"
fi
grep -q "no such object" "$WORK/close.txt" || fail "wrong error for an unknown window"

# --------------------------------------------------------------- events
stop_bridge
start_bridge --events
"$VS_WINDOW" --backend gnome watch 6 >"$WORK/events.txt" 2>&1 || fail "watch failed"
cat "$WORK/events.txt"
grep -q '^event	added	104	' "$WORK/events.txt" || fail "no added event"
grep -q '^event	activated	104	' "$WORK/events.txt" || fail "no activated event"
grep -q '^event	removed	104	' "$WORK/events.txt" || fail "no removed event"
grep -q '^event	workspace	0	-	1\$' "$WORK/events.txt" || fail "no workspace event"

echo "PASS: gnome (against $BRIDGE_LABEL; unverified on a live GNOME Shell)"
INNER

dbus-run-session -- bash "$WORK/body.sh"
