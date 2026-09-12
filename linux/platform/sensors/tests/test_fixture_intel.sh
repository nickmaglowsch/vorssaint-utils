#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The Intel-laptop fixture: coretemp, an i915 GT with no busy percentage, a
# discharging ACPI battery, wifi, and a kernel without PSI.

source "$(dirname "$0")/harness.sh"
ROOT="$FIXTURES/intel-laptop"

echo "== capabilities =="
run --root "$ROOT" --no-nvml caps
expect_status "caps succeeded" 0
expect "no PSI on this kernel" '^has_psi=0$'
expect "an Intel GT tree is present" '^has_intel_gpu=1$'
expect "no amdgpu" '^has_amdgpu=0$'
expect "power_supply is the backend" '^power_backend=power_supply$'
expect "power_supply capability set" '^has_power_supply=1$'

echo "== memory without PSI =="
run --root "$ROOT" --no-nvml memory
expect "pressure falls back to the MemAvailable shortfall" '^pressure_is_psi=0$'
expect "and says so with a level" '^pressure=0\.444'
expect "no compressor on this kernel" '^has_compressed=0$'
expect "swap in use" '^swap_used_bytes=106803200$'

echo "== network =="
run --root "$ROOT" --no-nvml --count 1 net
expect "wifi classified from phy80211" '^wlan0 kind=wifi up=1 default=1'
expect "a docker bridge is virtual and down" '^docker0 kind=virtual up=0'

# The wifi and ethernet classifications hang on a `device`/`phy80211` entry
# existing. git stores no empty directory, so if either were ever rebuilt as a
# bare mkdir the fixture would survive here and vanish on someone else's clone,
# silently reclassifying the interface as virtual. Assert the shape directly.
check_count=$((check_count + 1))
if [ -L "$ROOT/sys/class/net/wlan0/phy80211" ] && [ -L "$ROOT/sys/class/net/wlan0/device" ]; then
    echo "  ok: the wifi markers are symlinks, so a clean checkout keeps them"
else
    echo "  FAIL: wlan0's device/phy80211 are not symlinks; git will drop them"
    fail_count=$((fail_count + 1))
fi

echo "== gpu: Intel degrades honestly =="
run --root "$ROOT" --no-nvml gpu
expect "one card" '^gpu_count=1$'
expect "driver identified" '^card0 vendor=intel driver=i915'
expect "no busy percentage is claimed" '^card0\.has_busy=0$'
expect "no VRAM is claimed for an integrated GPU" '^card0\.has_vram=0$'
expect "the GT frequency is what sysfs does give" '^card0\.clock_mhz=950\.0$'

echo "== power =="
run --root "$ROOT" --no-nvml power
expect "battery found" '^has_battery=1$'
expect "running on battery" '^external_connected=0$'
expect "state read" '^battery\.state=discharging$'
expect "charge level" '^battery\.percentage=62\.0$'
expect "draw is negative while discharging" '^battery\.watts=-8\.940$'
expect "system draw is the battery draw while unplugged" '^system_watts=8\.940$'
expect "health is full over design" '^battery\.health=0\.889'
expect "cycle count" '^battery\.cycle_count=214$'
expect "time to empty estimated from energy and draw" '^battery\.time_to_empty_seconds=126'
expect "model name used as the label" '^battery\.label=5B10W51$'

echo "== temperatures =="
run --root "$ROOT" --no-nvml temps
expect "five sensors" '^temperature_count=5$'
expect "coretemp package is the CPU primary" \
    'label="Package id 0" driver=coretemp kind=cpu celsius=56\.0 high=100\.0 crit=100\.0 primary=1$'
expect "a core sensor is CPU but not primary" 'label="Core 1" driver=coretemp kind=cpu.*primary=0$'
expect "acpitz is a system sensor" 'driver=acpitz kind=system'
expect "the battery hwmon is a battery sensor" 'driver=BAT0 kind=battery celsius=31\.2'
expect "no fans on this laptop" '^fan_count=0$'

summary
