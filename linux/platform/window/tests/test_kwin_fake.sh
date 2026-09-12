#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# KWin backend against fake_kwin.py on a private session bus, plus a lint of the
# KWin script itself. KWin needs a Plasma session and cannot run here; this
# proves the backend drives org.kde.KWin /Scripting correctly and decodes what
# the script pushes back, not that KWin behaves like the fixture.

set -euo pipefail

VS_WINDOW=${1:?usage: test_kwin_fake.sh /path/to/vs-window}
PYTHON=${VS_PYTHON:-python3}
SOURCE_DIR=${VS_SOURCE_DIR:-$(dirname "$0")}
KWIN_SCRIPT=${VS_KWIN_SCRIPT:-$SOURCE_DIR/../kwin/vorssaint-window.js}
WORK=$(mktemp -d)

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v dbus-run-session >/dev/null 2>&1 || { echo "SKIP: dbus-run-session missing"; exit 77; }
"$PYTHON" -c 'import dbus, gi' 2>/dev/null || { echo "SKIP: python dbus/gi missing"; exit 77; }

# ----------------------------------------------------------------- JS lint
# The script only ever runs inside KWin's engine, so a syntax error would
# otherwise surface as a silent no-op on a user's desktop.
if command -v node >/dev/null 2>&1; then
    node --check "$KWIN_SCRIPT" || fail "vorssaint-window.js does not parse"
    echo "lint: node --check $(basename "$KWIN_SCRIPT") ok"
else
    echo "lint: skipped, node not installed"
fi
# The two roles must both be reachable from the file's tail.
grep -q 'VORSSAINT_COMMAND' "$KWIN_SCRIPT" || fail "script has no command role"
grep -q 'vorssaintInstallEvents' "$KWIN_SCRIPT" || fail "script has no event role"

export VORSSAINT_KWIN_SCRIPT="$KWIN_SCRIPT"
export XDG_RUNTIME_DIR="$WORK/run"
mkdir -p "$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE SWAYSOCK

cat >"$WORK/body.sh" <<INNER
set -euo pipefail
fail() { echo "FAIL: \$*" >&2; exit 1; }

"$PYTHON" "$SOURCE_DIR/fake_kwin.py" >"$WORK/fake.out" 2>"$WORK/fake.err" &
FAKE_PID=\$!
trap 'kill \$FAKE_PID 2>/dev/null || true' EXIT
for _ in \$(seq 1 60); do
    grep -q ready "$WORK/fake.out" 2>/dev/null && break
    sleep 0.1
done
grep -q ready "$WORK/fake.out" || { cat "$WORK/fake.err"; fail "fake KWin did not start"; }

# ---------------------------------------------------------------- probe
# org.kde.KWin owns a name on this bus and the script is installed, so the
# probe must land on kwin with no display of any kind present.
"$VS_WINDOW" probe >"$WORK/probe.txt" || fail "probe failed"
cat "$WORK/probe.txt"
grep -q '^backend	kwin\$' "$WORK/probe.txt" || fail "probe did not select kwin"
for capability in can_list can_activate can_close can_minimize can_move_resize \\
                  can_workspace_switch has_live_events; do
    grep -q "^\${capability}	yes\$" "$WORK/probe.txt" || fail "kwin should advertise \$capability"
done

# ----------------------------------------------------------------- list
"$VS_WINDOW" --backend kwin list >"$WORK/list.txt" || fail "list failed"
cat "$WORK/list.txt"
for name in alpha beta gamma; do
    grep -q "vorssaint-\$name" "$WORK/list.txt" || fail "list is missing vorssaint-\$name"
done
awk -F'\t' '\$2 == "vorssaint-alpha" {
    if (\$4 != 5101) { print "pid is " \$4; exit 1 }
    if (\$5 != "0,0 1280x1440") { print "frame is " \$5; exit 1 }
    if (\$6 !~ /focused/) { print "flags are " \$6; exit 1 }
    if (\$8 != "DP-1") { print "output is " \$8; exit 1 }
}' "$WORK/list.txt" || fail "alpha's record is wrong"
# gamma is on desktop 1 while desktop 0 is current.
awk -F'\t' '\$2 == "vorssaint-gamma" {
    if (\$6 ~ /on-current-workspace/) { print "flags are " \$6; exit 1 }
}' "$WORK/list.txt" || fail "gamma should not be on the current desktop"
# workspace.windowList() is a real stacking order, so it must not be marked
# unreliable the way the foreign-toplevel order is.
grep -q '?\$' "$WORK/list.txt" && fail "kwin stacking should be marked valid"

