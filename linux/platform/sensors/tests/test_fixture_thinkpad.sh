#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The ThinkPad fixture, which exists for the awkward cases: label-driven
# platform chips (thinkpad, dell_smm, asus) alongside single-purpose ones
# (zenpower, nouveau), a battery that reports charge in µAh rather than energy
# in µWh, a USB-PD charger with a rating, and a wireless mouse that has its own
# power_supply node.

source "$(dirname "$0")/harness.sh"
ROOT="$FIXTURES/thinkpad"

echo "== temperatures: the classification rules that replace the SMC prefixes =="
run --root "$ROOT" --no-nvml temps
expect_status "temps succeeded" 0
expect "nine sensors" '^temperature_count=9$'
expect "thinkpad CPU label wins over the driver name" \
    '^thinkpad/temp1_input label="CPU" driver=thinkpad kind=cpu'
expect "thinkpad GPU label" 'label="GPU" driver=thinkpad kind=gpu'
expect "thinkpad battery label" 'label="Bat0" driver=thinkpad kind=battery'
expect "thinkpad ambient label" 'label="Ambient" driver=thinkpad kind=system'
expect "dell_smm CPU label" 'label="CPU" driver=dell_smm kind=cpu'
expect "asus CPU label" 'label="CPU Temperature" driver=asus kind=cpu'
expect "zenpower Tdie is a CPU package sensor" 'label="Tdie" driver=zenpower kind=cpu'
expect "nouveau with no label is still a GPU" 'driver=nouveau kind=gpu'
expect "thermal pressure computed" '^thermal_pressure=(nominal|fair|serious|critical)$'

echo "== fans =="
expect "five fans across four chips" '^fan_count=5$'
expect "a fan with a pwm channel is controllable" \
    '^thinkpad/fan1_input label="Fan 1" driver=thinkpad rpm=2914 min=-1 max=-1 controllable=1 pwm=128$'
expect "a fan without one is read-only" \
    '^thinkpad/fan2_input label="Fan 2" driver=thinkpad rpm=0 min=-1 max=-1 controllable=0 pwm=-1$'

echo "== power: charge units, a USB-PD charger, a peripheral =="
run --root "$ROOT" --no-nvml power
expect "battery found" '^has_battery=1$'
expect "charging" '^battery\.state=charging$'
expect "percentage derived from charge_now over charge_full" '^battery\.percentage=74\.1$'
expect "draw is positive while charging" '^battery\.watts=23\.712$'
expect "energy derived from charge x voltage" '^battery\.energy_wh=38\.938$'
expect "health from the charge pair" '^battery\.health=0\.877'
expect "battery temperature in deci-degrees" '^battery\.temperature_celsius=31\.2$'
expect "charger connected" '^external_connected=1$'
expect "adapter draw is voltage x current" '^adapter_watts=45\.000$'
expect "charger rating from the PD maxima" '^adapter_max_watts=65\.000$'
refute "no whole-machine wattage is invented while plugged in" '^system_watts='

echo "== peripheral batteries without UPower =="
expect "one peripheral" '^peripheral_count=1$'
expect "a scope=Device battery is a peripheral, not the machine's" \
    '^peripheral0\.label=MX Master 3 Mouse$'
expect "and its kind is inferred from the model name" '^peripheral0\.kind=mouse$'
expect "with its charge level" '^peripheral0\.percentage=55\.0$'

echo "== cpu frequency without a cpufreq tree =="
run --root "$ROOT" --no-nvml --count 1 cpu
expect "falls back to /proc/cpuinfo" '^core0 usage=0\.0000 mhz=800\.0'
expect "per-cpu, not one value for all" '^core1 usage=0\.0000 mhz=801\.0'

summary
