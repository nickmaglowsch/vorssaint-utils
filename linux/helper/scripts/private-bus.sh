#!/bin/bash
# Exercise the whole privileged surface without systemd or polkit running.
#
# systemd and polkit are not available in a plain container, so this harness
# replaces the parts that can be replaced and is explicit about the parts that
# cannot:
#
#   replaced: the system bus       -> a private dbus-daemon declared
#                                     <type>system</type>, loading the real
#                                     dist/org.vorssaint.Helper1.conf from an
#                                     includedir, under the stock system.conf
#                                     default policy
#   replaced: the polkit authority -> the polkit_check() stub, compiled in only
#                                     with -DWITH_POLKIT=OFF
#   replaced: logind sessions      -> --fake-sessions, two unprivileged users
#                                     standing in for two seats
#   replaced: /sys/class/hwmon     -> a temporary tree of plain files
#   real:     the bus policy file, the name claim, the D-Bus interface, the
#             client library, the helper and everything behind it
#
# What it proves: the shipped busconfig admits the helper's name claim and the
# unprivileged caller's method calls; an unprivileged process cannot claim the
# name; the client library drives the helper end to end as a user with no
# device access at all; the session binding refuses a second seat; the fan
# watchdog restores automatic control on its own; and the authorisation gate
# gates every method, including the four new ones.
#
# usage: scripts/private-bus.sh <build-dir-built-with-WITH_POLKIT=OFF>
#        POLKIT_BUILD=<build-dir-with-polkit> to also run § 12
set -u