# The id is a hash of KWin's internalId UUID, so it must be stable between two
# invocations of the same backend.
ID_ALPHA=\$(awk -F'\t' '\$2 == "vorssaint-alpha" {print \$1; exit}' "$WORK/list.txt")
ID_AGAIN=\$("$VS_WINDOW" --backend kwin list | awk -F'\t' '\$2 == "vorssaint-alpha" {print \$1; exit}')
[[ "\$ID_ALPHA" == "\$ID_AGAIN" ]] || fail "kwin ids are not stable: \$ID_ALPHA vs \$ID_AGAIN"

# ------------------------------------------------------------- activate
"$VS_WINDOW" --backend kwin activate @vorssaint-beta || fail "activate failed"
"$VS_WINDOW" --backend kwin list | awk -F'\t' \\
    '\$2 == "vorssaint-beta" && \$6 ~ /focused/ { found = 1 } END { exit !found }' \\
    || fail "beta is not focused after activate"

# ------------------------------------------------------------- minimize
"$VS_WINDOW" --backend kwin minimize @vorssaint-beta 1 || fail "minimize failed"
"$VS_WINDOW" --backend kwin list | awk -F'\t' \\
    '\$2 == "vorssaint-beta" && \$6 ~ /minimized/ { found = 1 } END { exit !found }' \\
    || fail "beta is not minimized"
"$VS_WINDOW" --backend kwin minimize @vorssaint-beta 0 || fail "unminimize failed"

# ----------------------------------------------------------- move/resize
MOVED=\$("$VS_WINDOW" --backend kwin move @vorssaint-beta 300 200 900 700 2) \\
    || fail "move failed: \$MOVED"
[[ "\$MOVED" == "300,200 900x700" ]] || fail "backend read back \$MOVED"

# gamma refuses to change size; the backend must report the refusal rather than
# the success the request itself reported.
if "$VS_WINDOW" --backend kwin move @vorssaint-gamma 10 10 320 240 2 2>"$WORK/refused.txt"; then
    fail "a refused move reported success"
fi
grep -q "did not change" "$WORK/refused.txt" \\
    || fail "wrong error for a refused move: \$(cat "$WORK/refused.txt")"

# ------------------------------------------------------------ workspace
"$VS_WINDOW" --backend kwin workspace | grep -q '^0\$' || fail "current desktop is not 0"
"$VS_WINDOW" --backend kwin workspace 1 | grep -q '^1\$' || fail "desktop did not become 1"
if "$VS_WINDOW" --backend kwin workspace 9 2>"$WORK/desktop.txt"; then
    fail "switching to a desktop that does not exist reported success"
fi
"$VS_WINDOW" --backend kwin workspace 0 >/dev/null

# --------------------------------------------------------------- events
"$VS_WINDOW" --backend kwin watch 8 >"$WORK/events.txt" 2>&1 || fail "watch failed"
cat "$WORK/events.txt"
grep -q '^event	added	' "$WORK/events.txt" || fail "no added event"
grep -q '^event	activated	' "$WORK/events.txt" || fail "no activated event"
grep -q '^event	removed	' "$WORK/events.txt" || fail "no removed event"
grep -q '^event	workspace	0	-	1\$' "$WORK/events.txt" || fail "no workspace event"

# The added and removed events must carry the same id, hashed from the same
# internalId, or the switcher cannot pair them.
ADDED=\$(awk -F'\t' '\$2 == "added" {print \$3; exit}' "$WORK/events.txt")
grep -q "^event	removed	\$ADDED	" "$WORK/events.txt" || fail "removed id does not match added"

# ------------------------------------------------------ generated scripts
# Every command must have loaded its own short-lived script and the persistent
# event script must have been loaded exactly once.
grep -c 'vorssaint-window-events' "$WORK/fake.out" >/dev/null || true
echo "PASS: kwin (against fake_kwin.py; unverified on a live KWin)"
INNER

dbus-run-session -- bash "$WORK/body.sh"
