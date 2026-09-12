#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The `vs-sensors` harness against the Ryzen-desktop fixture tree: an amdgpu
# card, a k10temp package sensor, an nvme drive, no battery. This machine has
# none of those, which is the point of the tree.

source "$(dirname "$0")/harness.sh"
ROOT="$FIXTURES/amd-desktop"

echo "== capabilities =="
run --root "$ROOT" --no-nvml caps
expect_status "caps succeeded" 0
expect "hwmon found" '^has_hwmon=1$'
expect "PSI found" '^has_psi=1$'
expect "amdgpu found" '^has_amdgpu=1$'
expect "no Intel GT" '^has_intel_gpu=0$'
expect "no NVML" '^has_nvml=0$'
expect "no UPower under a fixture root" '^has_upower=0$'
expect "no power supply on a desktop" '^has_power_supply=0$'
expect "power backend names itself" '^power_backend=none$'

echo "== cpu =="
run --root "$ROOT" --no-nvml --count 1 cpu
expect_status "cpu succeeded" 0
expect "eight logical cores" '^core_count=8$'
expect "four physical cores" '^physical_core_count=4$'
expect "one package" '^package_count=1$'
expect "load average parsed" '^load1=1\.42$'
expect "frequency from cpufreq" '^core0 usage=0\.0000 mhz=3800\.0 package=0 core_id=0$'
expect "first sample has no rates" '^has_rates=0$'

echo "== memory =="
run --root "$ROOT" --no-nvml memory
expect "total is MemTotal" '^total_bytes=33554432000$'
expect "used is MemTotal - MemAvailable" '^used_bytes=8978432000$'
expect "app memory is AnonPages" '^app_bytes=7270400000$'
expect "pressure comes from PSI" '^pressure_is_psi=1$'
expect "PSI some avg10 scaled to 0...1" '^pressure=0\.0042$'

echo "== network =="
run --root "$ROOT" --no-nvml --count 1 net
expect "three interfaces" '^interface_count=3$'
expect "ethernet is classified and carries the default route" \
    '^enp5s0 kind=ethernet up=1 default=1 rx=91827364 tx=12345678'
expect "loopback is classified" '^lo kind=loopback'
expect "a bridge with no device link is virtual" '^virbr0 kind=virtual'

echo "== disk =="
run --root "$ROOT" --no-nvml --count 1 disk
expect "five block devices" '^device_count=5$'
expect "whole disk bytes are sectors x 512" '^nvme0n1 partition=0 read=31362121728 write=20762230784'
expect "a partition is marked" '^nvme0n1p2 partition=1'
expect "pseudo filesystems are skipped" '^mount_count=3$'
expect "a read-only mount is marked" '^/mnt/data source=/dev/sda fs=xfs device=sda ro=1'
expect "SMART is out of scope and says so" '^smart=out-of-scope$'
refute "no proc mount row" '^/proc source=proc'
refute "no tmpfs mount row" 'fs=tmpfs'

echo "== temperatures and fans =="
run --root "$ROOT" --no-nvml temps
expect "seven temperature sensors" '^temperature_count=7$'
expect "k10temp Tctl is the CPU primary" \
    '^k10temp/temp1_input label="Tctl" driver=k10temp kind=cpu celsius=48\.4 high=95\.0 crit=100\.0 primary=1$'
expect "k10temp Tccd1 is CPU but not primary" 'label="Tccd1".*kind=cpu.*primary=0$'
expect "nvme Composite is a drive primary" 'label="Composite" driver=nvme kind=drive.*primary=1$'
expect "amdgpu edge is the GPU primary" 'label="edge" driver=amdgpu kind=gpu.*primary=1$'
expect "amdgpu junction is GPU" 'label="junction" driver=amdgpu kind=gpu'
expect "thermal pressure derived from trip points" '^thermal_pressure=nominal$'
expect "one fan" '^fan_count=1$'
expect "the fan is readable and has a pwm channel" \
    '^amdgpu/fan1_input label="amdgpu fan1" driver=amdgpu rpm=1180 min=0 max=3200 controllable=1 pwm=94$'

echo "== gpu =="
run --root "$ROOT" --no-nvml gpu
expect "one card" '^gpu_count=1$'
expect "vendor and driver identified" '^card0 vendor=amd driver=amdgpu name=amdgpu card0$'
expect "busy percentage is available on amdgpu" '^card0\.busy=0\.3700$'
expect "VRAM is available on amdgpu" '^card0\.vram_total_bytes=17163091968$'
expect "power from the card hwmon" '^card0\.watts=43\.00$'
expect "temperature from the card hwmon" '^card0\.temperature_celsius=52\.0$'
expect "shader clock from the card hwmon" '^card0\.clock_mhz=2405\.0$'

echo "== power =="
run --root "$ROOT" --no-nvml power
expect "a desktop has no power source to read" '^power=unsupported by this backend$'
expect "and no peripheral batteries" '^peripheral_count=0$'

summary
