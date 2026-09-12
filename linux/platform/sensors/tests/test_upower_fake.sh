#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The sd-bus UPower client against a fake daemon on a private bus.
#
# `sd_bus_default_system` honours DBUS_SYSTEM_BUS_ADDRESS, so a `dbus-daemon`
# started for this test is the system bus as far as the client is concerned.
# Real UPower cannot run here (no system bus, no battery, no logind), and this
# is the closest honest substitute: the fake answers the same interface with the
# same signatures, so what is proven is the client, not the daemon.

source "$(dirname "$0")/harness.sh"

PYTHON=${VS_PYTHON:-python3}
command -v dbus-daemon > /dev/null || { echo "no dbus-daemon; skipping"; exit "$SKIP_EXIT"; }
"$PYTHON" -c "import dbus, gi" 2> /dev/null || {
    echo "python dbus/gi bindings missing for $PYTHON; skipping"
    exit "$SKIP_EXIT"
}

WORK=$(mktemp -d)
# The harness owns the single EXIT trap; this is the hook it calls. Killing the
# private bus matters: a leaked dbus-daemon per run adds up on a shared machine.
test_cleanup() {
    [ -n "${FAKE_PID:-}" ] && kill "$FAKE_PID" 2> /dev/null
    [ -n "${BUS_PID:-}" ] && kill "$BUS_PID" 2> /dev/null
    rm -rf "$WORK"
    return 0
}

cat > "$WORK/bus.conf" <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:tmpdir=/tmp</listen>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_type="method_return"/>
    <allow send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="signal"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
  </policy>
</busconfig>
CONF

dbus-daemon --config-file="$WORK/bus.conf" --print-address=1 --print-pid=2 \
    > "$WORK/address" 2> "$WORK/pid" &
for _ in $(seq 1 50); do
    [ -s "$WORK/address" ] && [ -s "$WORK/pid" ] && break
    sleep 0.1
done
DBUS_SYSTEM_BUS_ADDRESS=$(head -1 "$WORK/address")
BUS_PID=$(head -1 "$WORK/pid")
if [ -z "$DBUS_SYSTEM_BUS_ADDRESS" ]; then
    echo "dbus-daemon did not start; skipping"
    exit "$SKIP_EXIT"
fi
export DBUS_SYSTEM_BUS_ADDRESS

echo "== with no UPower on the bus =="
run --power upower caps
expect_status "create still succeeded" 0
expect "UPower capability honestly clear" '^has_upower=0$'

echo "== start the fake daemon =="
"$PYTHON" "$VS_SOURCE_DIR/fake_upower.py" > "$WORK/fake.log" 2>&1 &
FAKE_PID=$!
for _ in $(seq 1 100); do
    grep -q ready "$WORK/fake.log" && break
    sleep 0.1
done
if ! grep -q ready "$WORK/fake.log"; then
    echo "fake UPower did not come up:"
    sed 's/^/    | /' "$WORK/fake.log"
    exit "$SKIP_EXIT"
fi

echo "== capabilities =="
run --power upower caps
expect_status "caps succeeded" 0
expect "UPower detected" '^has_upower=1$'
expect "and chosen as the backend" '^power_backend=upower$'

echo "== the machine's own power =="
run --power upower power
expect_status "power succeeded" 0
expect "battery found" '^has_battery=1$'
expect "the line-power device says the adapter is connected" '^external_connected=1$'
expect "percentage" '^battery\.percentage=41\.0$'
expect "state" '^battery\.state=discharging$'
expect "EnergyRate is signed by State" '^battery\.watts=-12\.500$'
expect "whole-machine draw while on battery" '^system_watts=12\.500$'
expect "TimeToEmpty" '^battery\.time_to_empty_seconds=5400$'
expect "Temperature" '^battery\.temperature_celsius=32\.5$'
expect "Capacity becomes a 0...1 health fraction" '^battery\.health=0\.9100$'
expect "ChargeCycles" '^battery\.cycle_count=134$'
expect "Model is the label" '^battery\.label=BAT-X1$'
expect "Vendor" '^battery\.vendor=ACME$'
expect "Energy" '^battery\.energy_wh=22\.500$'
expect "EnergyFullDesign" '^battery\.energy_full_design_wh=57\.000$'

echo "== peripheral batteries =="
expect "two peripherals, and not the machine's own battery" '^peripheral_count=2$'
expect "a mouse is typed from UPower's Type" 'kind=mouse'
expect "a keyboard too" 'kind=keyboard'
expect "with its level" '^peripheral[01]\.percentage=80\.0$'
refute "the internal battery is not listed as a peripheral" '^peripheral[01]\.label=BAT-X1$'

echo "== the daemon leaving the bus shrinks capabilities =="
# The backend is built while UPower is up, so it starts with the capability;
# the daemon is killed mid-run and `dispatch` has to notice on the next tick.
( sleep 1; kill "$FAKE_PID" 2> /dev/null ) &
KILLER_PID=$!
run --power upower watch --count 3 --interval 1
expect_status "watch survived the daemon going away" 0
expect "the loss is announced through dispatch" '^event=backend_lost$'
wait "$KILLER_PID" 2> /dev/null
FAKE_PID=

summary
