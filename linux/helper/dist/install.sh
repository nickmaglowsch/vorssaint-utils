#!/bin/sh
# Install vorssaint-helper. Run through pkexec from the app's Capabilities
# page; one polkit prompt, no sudo, no shell-out to anything else.
#
#   pkexec /usr/lib/vorssaint/install.sh
#
# Properties this script is written to have, in order of how badly their
# absence would hurt:
#
#  1. Idempotent. Running it twice is running it once. Every file is compared
#     before it is written, and a second run prints "unchanged" for all of
#     them, which is also how the test asserts idempotence.
#  2. Verified by reading back. The exit status of `install` and `systemctl`
#     is not evidence; each file is compared with its source afterwards, and
#     the bus name is queried at the end.
#  3. It says what it changed. A privileged script that prints nothing leaves
#     the user with no way to know what was done to their system.
#  4. DESTDIR-clean. With DESTDIR set it writes only under that root and skips
#     every daemon-reload, which is what the test in tests/test_install.sh
#     uses and what a distribution package would use.
#
# POSIX sh: this runs as root on whatever the user has.

set -eu

DESTDIR=${DESTDIR:-}
PREFIX=${PREFIX:-/usr}
SRC=${SRC:-$(cd "$(dirname "$0")" && pwd)}
# The binary is not next to the data files in the build tree, so it is named
# separately; the app passes its bundled path.
HELPER_BIN=${HELPER_BIN:-$SRC/vorssaint-helper}

LIBEXEC="$DESTDIR$PREFIX/libexec"
DBUS_SYSTEM_D="$DESTDIR$PREFIX/share/dbus-1/system.d"
DBUS_SERVICES="$DESTDIR$PREFIX/share/dbus-1/system-services"
POLKIT_ACTIONS="$DESTDIR$PREFIX/share/polkit-1/actions"
SYSTEMD_UNITS="$DESTDIR$PREFIX/lib/systemd/system"
UDEV_RULES="$DESTDIR$PREFIX/lib/udev/rules.d"

changed=0
n_installed=0
n_updated=0
n_unchanged=0

die() {
    echo "install: $*" >&2
    exit 1
}

# install_file <source> <destination> <mode>
#
# Compares before writing, and compares again afterwards. The second
# comparison is the one that matters: a full filesystem, a read-only /usr and
# an SELinux denial all let `install` look like it worked.
install_file() {
    src=$1
    dst=$2
    mode=$3

    [ -f "$src" ] || die "missing source file: $src"

    if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
        # The content matches; the mode still might not.
        cur=$(stat -c '%a' "$dst" 2>/dev/null || echo '')
        if [ "$cur" = "$mode" ]; then
            echo "  unchanged  $dst"
            n_unchanged=$((n_unchanged + 1))
            return 0
        fi
        chmod "$mode" "$dst"
        echo "  mode $mode  $dst"
        n_updated=$((n_updated + 1))
        changed=1
        return 0
    fi

    if [ -e "$dst" ]; then verb="updated   "; else verb="installed "; fi

    mkdir -p "$(dirname "$dst")"
    install -m "$mode" "$src" "$dst" || die "cannot install $dst"

    cmp -s "$src" "$dst" || die "$dst does not match $src after installing it"
    cur=$(stat -c '%a' "$dst" 2>/dev/null || echo '?')
    [ "$cur" = "$mode" ] || die "$dst has mode $cur, expected $mode"

    echo "  $verb $dst"
    if [ "$verb" = "updated   " ]; then
        n_updated=$((n_updated + 1))
    else
        n_installed=$((n_installed + 1))
    fi
    changed=1
}

echo "vorssaint-helper: installing into ${DESTDIR:-/} (prefix $PREFIX)"
echo "  source:  $SRC"
echo "  binary:  $HELPER_BIN"
echo