BUILD=${1:?usage: private-bus.sh <build-dir>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=${RUN_DIR:-/tmp/wps1/privbus}
UNPRIV=${UNPRIV_USER:-vorssaint-test}
UNPRIV2=${UNPRIV_USER2:-vorssaint-test2}

rm -rf "$RUN"
mkdir -p "$RUN/system.d" "$RUN/hwmon/hwmon0"
cp "$HERE/dist/org.vorssaint.Helper1.conf" "$RUN/system.d/"

# A fake hwmon tree: one controllable fan, currently on the chip's own curve.
printf 'nct6798\n' > "$RUN/hwmon/hwmon0/name"
printf '120\n'     > "$RUN/hwmon/hwmon0/pwm1"
printf '2\n'       > "$RUN/hwmon/hwmon0/pwm1_enable"

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
chmod -R 0777 "$RUN/hwmon"

id "$UNPRIV"  >/dev/null 2>&1 || useradd -M -s /usr/sbin/nologin "$UNPRIV"
id "$UNPRIV2" >/dev/null 2>&1 || useradd -M -s /usr/sbin/nologin "$UNPRIV2"
UID1=$(id -u "$UNPRIV")
UID2=$(id -u "$UNPRIV2")
echo "two unprivileged users stand in for two seats: $UNPRIV=$UID1 -> session c1,"
echo "                                               $UNPRIV2=$UID2 -> session c2"

as() {
  u=$1; shift
  setpriv --reuid "$u" --regid nogroup --clear-groups \
          env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" "$@"
}

say "3. an unprivileged process tries to own org.vorssaint.Helper1"
as "$UNPRIV" "$BUILD/vorssaint-helper" --backend fake 2>&1 | sed 's/^/  impostor: /'
echo "  (only <policy user=\"root\"> carries allow own= for this name)"

say "4. the real helper (root) claims the name"
"$BUILD/vorssaint-helper" --backend fake --tap \
    --hwmon-root "$RUN/hwmon" \
    --fake-sessions "uid:$UID1=c1,uid:$UID2=c2" > "$RUN/helper.out" 2>&1 &
HELPER=$!
sleep 1
sed 's/^/  /' "$RUN/helper.out"
echo "-- the bus agrees the name is owned, and by which uid --"
dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus.GetNameOwner string:org.vorssaint.Helper1 2>&1 | sed 's/^/  /'
dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus.GetConnectionUnixUser string:org.vorssaint.Helper1 2>&1 | sed 's/^/  /'

say "5. what the unprivileged uid can reach without the helper"
as "$UNPRIV" id 2>&1 | sed 's/^/  /'
as "$UNPRIV" sh -c 'ls -l /dev/uinput /dev/input /dev/i2c-* 2>&1' | sed 's/^/  /'
as "$UNPRIV" sh -c 'ls -l /sys/class/hwmon 2>&1' | sed 's/^/  /'

# Every call below goes through the client library, not dbus-send: what is
# under test is the code the app will link.
c()  { echo "-- $* --"; as "$UNPRIV"  "$BUILD/vorssaint-helperctl" "$@" 2>&1 | sed 's/^/  /'; }
c2() { echo "-- (seat 2) $* --"; as "$UNPRIV2" "$BUILD/vorssaint-helperctl" "$@" 2>&1 | sed 's/^/  /'; }

say "6. the unprivileged client drives the relay through the client library"
c available
c get Backend
c get Authorization
c get-devices
c get Owner
c enable
c get Owner
c get-devices
c set-rules '{"tap_hold":true,"tap_threshold_ms":150,"chatter":true,"chatter_ms":25}'
c get Rules
echo "-- set-rules with a malformed document (must be rejected) --"
as "$UNPRIV" "$BUILD/vorssaint-helperctl" set-rules '{"tap_threshold_ms":"whenever"}' 2>&1 |
  sed 's/^/  /'
c listen 2

say "7. the session binding: only the seat that enabled it may change it"
# This is the PLAN.md § 4.4 gate decision. Seat 1 holds the relay; seat 2 is
# authorised for the same actions by the same stub, so if these succeeded the
# binding would be decorative.
c2 get Owner
c2 disable
c2 set-rules '{"tap_hold":false}'
echo "-- but seat 2 may still read, which needs no ownership --"
c2 get-devices
c2 get-capabilities
echo "-- and the owning seat may --"
c disable
echo "-- after release the binding is gone and seat 2 may take it --"
c2 enable
c2 get Owner
echo "-- now seat 1 is the one refused --"
c disable
c2 disable

say "8. GetCapabilities: what this machine can actually do"
c get-capabilities

say "9. fan control and the watchdog"
echo "-- the fake hwmon tree before --"
for f in "$RUN"/hwmon/hwmon0/*; do echo "  $f = $(cat "$f")"; done
c fan-pwm hwmon0 1 64
echo "-- after SetFanPwm: manual mode (1) and the duty cycle, both read back --"
for f in "$RUN"/hwmon/hwmon0/*; do echo "  $f = $(cat "$f")"; done
c get Fan
echo "-- a heartbeat defers the restore --"
c fan-heartbeat
echo "-- refusals: a bad hwmon name and a channel that does not exist --"
c fan-pwm ../../etc 1 64
c fan-pwm hwmon9 1 64
echo "-- now stop heart-beating and wait out the 10 s watchdog --"
sleep 12
echo "-- the helper restored automatic control with no client involvement --"
for f in "$RUN"/hwmon/hwmon0/*; do echo "  $f = $(cat "$f")"; done
c get Fan
grep "FAN WATCHDOG" "$RUN/helper.out" | sed 's/^/  /'

say "10. DDC/CI"
echo "There is no /dev/i2c-* on this machine, so the only thing this can show"
echo "is that the method is reachable and refuses what it must. The framing is"
echo "proven against a fake monitor in tests/test_ddc.c and has never spoken to"
echo "a display."
c ddc-write ../../dev/mem 10 50
c ddc-read i2c-0 10

say "11. the interface as the bus sees it"
dbus-send --system --print-reply --dest=org.vorssaint.Helper1 /org/vorssaint/Helper1 \
  org.freedesktop.DBus.Introspectable.Introspect 2>&1 |
  grep -E "interface name=.org.vorssaint|method name|signal name|property name" |
  sed 's/^/  /'

say "12. helper log"
sed 's/^/  /' "$RUN/helper.out"

kill "$HELPER" 2>/dev/null
wait "$HELPER" 2>/dev/null

say "13. the authorization gate actually gates, on every method"
# Same binary, same bus, same client: only the stub's verdict changes. If the
# calls still succeeded here, the gate would be decorative.
"$BUILD/vorssaint-helper" --backend fake --deny-all --hwmon-root "$RUN/hwmon" \
    > "$RUN/deny.out" 2>&1 &
DENY=$!
sleep 1
c enable
c set-rules '{"tap_hold":false}'
c get-devices
c get-capabilities
c fan-pwm hwmon0 1 200
c fan-heartbeat
c ddc-write i2c-0 10 50
c ddc-read i2c-0 10
echo "-- helper log --"
sed 's/^/  /' "$RUN/deny.out"
kill "$DENY" 2>/dev/null
wait "$DENY" 2>/dev/null

if [ -n "${POLKIT_BUILD:-}" ]; then
  say "14. the same helper built WITH polkit, on the same bus"
  # No polkit authority runs in this container, so the call cannot succeed.
  # What it shows is that the real polkit_authority_* calls are on the path:
  # the failure comes from inside libpolkit-gobject-1, not from a stub.
  echo "-- polkit symbols the binary imports --"
  nm -D "$POLKIT_BUILD/vorssaint-helper" | grep polkit | sed 's/^/  /'
  echo "-- and --fake-sessions is refused in that build --"
  "$POLKIT_BUILD/vorssaint-helper" --backend fake --fake-sessions "uid:$UID1=c1" 2>&1 |
    sed 's/^/  /'
  "$POLKIT_BUILD/vorssaint-helper" --backend fake --hwmon-root "$RUN/hwmon" \
      > "$RUN/polkit.out" 2>&1 &
  PK=$!
  sleep 1
  as "$UNPRIV" "$BUILD/vorssaint-helperctl" get Authorization 2>&1 | sed 's/^/  /'
  echo "-- every gated method with the real authority check --"
  for cmd in "enable" "get-devices" "get-capabilities" "fan-pwm hwmon0 1 64" "ddc-read i2c-0 10"; do
    echo "  -- $cmd --"
    # shellcheck disable=SC2086
    as "$UNPRIV" "$BUILD/vorssaint-helperctl" $cmd 2>&1 | sed 's/^/    /'
  done
  echo "-- helper log --"
  sed 's/^/  /' "$RUN/polkit.out"
  kill "$PK" 2>/dev/null
  wait "$PK" 2>/dev/null
fi

say "15. shut down"
kill "$(cat "$RUN/bus.pid")" 2>/dev/null
echo "done"
