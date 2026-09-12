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

**Then `docs/linux-port/RELAY_RULES.md`** for the rules engine: which macOS
`…Support.swift` each rule is a port of, what the C does differently and why,
how many of the Swift test vectors were carried over, and the latency numbers
with every rule enabled.

```
src/          the daemon
  helper.c        the D-Bus service: methods, properties, the relay thread
  rules.c/.h      the rules engine: defaults, dispatch, the SetRules and
                  SetContext readers, the Rules property, the notice ring
  rules_debounce.c  keyboard_debounce, mouse_click_debounce
  rules_scroll.c    scroll_invert, smooth_scroll
  rules_super.c     super_key
  rules_mouse.c     mouse_button_shortcut and the hold-and-drag gesture
  rules_quit.c      quit_protection
  device.h        the device layer interface
  device_evdev.c  libudev discovery, EVIOCGRAB, uinput, udev_monitor hot-plug
  device_fake.c   in-process queues, for a machine with no uinput
  session.c/.h    logind session binding, behind an interface with a fake
  fan.c/.h        hwmon pwm writes, read-back verification, the watchdog
  ddc.c/.h        DDC/CI framing and transport, with a fake monitor
  caps.c/.h       GetCapabilities: what this machine can actually do
  grabholder.c/.h who is holding a device open, for a refused EVIOCGRAB
  polkit_check.c  the authorisation gate
  relay.c         a CLI for the device layer: --replay, --bench, --tap,
                  --rules-file
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

A bare configure defaults to `RelWithDebInfo`, which is what gets installed:
`_FORTIFY_SOURCE` does nothing without optimisation, so a build with no type
set would silently differ from the shipped one. Debug is supported and must
stay warning-clean; it just is not what to install, and CMake says so.

**The build must be clean under every type, not just one.**
`-Wformat-truncation`, `-Wmaybe-uninitialized` and `-Wrestrict` reason
differently at each optimisation level -- GCC inlines more at `-O3`, so it
knows more about what a buffer can hold and sees overlaps it cannot see at
`-O0`. With `-Werror` a warning in any of them is a build failure for whoever
hits it first, so check all four:

```sh
bash linux/helper/scripts/build-matrix.sh
```

That configures, builds and runs `ctest` under no build type, Debug, Release
and RelWithDebInfo, and fails if any of them warns.

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
sudo build/vorssaint-relay --backend evdev \
     --rules-file linux/helper/tests/bench_rules.json # grab + relay, all rules
evtest                                               # shows "Vorssaint Relay"
sudo build/vorssaint-relay --backend evdev --tap --record capture.bin
build/vorssaint-relay --backend fake --replay capture.bin -v \
     --rules-file linux/helper/tests/replay_rules.json

sudo pkexec linux/helper/dist/install.sh
systemd-analyze security vorssaint-helper.service
pkaction --action-id org.vorssaint.helper.enable --verbose
vorssaint-helperctl get-capabilities
```