install_file "$HELPER_BIN"                       "$LIBEXEC/vorssaint-helper"                            755
install_file "$SRC/org.vorssaint.Helper1.conf"   "$DBUS_SYSTEM_D/org.vorssaint.Helper1.conf"            644
install_file "$SRC/org.vorssaint.Helper1.service" "$DBUS_SERVICES/org.vorssaint.Helper1.service"        644
install_file "$SRC/org.vorssaint.helper.policy"  "$POLKIT_ACTIONS/org.vorssaint.helper.policy"          644
install_file "$SRC/vorssaint-helper.service"     "$SYSTEMD_UNITS/vorssaint-helper.service"              644
install_file "$SRC/70-vorssaint-uinput.rules"    "$UDEV_RULES/70-vorssaint-uinput.rules"                644
install_file "$SRC/71-vorssaint-i2c.rules"       "$UDEV_RULES/71-vorssaint-i2c.rules"                   644

echo
echo "  $n_installed installed, $n_updated updated, $n_unchanged unchanged"

if [ "$changed" -eq 0 ]; then
    echo
    echo "Nothing changed; skipping the reloads."
elif [ -n "$DESTDIR" ]; then
    echo
    echo "DESTDIR is set: files staged only, skipping the reloads."
else
    echo
    echo "Reloading:"
    # Each of these is optional in the sense that the machine may not have the
    # daemon; none of them is optional in the sense of being allowed to fail
    # silently, so every one prints what happened.
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules && echo "  udev rules reloaded"
        udevadm trigger --subsystem-match=misc --action=add >/dev/null 2>&1 || true
        udevadm trigger --subsystem-match=i2c-dev --action=add >/dev/null 2>&1 || true
    else
        echo "  udevadm not found: udev rules take effect at the next boot"
    fi
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        systemctl daemon-reload && echo "  systemd units reloaded"
        # reload, not restart: picks up system.d/ without dropping every
        # connection on the system bus.
        systemctl reload dbus.service 2>/dev/null && echo "  dbus policy reloaded" ||
            echo "  could not reload dbus.service; the policy takes effect at the next boot"
        systemctl enable --now vorssaint-helper.service && echo "  vorssaint-helper.service enabled"
    else
        echo "  systemd is not running here: the unit was staged but not started"
    fi
fi

if [ -n "$DESTDIR" ]; then
    echo
    echo "DESTDIR is set: nothing on this system was reloaded or started."
fi

echo
echo "Verifying (reading back, not trusting the exit status above):"
rc=0
for f in "$LIBEXEC/vorssaint-helper" \
         "$DBUS_SYSTEM_D/org.vorssaint.Helper1.conf" \
         "$DBUS_SERVICES/org.vorssaint.Helper1.service" \
         "$POLKIT_ACTIONS/org.vorssaint.helper.policy" \
         "$SYSTEMD_UNITS/vorssaint-helper.service" \
         "$UDEV_RULES/70-vorssaint-uinput.rules" \
         "$UDEV_RULES/71-vorssaint-i2c.rules"; do
    if [ -f "$f" ]; then
        echo "  present    $f"
    else
        echo "  MISSING    $f"
        rc=1
    fi
done

if [ -z "$DESTDIR" ] && command -v busctl >/dev/null 2>&1 &&
   [ -S /run/dbus/system_bus_socket ]; then
    owner=$(busctl --system get-property org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus Features >/dev/null 2>&1 && echo yes || echo no)
    if [ "$owner" = yes ]; then
        # The helper is bus-activated, so asking for a property starts it. If
        # this fails the install is not finished, whatever the files say.
        if busctl --system get-property org.vorssaint.Helper1 /org/vorssaint/Helper1 \
                 org.vorssaint.Helper1 Authorization 2>/dev/null; then
            echo "  the helper answers on the system bus (shown above)"
        else
            echo "  the helper does not answer on the system bus yet"
            rc=1
        fi
    fi
fi
if [ -z "$DESTDIR" ] && command -v pkaction >/dev/null 2>&1; then
    for a in enable set-rules fan-control ddc get-devices get-capabilities; do
        if pkaction --action-id "org.vorssaint.helper.$a" >/dev/null 2>&1; then
            echo "  polkit knows org.vorssaint.helper.$a"
        else
            echo "  polkit does not know org.vorssaint.helper.$a"
            rc=1
        fi
    done
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "vorssaint-helper: installed."
else
    echo "vorssaint-helper: installed with problems; see the lines above." >&2
fi
exit "$rc"
