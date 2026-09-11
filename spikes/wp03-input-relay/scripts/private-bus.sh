#!/bin/bash
# Exercise the privilege split without systemd or polkit running.
#
# systemd and polkit are not available in a plain container, so this harness
# replaces the parts that can be replaced and is explicit about the parts that
# cannot:
#
#   replaced: the system bus       -> a private dbus-daemon declared
#                                     <type>system</type>, loading the real
#                                     data/org.vorssaint.Helper1.conf from an
#                                     includedir, under the stock system.conf
#                                     default policy
#   replaced: the polkit authority -> the polkit_check() stub, compiled in only
#                                     with -DWITH_POLKIT=OFF
#   real:     the bus policy file, the name claim, the D-Bus interface, the
#             client, the helper, and the rules engine behind them
#
# What it proves: the shipped busconfig admits the helper's name claim and the
# unprivileged caller's method calls; an unprivileged process cannot claim the
# name; and the client drives the helper end to end as a user with no device
# access at all.
#
# usage: scripts/private-bus.sh <build-dir-built-with-WITH_POLKIT=OFF>
set -u

BUILD=${1:?usage: private-bus.sh <build-dir>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=${RUN_DIR:-/tmp/wp03/privbus}
UNPRIV=${UNPRIV_USER:-vorssaint-test}

rm -rf "$RUN"
mkdir -p "$RUN/system.d"
cp "$HERE/data/org.vorssaint.Helper1.conf" "$RUN/system.d/"

# The <policy context="default"> block below is copied verbatim from the
# distribution's /usr/share/dbus-1/system.conf, so the harness enforces the same
# default-deny the real system bus does: no name ownership and no method calls
# except where a file in system.d punches a hole. That is the property under
# test; only <listen> and <includedir> differ from the real thing.
cat > "$RUN/bus.conf" <<BUSCONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$RUN/socket</listen>
  <auth>EXTERNAL</auth>

  <policy context="default">
    <allow user="*"/>

    <!-- Holes must be punched in service configuration files for
         name ownership and sending method calls -->
    <deny own="*"/>
    <deny send_type="method_call"/>

    <allow send_type="signal"/>
    <allow send_requested_reply="true" send_type="method_return"/>
    <allow send_requested_reply="true" send_type="error"/>

    <allow receive_type="method_call"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
    <allow receive_type="signal"/>

    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus" />
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Introspectable"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Properties"/>
  </policy>

  <includedir>$RUN/system.d</includedir>
</busconfig>
BUSCONF

say() { printf '\n===== %s =====\n' "$*"; }

say "1. bus policy in force"
echo "--- $RUN/bus.conf (stock system.conf default policy + includedir) ---"
cat "$RUN/bus.conf"
echo "--- $RUN/system.d/org.vorssaint.Helper1.conf (the file the port ships) ---"
cat "$RUN/system.d/org.vorssaint.Helper1.conf"

say "2. start the private system bus"
# --print-pid takes a file descriptor, not a path, hence the redirect. The
# address is not printed back: it is the <listen> line above, verbatim.
dbus-daemon --config-file="$RUN/bus.conf" --fork --print-pid=3 3> "$RUN/bus.pid" || exit 1
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$RUN/socket"
echo "DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SYSTEM_BUS_ADDRESS"
echo "dbus-daemon pid=$(cat "$RUN/bus.pid") started by uid=$(id -u)"
chmod 0755 "$RUN"
chmod 0777 "$RUN/socket" 2>/dev/null

id "$UNPRIV" >/dev/null 2>&1 || useradd -M -s /usr/sbin/nologin "$UNPRIV"
as_unpriv() {
  setpriv --reuid "$UNPRIV" --regid nogroup --clear-groups \
          env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" "$@"
}

say "3. an unprivileged process tries to own org.vorssaint.Helper1"
as_unpriv "$BUILD/vorssaint-helper" --backend fake 2>&1 | sed 's/^/  impostor: /'
echo "  (only <policy user=\"root\"> carries allow own= for this name)"

say "4. the real helper (root) claims the name"
"$BUILD/vorssaint-helper" --backend fake --tap > "$RUN/helper.out" 2>&1 &
HELPER=$!
sleep 1
sed 's/^/  /' "$RUN/helper.out"
echo "-- the bus agrees the name is owned, and by which uid --"
dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus.GetNameOwner string:org.vorssaint.Helper1 2>&1 | sed 's/^/  /'
dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus.GetConnectionUnixUser string:org.vorssaint.Helper1 2>&1 | sed 's/^/  /'

say "5. what the unprivileged uid can reach without the helper"
as_unpriv id 2>&1 | sed 's/^/  /'
as_unpriv sh -c 'ls -l /dev/uinput /dev/input 2>&1' | sed 's/^/  /'

say "6. the unprivileged client calls the helper"
c() { echo "-- $* --"; as_unpriv "$BUILD/vorssaint-relayctl" "$@" 2>&1 | sed 's/^/  /'; }
c get Backend
c get Authorization
c get-devices
c enable
c get-devices
c set-rules '{"tap_hold":true,"tap_threshold_ms":150,"chatter":true,"chatter_ms":25}'
c get Rules
echo "-- set-rules with a malformed document (must be rejected) --"
as_unpriv "$BUILD/vorssaint-relayctl" set-rules '{"tap_threshold_ms":"whenever"}' 2>&1 |
  sed 's/^/  /'
c listen 2
c disable
echo "-- after Enable(false): devices released, GetDevices re-enumerates without grabbing --"
as_unpriv "$BUILD/vorssaint-relayctl" get-devices 2>&1 | sed 's/^/  /'
echo "-- and Enable(true) again must open a clean backend, not a second grab --"
c enable
c get-devices
c disable

say "7. the interface as the bus sees it"
dbus-send --system --print-reply --dest=org.vorssaint.Helper1 /org/vorssaint/Helper1 \
  org.freedesktop.DBus.Introspectable.Introspect 2>&1 |
  grep -E "interface name=.org.vorssaint|method name|signal name|arg name|property name" |
  sed 's/^/  /'

say "8. helper log"
sed 's/^/  /' "$RUN/helper.out"

kill "$HELPER" 2>/dev/null
wait "$HELPER" 2>/dev/null

say "9. the authorization gate actually gates"
# Same binary, same bus, same client: only the stub's verdict changes. If the
# calls still succeeded here, the gate would be decorative.
"$BUILD/vorssaint-helper" --backend fake --deny-all > "$RUN/deny.out" 2>&1 &
DENY=$!
sleep 1
c enable
c set-rules '{"tap_hold":false}'
c get-devices
echo "-- helper log --"
sed 's/^/  /' "$RUN/deny.out"
kill "$DENY" 2>/dev/null
wait "$DENY" 2>/dev/null

if [ -n "${POLKIT_BUILD:-}" ]; then
  say "10. the same helper built WITH polkit, on the same bus"
  # No polkit authority runs in this container, so the call cannot succeed. What
  # it shows is that the real polkit_authority_* calls are on the path: the
  # failure comes from inside libpolkit-gobject-1, not from a stub.
  echo "-- polkit symbols the binary imports --"
  nm -D "$POLKIT_BUILD/vorssaint-helper" | grep polkit | sed 's/^/  /'
  "$POLKIT_BUILD/vorssaint-helper" --backend fake > "$RUN/polkit.out" 2>&1 &
  PK=$!
  sleep 1
  as_unpriv "$BUILD/vorssaint-relayctl" get Authorization 2>&1 | sed 's/^/  /'
  echo "-- Enable() with the real authority check --"
  as_unpriv "$BUILD/vorssaint-relayctl" enable 2>&1 | sed 's/^/  /'
  echo "-- helper log --"
  sed 's/^/  /' "$RUN/polkit.out"
  kill "$PK" 2>/dev/null
  wait "$PK" 2>/dev/null
fi

say "11. shut down"
kill "$(cat "$RUN/bus.pid")" 2>/dev/null
echo "done"
