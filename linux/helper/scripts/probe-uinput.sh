#!/bin/bash
# Establish, from the kernel rather than from guesswork, whether the real
# evdev/uinput path can run here, and show the relay's behaviour either way.
#
# Run it on any machine: on a normal desktop it prints the discovered devices
# and starts relaying; on a kernel without uinput it prints the exact reason.
#
# usage: scripts/probe-uinput.sh <build-dir>
set -u
BUILD=${1:?usage: probe-uinput.sh <build-dir>}

echo "### kernel"
uname -r

echo
echo "### kernel config for the two facilities the relay needs"
if [ -r /proc/config.gz ]; then
  zcat /proc/config.gz |
    grep -E "CONFIG_INPUT_EVDEV|CONFIG_INPUT_UINPUT|CONFIG_MODULES[ =]|CONFIG_MODULES is"
elif [ -r "/boot/config-$(uname -r)" ]; then
  grep -E "CONFIG_INPUT_EVDEV|CONFIG_INPUT_UINPUT|CONFIG_MODULES[ =]|CONFIG_MODULES is" \
    "/boot/config-$(uname -r)"
else
  echo "  (no kernel config exposed)"
fi

echo
echo "### can a module be loaded at all"
command -v modprobe || echo "  modprobe: not installed"
modprobe uinput 2>&1 || true
ls /lib/modules 2>&1

echo
echo "### /proc/misc: a live uinput registers minor 223 here"
grep -E "223|uinput" /proc/misc || echo "  (no uinput minor registered)"

echo
echo "### /sys/class/misc/uinput"
ls -la /sys/class/misc/uinput 2>&1

echo
echo "### create the device nodes by hand and try to open them"
mknod /dev/uinput c 10 223 2>&1 && ls -l /dev/uinput
mkdir -p /dev/input && mknod /dev/input/event0 c 13 64 2>&1 && ls -l /dev/input/event0

echo
echo "### the relay on the real evdev backend (no fallback, no mock)"
"$BUILD/vorssaint-relay" --backend evdev
echo "  exit=$?"

echo
echo "### listen-only (--tap) mode, same backend"
"$BUILD/vorssaint-relay" --backend evdev --tap
echo "  exit=$?"

echo
echo "### the kernel's own view of input devices"
echo "  /sys/class/input entries: $(ls /sys/class/input 2>/dev/null | wc -l)"
echo "  /proc/bus/input/devices lines: $(wc -l < /proc/bus/input/devices 2>/dev/null || echo 0)"

rm -f /dev/uinput /dev/input/event0
rmdir /dev/input 2>/dev/null
echo
echo "(hand-made test nodes removed)"
