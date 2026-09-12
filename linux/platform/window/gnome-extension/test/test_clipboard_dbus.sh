#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The clipboard half of org.vorssaint.WindowBridge (WP-A8) over a real session
# bus: run_bridge.js owns the name with the extension's own lib/service.js, and
# gdbus is the client. There is no C client for these members yet, so this is
# what pins their marshalling — `ay` in and out, `as`, and the asynchronous
# read path — before WP-A8 writes the other side.
#
# Mutter is not involved: the compositor behind the service is a fixture, so
# what is proven here is the D-Bus surface and the service's own logic, not
# Meta.Selection. See GNOME_BRIDGE.md for what remains unverified.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v dbus-run-session >/dev/null 2>&1 || { echo "SKIP: dbus-run-session missing"; exit 77; }
command -v gjs >/dev/null 2>&1 || { echo "SKIP: gjs missing"; exit 77; }
command -v gdbus >/dev/null 2>&1 || { echo "SKIP: gdbus missing"; exit 77; }

cat >"$WORK/body.sh" <<INNER
set -euo pipefail
fail() { echo "FAIL: \$*" >&2; exit 1; }

gjs -m "$HERE/run_bridge.js" >"$WORK/bridge.out" 2>"$WORK/bridge.err" &
BRIDGE=\$!
trap 'kill \$BRIDGE 2>/dev/null || true' EXIT
for _ in \$(seq 1 60); do
    grep -q ready "$WORK/bridge.out" 2>/dev/null && break
    sleep 0.1
done
grep -q ready "$WORK/bridge.out" || { cat "$WORK/bridge.err"; fail "bridge did not start"; }

call() {
    gdbus call --session --dest org.vorssaint.WindowBridge \\
        --object-path /org/vorssaint/WindowBridge \\
        --method org.vorssaint.WindowBridge."\$@"
}

# The interface really is on the bus, with the members the XML declares.
gdbus introspect --session --dest org.vorssaint.WindowBridge \\
    --object-path /org/vorssaint/WindowBridge >"$WORK/introspect.txt" \\
    || fail "introspection failed"
for member in ClipboardMimeTypes ClipboardRead ClipboardWrite ClipboardClear ClipboardChanged; do
    grep -q "\$member" "$WORK/introspect.txt" || fail "\$member is not on the bus"
done

# Version is a property, so the Capabilities page can tell a stale install.
gdbus call --session --dest org.vorssaint.WindowBridge \\
    --object-path /org/vorssaint/WindowBridge \\
    --method org.freedesktop.DBus.Properties.Get org.vorssaint.WindowBridge Version \\
    | grep -q 'uint32 1' || fail "Version is not readable as a property"

# Nothing on the clipboard yet.
call ClipboardMimeTypes 0 | grep -q '(@as \[\],)' || fail "a fresh clipboard is not empty"

# Watch the change signal while we write.
gdbus monitor --session --dest org.vorssaint.WindowBridge >"$WORK/signals.txt" 2>&1 &
MONITOR=\$!
sleep 0.5

# ay in: "hi" is 0x68 0x69.
call ClipboardWrite 0 '"text/plain;charset=utf-8"' '[0x68, 0x69]' >/dev/null \\
    || fail "ClipboardWrite failed"
call ClipboardMimeTypes 0 | grep -q 'text/plain;charset=utf-8' \\
    || fail "the written mime type is not reported"

# ay out, through the asynchronous path.
READ=\$(call ClipboardRead 0 '"text/plain;charset=utf-8"')
[[ "\$READ" == *"[byte 0x68, 0x69]"* ]] || fail "ClipboardRead returned \$READ"

# A type nobody offers is an error, not empty data.
if call ClipboardRead 0 '"image/png"' >"$WORK/missing.txt" 2>&1; then
    fail "reading an absent mime type reported success"
fi

call ClipboardClear 0 >/dev/null || fail "ClipboardClear failed"
call ClipboardMimeTypes 0 | grep -q '(@as \[\],)' || fail "the clipboard was not cleared"

sleep 0.5
kill \$MONITOR 2>/dev/null || true
grep -q 'ClipboardChanged (uint32 0,' "$WORK/signals.txt" \\
    || { cat "$WORK/signals.txt"; fail "no ClipboardChanged signal"; }

# The primary selection is a separate selection, not an alias.
call ClipboardWrite 1 '"text/plain"' '[0x41]' >/dev/null || fail "primary write failed"
call ClipboardMimeTypes 0 | grep -q '(@as \[\],)' \\
    || fail "writing the primary selection changed the clipboard"
call ClipboardMimeTypes 1 | grep -q 'text/plain' || fail "the primary selection is not readable"

echo "PASS: clipboard members (fixture clipboard; Meta.Selection unverified)"
INNER

dbus-run-session -- bash "$WORK/body.sh"
