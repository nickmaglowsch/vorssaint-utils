# `vorssaint-helper`

The privileged half of the Linux port: one root daemon on the D-Bus system
bus that owns the input relay (evdev grab + uinput re-emit), hwmon fan
control, and DDC/CI over `/dev/i2c-*`. Everything else the app needs
— logind backlight, inhibitors, UPower, BlueZ, PipeWire, portals — is
unprivileged and does not come near this directory.

**Read `docs/linux-port/PRIVILEGES.md` first.** It is the threat model, the
API contract, and — in § 8 — the honest account of which claims about this
code have been proven and which have only been argued. Grown from
`spikes/wp03-input-relay`, which is left untouched as the record of the
Phase 0 gate decision.

```
src/          the daemon
  helper.c        the D-Bus service: methods, properties, the relay thread
  rules.c/.h      the rules engine: pure logic, no syscalls
  device.h        the device layer interface
  device_evdev.c  libudev discovery, EVIOCGRAB, uinput, udev_monitor hot-plug
  device_fake.c   in-process queues, for a machine with no uinput
  session.c/.h    logind session binding, behind an interface with a fake
  fan.c/.h        hwmon pwm writes, read-back verification, the watchdog
  ddc.c/.h        DDC/CI framing and transport, with a fake monitor
  caps.c/.h       GetCapabilities: what this machine can actually do
  grabholder.c/.h who is holding a device open, for a refused EVIOCGRAB
  polkit_check.c  the authorisation gate
  relay.c         a CLI for the device layer: --replay, --bench, --tap
client/       the client library the platform layer links, and its CLI
dist/         everything that gets installed, and install.sh/uninstall.sh
tests/        ctest suites; `bash tests/test_install.sh` needs two env vars
scripts/      private-bus.sh, the end-to-end scenario; probe-uinput.sh
```

## Building

```sh
apt-get install -y cmake build-essential pkg-config dbus \
    libevdev-dev libudev-dev libsystemd-dev libpolkit-gobject-1-dev

cmake -S linux/helper -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
(cd build && ctest --output-on-failure)
```

`RelWithDebInfo` is not optional advice: `_FORTIFY_SOURCE` does nothing
without optimisation, and the CMake file warns if you configure a build where
the hardening flags would be silently inert.

## The end-to-end scenario

There is no systemd and no polkit in a plain container, so `private-bus.sh`
replaces exactly those two things — plus logind sessions and `/sys/class/hwmon`
— and is explicit about which is which. It runs a real `dbus-daemon` declared
`<type>system</type>` loading the shipped `dist/org.vorssaint.Helper1.conf`,
and drives the helper from two unprivileged users standing in for two seats.

```sh
cmake -S linux/helper -B build-stub -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_POLKIT=OFF
cmake --build build-stub -j4
POLKIT_BUILD=$PWD/build bash linux/helper/scripts/private-bus.sh $PWD/build-stub
```

`-DWITH_POLKIT=OFF` compiles a stub authority in place of the real one. It is
loud about it — it says so in the `Authorization` property and in every log
line — and it is never what gets installed. `POLKIT_BUILD` points the last
section of the scenario at a real build so the two can be compared on the same
bus.

## On real hardware

The parts this container cannot run (see `PRIVILEGES.md` § 8):

```sh
sudo build/vorssaint-relay --backend evdev --tap     # listen only, no grab
sudo build/vorssaint-relay --backend evdev           # grab + relay
evtest                                               # shows "Vorssaint Relay"
sudo build/vorssaint-relay --backend evdev --tap --record capture.bin
build/vorssaint-relay --backend fake --replay capture.bin -v

sudo pkexec linux/helper/dist/install.sh
systemd-analyze security vorssaint-helper.service
pkaction --action-id org.vorssaint.helper.enable --verbose
vorssaint-helperctl get-capabilities
```
