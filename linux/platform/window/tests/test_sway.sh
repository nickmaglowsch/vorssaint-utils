#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# wlroots backend against a real compositor: headless sway on the pixman
# renderer with foot windows. Listing, activation, minimize and close go
# through zwlr_foreign_toplevel_management_v1; move/resize goes through the
# sway IPC socket, which is the only channel on Wayland that can place another
# client's window.

set -euo pipefail

VS_WINDOW=${1:?usage: test_sway.sh /path/to/vs-window}
WORK=$(mktemp -d)
PIDS=()

cleanup() {
    [[ -n "${SWAYSOCK:-}" ]] && swaymsg exit >/dev/null 2>&1 || true
    for pid in "${PIDS[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

require() {
    command -v "$1" >/dev/null 2>&1 || { echo "SKIP: $1 not installed"; exit 77; }
}
require sway
require swaymsg
require foot

fail() { echo "FAIL: $*" >&2; exit 1; }

export XDG_RUNTIME_DIR="$WORK/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY

cat >"$WORK/sway.conf" <<'CONFIG'
# No bar, no keybindings, no idle: the test drives everything over IPC.
default_border none
focus_follows_mouse no
output HEADLESS-1 resolution 1920x1080 position 0 0
CONFIG

export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
export SWAYSOCK="$XDG_RUNTIME_DIR/sway-ipc.sock"

sway -d -c "$WORK/sway.conf" >"$WORK/sway.log" 2>&1 &
PIDS+=($!)

for _ in $(seq 1 60); do
    [[ -S "$SWAYSOCK" ]] && swaymsg -t get_version >/dev/null 2>&1 && break
    sleep 0.25
done
[[ -S "$SWAYSOCK" ]] || { sed -n '1,40p' "$WORK/sway.log"; fail "sway did not start"; }
export WAYLAND_DISPLAY=$(swaymsg -t get_outputs >/dev/null 2>&1 && ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
[[ -n "$WAYLAND_DISPLAY" ]] || fail "no wayland socket in $XDG_RUNTIME_DIR"
echo "sway up: WAYLAND_DISPLAY=$WAYLAND_DISPLAY $(swaymsg -t get_version -r | head -c 120)"

# ------------------------------------------------------------------- probe
"$VS_WINDOW" --backend wlr probe >"$WORK/probe.txt" || fail "probe failed"
cat "$WORK/probe.txt"
grep -q '^backend	wlr$' "$WORK/probe.txt" || fail "probe did not select wlr"
for capability in can_list can_activate can_close can_minimize has_live_events; do
    grep -q "^${capability}	yes$" "$WORK/probe.txt" || fail "wlr should advertise $capability"
done
# move/resize is a sway-IPC capability, not a protocol one; SWAYSOCK is set, so
# the backend must have found it and turned the capability on.
grep -q '^can_move_resize	yes$' "$WORK/probe.txt" \
    || fail "wlr should advertise can_move_resize when SWAYSOCK is set"

# The absence of ext-foreign-toplevel-list on this wlroots is expected and
# harmless; record which of the two protocols this compositor offers.
swaymsg -t get_version -r | head -c 200 >"$WORK/version.txt"

# -------------------------------------------------------------------- list
for name in alpha beta gamma; do
    foot --title="vorssaint-$name" --app-id="vorssaint-$name" -- sleep 600 \
        >"$WORK/foot-$name.log" 2>&1 &
    PIDS+=($!)
done
sleep 4

"$VS_WINDOW" --backend wlr list >"$WORK/list.txt" || fail "list failed"
cat "$WORK/list.txt"
for name in alpha beta gamma; do
    grep -q "vorssaint-$name" "$WORK/list.txt" || fail "list is missing vorssaint-$name"
done

# The protocol carries neither pid nor geometry; the sway IPC enrichment must
# fill both in, or the switcher has nothing to filter by display with.
awk -F'\t' '$2 == "vorssaint-alpha" {
    if ($4 <= 0) { print "pid not reported: " $4; exit 1 }
    if ($6 !~ /has-geometry/) { print "flags are " $6; exit 1 }
    if ($6 !~ /has-pid/) { print "flags are " $6; exit 1 }
    if ($7 != "1") { print "workspace is " $7; exit 1 }
    if ($8 != "HEADLESS-1") { print "output is " $8; exit 1 }
}' "$WORK/list.txt" || fail "alpha's record is incomplete"

# The foreign-toplevel protocol announces toplevels in creation order and never
# restacks them, so the backend must not claim the order is a stacking order.
grep -q '?$' "$WORK/list.txt" || fail "wlr stacking should be marked unreliable"

# Foreign-toplevel handles are numbered per connection, so an id printed by one
# invocation means nothing to the next. Address windows by @app_id instead,
# which vs-window resolves inside the process that acts on it.
ID_ALPHA=@vorssaint-alpha
ID_BETA=@vorssaint-beta
ID_GAMMA=@vorssaint-gamma

# ---------------------------------------------------------------- activate
"$VS_WINDOW" --backend wlr activate "$ID_ALPHA" || fail "activate failed"
sleep 1
FOCUSED=$(swaymsg -t get_tree | "$VS_PYTHON" -c '
import json, sys
def walk(node):
    if node.get("focused"):
        return node.get("app_id") or ""
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = walk(child)
        if found:
            return found
    return ""
print(walk(json.load(sys.stdin)))')
[[ "$FOCUSED" == "vorssaint-alpha" ]] || fail "sway focus is '$FOCUSED', wanted vorssaint-alpha"
"$VS_WINDOW" --backend wlr list | awk -F'\t' \
    '$2 == "vorssaint-alpha" && $6 ~ /focused/ { found = 1 } END { exit !found }' \
    || fail "alpha is not reported focused after activate"

# -------------------------------------------------------------- move/resize
MOVED=$("$VS_WINDOW" --backend wlr move "$ID_BETA" 200 150 800 600 2) \
    || fail "move failed: $MOVED"
echo "moved to $MOVED"
[[ "$MOVED" == "200,150 800x600" ]] || fail "backend read back $MOVED, wanted 200,150 800x600"

# Independent confirmation straight from sway.
RECT=$(swaymsg -t get_tree | "$VS_PYTHON" -c '
import json, sys
def walk(node):
    if node.get("app_id") == "vorssaint-beta":
        rect = node["rect"]
        return "%d,%d %dx%d" % (rect["x"], rect["y"], rect["width"], rect["height"])
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = walk(child)
        if found:
            return found
    return ""
print(walk(json.load(sys.stdin)))')
[[ "$RECT" == "200,150 800x600" ]] || fail "sway reports beta at $RECT"

MOVED2=$("$VS_WINDOW" --backend wlr move "$ID_BETA" 40 30 500 400 2) \
    || fail "second move failed: $MOVED2"
[[ "$MOVED2" == "40,30 500x400" ]] || fail "second move read back $MOVED2"

# ----------------------------------------------------------------- minimize
# wlroots has no minimized state of its own: sway acknowledges set_minimized
# and does nothing. The capability stays advertised because the protocol
# carries the request, but the call reads the compositor's own `state` event
# back and must report VS_ERR_NOT_APPLIED rather than a success it cannot back
# up — the same contract move_resize keeps.
if "$VS_WINDOW" --backend wlr minimize "$ID_GAMMA" 1 >"$WORK/minimize.txt" 2>&1; then
    # A future wlroots that grows a minimized state is allowed to pass here,
    # but then the window really has to be minimized.
    "$VS_WINDOW" --backend wlr list | awk -F'\t' \
        '$2 == "vorssaint-gamma" && $6 ~ /minimized/ { found = 1 } END { exit !found }' \
        || fail "minimize reported success without minimizing the window"
    echo "minimize: this compositor really minimizes"
else
    grep -q "did not change" "$WORK/minimize.txt" \
        || fail "wrong error for an ignored minimize: $(cat "$WORK/minimize.txt")"
    echo "minimize: sway acknowledged and ignored it, reported as not applied"
fi
"$VS_WINDOW" --backend wlr list | grep -q "vorssaint-gamma" || fail "gamma vanished from the list"

# ---------------------------------------------------------------- workspace
"$VS_WINDOW" --backend wlr workspace >"$WORK/workspace.txt" || fail "workspace read failed"
[[ "$(cat "$WORK/workspace.txt")" == "1" ]] || fail "current workspace is not 1"
"$VS_WINDOW" --backend wlr workspace 2 >"$WORK/workspace2.txt" || fail "workspace switch failed"
[[ "$(cat "$WORK/workspace2.txt")" == "2" ]] || fail "workspace did not become 2"
# A window left behind on workspace 1 must stop being on-screen.
"$VS_WINDOW" --backend wlr list | awk -F'\t' \
    '$2 == "vorssaint-alpha" { if ($6 !~ /on-current-workspace/ && $6 !~ /on-screen/) found = 1 } END { exit !found }' \
    || fail "alpha still claims to be on the current workspace"
"$VS_WINDOW" --backend wlr workspace 1 >/dev/null

# ------------------------------------------------------------ output removal
# A monitor unplug withdraws the wl_output global. The headless backend can do
# this for real, so the registry's global_remove path is exercised rather than
# reasoned about: the backend must drop the proxy, stop naming the dead output,
# and keep working.
swaymsg create_output >/dev/null || fail "create_output failed"
sleep 1
swaymsg -t get_outputs -r | grep -q HEADLESS-2 || fail "HEADLESS-2 was not created"
swaymsg '[app_id=vorssaint-gamma] move container to output HEADLESS-2' >/dev/null \
    || fail "could not move gamma to HEADLESS-2"
sleep 1
"$VS_WINDOW" --backend wlr list | awk -F'\t' \
    '$2 == "vorssaint-gamma" && $8 == "HEADLESS-2" { found = 1 } END { exit !found }' \
    || fail "gamma is not reported on HEADLESS-2"

"$VS_WINDOW" --backend wlr watch 6 >"$WORK/unplug.txt" 2>&1 &
UNPLUG_WATCH=$!
PIDS+=("$UNPLUG_WATCH")
sleep 1
swaymsg 'output HEADLESS-2 unplug' >/dev/null || fail "unplug failed"
sleep 3
kill "$UNPLUG_WATCH" 2>/dev/null || true
wait "$UNPLUG_WATCH" 2>/dev/null || true

swaymsg -t get_outputs -r | grep -q HEADLESS-2 && fail "HEADLESS-2 survived the unplug"
"$VS_WINDOW" --backend wlr list >"$WORK/after-unplug.txt" || fail "list failed after unplug"
cat "$WORK/after-unplug.txt"
grep -q HEADLESS-2 "$WORK/after-unplug.txt" && fail "a window still names the removed output"
grep -q "vorssaint-gamma" "$WORK/after-unplug.txt" || fail "gamma was lost with its output"
# The backend must still be usable, not merely not crashed.
"$VS_WINDOW" --backend wlr activate "$ID_GAMMA" || fail "activate broke after an output removal"

# ------------------------------------------------------------------- events
"$VS_WINDOW" --backend wlr watch 90 >"$WORK/events.txt" 2>&1 &
WATCH_PID=$!
PIDS+=("$WATCH_PID")
sleep 2

foot --title="vorssaint-delta" --app-id="vorssaint-delta" -- sleep 600 \
    >"$WORK/foot-delta.log" 2>&1 &
PIDS+=($!)

# Poll rather than sleep a fixed time: how long foot takes to map its first
# surface, and how long it takes to act on the close request, are both the
# machine's business, not the backend's.
for _ in $(seq 1 60); do
    "$VS_WINDOW" --backend wlr list | grep -q "vorssaint-delta" && break
    sleep 0.5
done
"$VS_WINDOW" --backend wlr list | grep -q "vorssaint-delta" \
    || fail "delta never appeared in the list"

"$VS_WINDOW" --backend wlr close @vorssaint-delta || fail "close failed"
for _ in $(seq 1 60); do
    "$VS_WINDOW" --backend wlr list | grep -q "vorssaint-delta" || break
    sleep 0.5
done
sleep 2
kill "$WATCH_PID" 2>/dev/null || true
wait "$WATCH_PID" 2>/dev/null || true

cat "$WORK/events.txt"
# Foreign-toplevel handles are per-connection, so the watcher's ids are its own
# and must not be compared with the ids the listing process saw. Find the id the
# watcher gave delta, then require its removal under that same id.
WATCH_DELTA=$(awk -F'\t' '$2 == "added" && $4 == "vorssaint-delta" {print $3; exit}' \
    "$WORK/events.txt")
[[ -n "$WATCH_DELTA" ]] || fail "no added event for delta"
grep -q "^event	removed	$WATCH_DELTA	" "$WORK/events.txt" || fail "no removed event for delta"
"$VS_WINDOW" --backend wlr list | grep -q "vorssaint-delta" \
    && fail "delta is still listed after close"

# An id the compositor never handed out must be NOT_FOUND, not a crash.
if "$VS_WINDOW" --backend wlr activate 999999 2>"$WORK/activate2.txt"; then
    fail "activating an unknown window reported success"
fi
grep -q "no such object" "$WORK/activate2.txt" || fail "wrong error for an unknown window"

echo "PASS: sway"
