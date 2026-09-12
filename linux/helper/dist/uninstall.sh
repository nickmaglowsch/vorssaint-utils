#!/bin/sh
# Remove vorssaint-helper completely. Run through pkexec from the app's
# Capabilities page, the same way install.sh is.
#
#   pkexec /usr/lib/vorssaint/uninstall.sh
#
# The capability goes away with the files: no group membership, no lingering
# udev permission, no state directory. That is the property the `input` group
# alternative cannot offer, and it is only true if this script really removes
# everything, so it verifies afterwards that each path is gone.
#
# Idempotent: running it on a machine with no helper prints "absent" for every
# file and exits 0.

set -eu

DESTDIR=${DESTDIR:-}
PREFIX=${PREFIX:-/usr}

FILES="
$DESTDIR$PREFIX/libexec/vorssaint-helper
$DESTDIR$PREFIX/share/dbus-1/system.d/org.vorssaint.Helper1.conf
$DESTDIR$PREFIX/share/dbus-1/system-services/org.vorssaint.Helper1.service
$DESTDIR$PREFIX/share/polkit-1/actions/org.vorssaint.helper.policy
$DESTDIR$PREFIX/lib/systemd/system/vorssaint-helper.service
$DESTDIR$PREFIX/lib/udev/rules.d/70-vorssaint-uinput.rules
$DESTDIR$PREFIX/lib/udev/rules.d/71-vorssaint-i2c.rules
"

echo "vorssaint-helper: removing from ${DESTDIR:-/} (prefix $PREFIX)"
echo

# Stop it first. A running helper may hold an EVIOCGRAB, and deleting the
# binary out from under it would leave the user's keyboard grabbed by a
# process with no file on disk to explain it.
if [ -z "$DESTDIR" ] && command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if systemctl is-active --quiet vorssaint-helper.service; then
        systemctl stop vorssaint-helper.service && echo "  stopped    vorssaint-helper.service"
    fi
    systemctl disable vorssaint-helper.service >/dev/null 2>&1 &&
        echo "  disabled   vorssaint-helper.service" || true
fi

n_removed=0
n_absent=0
for f in $FILES; do
    if [ -e "$f" ]; then
        rm -f "$f"
        [ -e "$f" ] && { echo "  FAILED     $f" >&2; exit 1; }
        echo "  removed    $f"
        n_removed=$((n_removed + 1))
    else
        echo "  absent     $f"
        n_absent=$((n_absent + 1))
    fi
done

echo
echo "  $n_removed removed, $n_absent already absent"

if [ -n "$DESTDIR" ]; then
    echo
    echo "DESTDIR is set: files removed from the staging root only."
    exit 0
fi

if [ "$n_removed" -gt 0 ]; then
    echo
    echo "Reloading:"
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        systemctl daemon-reload && echo "  systemd units reloaded"
        systemctl reload dbus.service 2>/dev/null && echo "  dbus policy reloaded" || true
    fi
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules && echo "  udev rules reloaded"
    fi
fi

echo
echo "Verifying nothing is left:"
rc=0
for f in $FILES; do
    if [ -e "$f" ]; then
        echo "  STILL HERE $f"
        rc=1
    fi
done
if [ -z "$DESTDIR" ] && command -v busctl >/dev/null 2>&1 &&
   [ -S /run/dbus/system_bus_socket ]; then
    if busctl --system status org.vorssaint.Helper1 >/dev/null 2>&1; then
        echo "  the helper is still on the system bus"
        rc=1
    else
        echo "  gone from the system bus"
    fi
fi
[ "$rc" -eq 0 ] && echo "  nothing left on disk"

echo
if [ "$rc" -eq 0 ]; then
    echo "vorssaint-helper: removed."
else
    echo "vorssaint-helper: removal incomplete; see the lines above." >&2
fi
exit "$rc"
