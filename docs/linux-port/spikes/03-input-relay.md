# WP-03 spike: evdev/uinput input relay

**Verdict: go, with one caveat that only hardware can close.** The relay, the
rules engine, the layout argument and the privilege split are all proven here.
The one thing this environment could not run is the kernel's uinput driver,
because the kernel it was built on does not contain it — a fact established
from `/proc/config.gz`, not inferred from a failure. The relay's device layer is
therefore an interface with two implementations; the real evdev/uinput one is
the daemon's default and is compiled on every build, and everything else was
proven through the in-process fake and a recorded evdev event stream.

Code: `spikes/wp03-input-relay/`. Privilege design: `docs/linux-port/PRIVILEGES.md`.

---

## 1. What uinput access was possible here, and how that was established

None, and it is not a permissions problem. The kernel has no uinput driver at
all, and cannot be given one, because it was built without loadable module
support.

```
$ zcat /proc/config.gz | grep -E "CONFIG_INPUT_EVDEV|CONFIG_INPUT_UINPUT|CONFIG_MODULES[ =]|CONFIG_MODULES is"
# CONFIG_MODULES is not set
CONFIG_INPUT_EVDEV=y
# CONFIG_INPUT_UINPUT is not set
```

That is the whole answer: `CONFIG_INPUT_UINPUT is not set` means the driver is
not in the image, and `CONFIG_MODULES is not set` means no module can ever be
loaded to supply it. Everything else follows and was checked anyway, because a
missing config symbol and a missing device node look the same from userspace
until you try:

```
$ bash spikes/wp03-input-relay/scripts/probe-uinput.sh /tmp/wp03/build
### kernel
6.18.44-fc-v24

### can a module be loaded at all
  modprobe: not installed
scripts/probe-uinput.sh: line 30: modprobe: command not found
ls: cannot access '/lib/modules': No such file or directory

### /proc/misc: a live uinput registers minor 223 here
  (no uinput minor registered)

### /sys/class/misc/uinput
ls: cannot access '/sys/class/misc/uinput': No such file or directory

### create the device nodes by hand and try to open them
crw-r--r-- 1 root root 10, 223 Sep 11 22:28 /dev/uinput
crw-r--r-- 1 root root 13, 64 Sep 11 22:28 /dev/input/event0

### the relay on the real evdev backend (no fallback, no mock)
relay: cannot start on backend 'evdev': open /dev/uinput: No such device (errno 19)
  exit=2

### listen-only (--tap) mode, same backend
relay: cannot start on backend 'evdev': no ID_INPUT_KEYBOARD/MOUSE/TOUCHPAD device found under /dev/input
  exit=2

### the kernel's own view of input devices
  /sys/class/input entries: 0
  /proc/bus/input/devices lines: 0
```

Reading that carefully:

- `mknod /dev/uinput c 10 223` **succeeds** — it is only a filesystem
  operation, and a node can be created for a driver that does not exist. This
  is the trap: the node's presence proves nothing.
- `open()` on it returns **ENODEV (19)**, which is the kernel saying no driver
  is registered at char major 10 minor 223. `/proc/misc` confirms the same from
  the other side: no uinput minor is registered.
- `/dev/input/event0` created by hand opens with **ENXIO (6)**, and
  `/sys/class/input` and `/proc/bus/input/devices` are both empty: `evdev` is
  compiled in (`CONFIG_INPUT_EVDEV=y`) but this microVM has no input hardware
  for it to expose.

The work package suggested using `libevdev-uinput` to create a virtual
**source** device so the whole path could be tested end to end. That is the
right idea and it is what I would do on any normal kernel — but it needs the
same `/dev/uinput` the relay's output needs, so it fails for exactly the same
reason. There is no arrangement of this container that produces a working
uinput device.

**Consequence for the code.** The device layer (`src/device.h`) is an interface
with `open` / `read` / `write` / `sync` / `describe` / `close`. Two
implementations:

| | `src/device_evdev.c` | `src/device_fake.c` |
|---|---|---|
| discovery | libudev, matching `ID_INPUT_KEYBOARD`, `ID_INPUT_MOUSE`, `ID_INPUT_TOUCHPAD` | two hard-coded descriptors |
| claim | `EVIOCGRAB` via `libevdev_grab()`, fatal if refused | a flag |
| output | one uinput device, union of the sources' capabilities | an in-memory sink |
| read | `poll()` over all source fds + `libevdev_next_event`, `SYN_DROPPED` resync | a queue with a synthetic clock |

The evdev backend is the daemon's **default** (`--backend evdev`), is compiled
on every build with `-Werror`, and is the code that would ship. It is not a
mock and has no fallback: when it cannot run it says why and exits 2, as above.

One bug this environment found and fixed: `--tap` (listen-only) originally
required `/dev/uinput` too. It should not — tap mode takes no grab and creates
no output device, so on a machine where only the relay proper cannot run, the
shortcut recorder should still work. The two failure messages above are now
different because the two modes genuinely need different things.

## 2. The rules

Both rules live in `src/rules.c`, which does no I/O at all: it takes one event
plus a nanosecond timestamp and returns 0..n events. That is what lets the same
code be driven by the fake, by a recorded stream, and by real hardware.

**Rule 1, Caps Lock tap → Escape / hold → Ctrl, threshold 200 ms.** The press
is withheld until the outcome is known. It resolves to a hold either when the
200 ms timer fires (the relay's `poll()` timeout is set from
`rules_deadline_ns()`, so this happens while the key is still down) or
immediately when any *other* key is pressed — which is what makes the key usable
as a real modifier. A release before either is a tap. Autorepeat on the source
key is swallowed; pointer motion does not resolve a pending decision.

**Rule 2, 40 ms per-keycode chatter filter.** A key *press* arriving within
40 ms of that same keycode's previous *release* is a bouncing switch, not a
keystroke, so it is dropped — and so is the release that closes it, or the
downstream key state would go out of balance. Kernel autorepeat (`value == 2`)
is not switch chatter and passes through untouched. The filter runs **before**
the tap/hold machine, so a bouncing Caps Lock produces one Escape, not several.

### 2.1 Unit tests

```
$ /tmp/wp03/build/test_rules
rule 1: Caps Lock tap -> Escape, hold -> Control (200 ms)
  50 ms tap emits Escape                         ok
  199 ms is still a tap                          ok
  200 ms exactly is a hold                       ok
  400 ms hold emits Control down then up         ok
  Caps+C before 200 ms is Ctrl+C                 ok
  source-key autorepeat is swallowed             ok
  mouse motion does not resolve a tap            ok
  unrelated keys pass through                    ok

rule 2: 40 ms per-keycode chatter filter
  one bounce inside 40 ms is dropped             ok
  a burst of bounces is dropped whole            ok
  a 60 ms repeat survives                        ok
  the filter is per keycode                      ok
  kernel autorepeat passes through               ok

configuration
  SetRules JSON is parsed                        ok
  malformed SetRules JSON is rejected            ok (bad value for "tap_threshold_ms")
  non-object SetRules is rejected                ok (not a JSON object)

chatter window applies to the remapped key too
  a bouncing Caps Lock yields one Escape         ok

device lifecycle: Enable(false) must actually release the devices
  Enable(true) then Enable(false) closes the backend ok
  a second Enable(true) opens a clean backend    ok
  the reopened backend relays correctly          ok
  closing the reopened backend counts too        ok

all rules tests passed
```

The lifecycle block is there because QA found a real defect in the first
revision: `Enable(false)` stopped the relay thread and logged "devices
released" while every source stayed `EVIOCGRAB`'d until the process exited. On
real hardware that is a keyboard the user cannot type on. The helper now holds
no backend object at all while disabled — `Enable(true)` creates one and claims,
`Enable(false)` closes it — and `release_devices()` is the single path by which
the relay ever stops, including when a source dies mid-stream. The test is not
vacuous: reverting `disable_cycle()` to the pre-fix behaviour fails it.

```
$ sed -i 's|disable_cycle(input_backend \*b) { b->close(b); }|disable_cycle(input_backend *b) { (void)b; }|' tests/test_rules.c
$ cmake --build /tmp/wp03/build -j4 && /tmp/wp03/build/test_rules | tail -6
device lifecycle: Enable(false) must actually release the devices
  FAIL Enable(true) then Enable(false) closes the backend: close() was not called: count 14 -> 14
  a second Enable(true) opens a clean backend    ok
  the reopened backend relays correctly          ok
  FAIL closing the reopened backend counts too: expected 16 closes, saw 15

FAILURES
```

### 2.2 Against a recorded evdev event stream

`gen-stream` writes a file that is byte-for-byte what reading an evdev
character device yields — packed `struct input_event` records — so a capture
taken on real hardware with `cat /dev/input/eventN > stream.bin` replays through
the identical path with no conversion.

```
$ /tmp/wp03/build/gen-stream /tmp/wp03/stream.bin
$ /tmp/wp03/build/vorssaint-relay --backend fake --replay /tmp/wp03/stream.bin -v
in            0.000000000 EV_KEY KEY_CAPSLOCK         1
in            0.045000000 EV_KEY KEY_CAPSLOCK         0
  >>          0.045000000 EV_KEY KEY_ESC              1      <- 45 ms tap
  >>          0.045000000 EV_KEY KEY_ESC              0
in            0.345000000 EV_KEY KEY_H                1
  >>          0.345000000 EV_KEY KEY_H                1
…
in            0.780000000 EV_KEY KEY_CAPSLOCK         1
TIM>          0.980000000 EV_KEY KEY_LEFTCTRL         1      <- timer at press+200 ms
in            1.180000000 EV_KEY KEY_CAPSLOCK         0
  >>          1.180000000 EV_KEY KEY_LEFTCTRL         0
in            1.480000000 EV_KEY KEY_CAPSLOCK         1
in            1.560000000 EV_KEY KEY_C                1
  >>          1.560000000 EV_KEY KEY_LEFTCTRL         1      <- other key resolves the hold
  >>          1.560000000 EV_KEY KEY_C                1
in            1.620000000 EV_KEY KEY_C                0
  >>          1.620000000 EV_KEY KEY_C                0
in            1.690000000 EV_KEY KEY_CAPSLOCK         0
  >>          1.690000000 EV_KEY KEY_LEFTCTRL         0
in            2.090000000 EV_KEY KEY_E                1
  >>          2.090000000 EV_KEY KEY_E                1
in            2.120000000 EV_KEY KEY_E                0
  >>          2.120000000 EV_KEY KEY_E                0
in            2.128000000 EV_KEY KEY_E                1      <- bounce  8 ms after release
in            2.134000000 EV_KEY KEY_E                0         (both dropped)
in            2.145000000 EV_KEY KEY_E                1      <- bounce 11 ms after release
in            2.154000000 EV_KEY KEY_E                0         (both dropped)
in            2.214000000 EV_KEY KEY_E                1      <- 60 ms: intentional, survives
  >>          2.214000000 EV_KEY KEY_E                1
…
in            3.498000000 EV_KEY KEY_CAPSLOCK         1
in            3.518000000 EV_REL REL_X                7      <- motion does not resolve
  >>          3.518000000 EV_REL REL_X                7
in            3.548000000 EV_KEY KEY_CAPSLOCK         0
  >>          3.548000000 EV_KEY KEY_ESC              1      <- still a tap
  >>          3.548000000 EV_KEY KEY_ESC              0
replayed 57 raw events
emitted  25 events to the output device
state: {"tap_hold":true,"tap_threshold_ms":200,"chatter":true,"chatter_ms":40,
        "tap_source":58,"tap_output":1,"hold_output":29,"tap_state":"idle",
        "in":29,"out":25,"chatter_dropped":4,"taps":2,"holds":2}
```

(Elided lines marked `…`; the full output is `tests/replay_expected.txt`, which
the `replay` ctest compares against byte for byte.)

```
$ cd /tmp/wp03/build && ctest --output-on-failure
    Start 1: rules
1/2 Test #1: rules ............................   Passed    0.00 sec
    Start 2: replay
2/2 Test #2: replay ...........................   Passed    0.02 sec

100% tests passed, 0 tests failed out of 2
```

## 3. Latency

Measured with `clock_gettime(CLOCK_MONOTONIC)` immediately after `read()`
returns and immediately after the matching `write()` + `SYN_REPORT`, over 10 000
synthetic events shaped like typing (press/release pairs 12 ms apart on rotating
keycodes, so both rules run on every event and neither suppresses anything).

```
$ for i in 1 2 3; do /tmp/wp03/build/vorssaint-relay --backend fake --bench 10000; done
```

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| p50 | 34 ns | 34 ns | 34 ns |
| p99 | 36 ns | 36 ns | 36 ns |
| p99.9 | ~1.7 µs | ~1.7 µs | ~1.7 µs |
| max | 20.2 µs | 20.9 µs | 25.4 µs |

That is the **userspace cost only**: rules engine plus an in-memory queue write.
It is not the whole story, because the fake backend makes no syscall, and on
real hardware `libevdev_uinput_write_event()` is a `write(2)` of a 24-byte
`struct input_event` to the uinput fd. So the bench can also perform that same
write to a real descriptor:

```
$ for i in 1 2 3; do /tmp/wp03/build/vorssaint-relay --backend fake --bench 10000 --write-to /dev/null; done
```

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| p50 | 315 ns | 316 ns | 316 ns |
| p99 | 356 ns | 373 ns | 351 ns |
| p99.9 | ~4.7 µs | ~4.8 µs | ~4.7 µs |
| max | 32.0 µs | 100.1 µs | 28.8 µs |

**How to read these honestly.** Each input event produces two `write(2)` calls
— the event itself and its `SYN_REPORT` — which account for about 282 ns of the
316 ns, roughly 140 ns each. The rules themselves are the remaining ~34 ns and
are not the cost of anything. What neither number contains is the
kernel side: evdev delivery from the hardware driver, the uinput driver's own
handling, the scheduler wakeup that gets the relay running after an event
arrives, and libinput's processing in the compositor. On real hardware the
scheduler wakeup dominates everything here by two to three orders of magnitude
— published measurements of `keyd`, which is the same design, put total added
latency in the low hundreds of microseconds, against a USB keyboard polling
interval of 1–8 ms.

So the useful conclusion is not the number but its shape: **the relay's own
work is ~0.3 µs per event, about 0.03 % of one 1 ms USB polling interval, and
is not where any latency the user could feel comes from.** The design has
headroom. This must still be re-measured on hardware in WP-S1 before the claim
is repeated to users; the number that matters there is end-to-end, measured
with a second uinput device as a known-time stimulus.

Caveat on the environment: this is a 4-CPU microVM on kernel 6.18 with no
isolation from whatever else the host is doing, which is what the ~20–100 µs
maxima are. The p50/p99 are stable to within 2 ns across runs, so the central
tendency is trustworthy even if the tail is not.

## 4. Layout mirroring

**The relay does not mirror the layout; it never touches it.** That is the whole
result, and it is worth stating precisely because the work package's phrasing
("a non-US xkb layout that the uinput device must mirror") suggests an
obligation the design does not have.

An evdev keycode names a physical switch position. The keysym it produces is
decided much later, by whoever owns the keymap: the Wayland compositor, or the X
server. The relay's uinput device is an ordinary keyboard as far as the session
is concerned, so the session applies the very same keymap to it that it applies
to the hardware keyboard. A relay working at keycode level is layout-correct by
construction, and stays correct across a live layout switch, because it has no
opinion to become stale.

`xkb-layout-demo` makes that concrete with libxkbcommon — the same library the
compositor uses — by compiling the same keycodes under four layouts:

```
$ /tmp/wp03/build/xkb-layout-demo
relay keycode     evdev    xkb           us           de           fr           ru
--------------- ------- ------ ------------ ------------ ------------ ------------
KEY_Q                16     24            q            q            a Cyrillic_shorti
KEY_W                17     25            w            w            z Cyrillic_tse
KEY_Y                21     29            y            z            y  Cyrillic_en
KEY_Z                44     52            z            y            w  Cyrillic_ya
KEY_A                30     38            a            a            q  Cyrillic_ef
KEY_M                50     58            m            m        comma Cyrillic_softsign
KEY_SEMICOLON        39     47    semicolon   odiaeresis            m Cyrillic_zhe
KEY_LEFTBRACE        26     34  bracketleft   udiaeresis dead_circumflex  Cyrillic_ha
KEY_CAPSLOCK         58     66    Caps_Lock    Caps_Lock    Caps_Lock    Caps_Lock
KEY_ESC               1      9       Escape       Escape       Escape       Escape
KEY_LEFTCTRL         29     37    Control_L    Control_L    Control_L    Control_L

What the relay's two rules emit, as keycodes:
  Caps Lock tap  -> KEY_ESC      = evdev 1 (xkb 9)
  Caps Lock hold -> KEY_LEFTCTRL = evdev 29 (xkb 37)
Those two numbers do not change with the layout, which is why the
rules engine never consults libxkbcommon at runtime.
```

Read the rows, not the columns. `KEY_Q` is `q` under `us` and `de` (the WP's
named pair — German QWERTZ swaps Y and Z, not Q), but `a` under `fr` (AZERTY)
and `Cyrillic_shorti` under `ru`. `KEY_Y` is `y` under `us` and `z` under `de`,
which is the QWERTY/QWERTZ swap seen directly. The evdev and xkb keycode columns
— the only thing the relay reads or writes — are identical for every layout.
(The xkb keycode is the evdev code plus 8; that offset is historical, from X11
keycodes starting at 8.)

The last two lines are the part that matters for these rules specifically:
`KEY_ESC` and `KEY_LEFTCTRL` are the same keycodes in every layout, so the
tap/hold rule is layout-independent without any special handling. The relay
links libxkbcommon nowhere; `xkb_demo.c` is a demonstration binary, not part of
the daemon.

**Where a layout *would* matter, and does not here.** A rule expressed as "map
the key that produces `ö`" rather than "map keycode 39" would need the session's
keymap, would need to be re-resolved on every layout change, and would be
ambiguous when two layouts are configured. If the shortcut UI ever lets users
pick keys by the character printed on them, the *app* must do that resolution
with libxkbcommon and send keycodes over `SetRules` — the helper must keep
taking keycodes only. Recorded as a constraint for WP-S1 and for the shortcut
recorder work package.

## 5. Privilege prototype

systemd and polkit do not run in this container, so the harness replaces exactly
those two things and nothing else. What is real: the shipped bus policy file,
the name claim, the D-Bus interface, the client, the helper and the rules engine
behind it. What is replaced: the system bus (a private `dbus-daemon` declared
`<type>system</type>`, whose `<policy context="default">` block is copied
verbatim from the distribution's `/usr/share/dbus-1/system.conf`, loading the
real `org.vorssaint.Helper1.conf` from an `includedir`), and the polkit
authority (a stub compiled in only with `-DWITH_POLKIT=OFF`).

```
$ POLKIT_BUILD=/tmp/wp03/build \
  bash spikes/wp03-input-relay/scripts/private-bus.sh /tmp/wp03/build-stub
```

**The bus policy refuses an impostor the name.**

```
===== 3. an unprivileged process tries to own org.vorssaint.Helper1 =====
  impostor: helper: cannot own org.vorssaint.Helper1: Permission denied
```

**The root helper claims it, and the bus confirms who owns it.**

```
===== 4. the real helper (root) claims the name =====
  helper: owning org.vorssaint.Helper1 on the system bus, uid=0, …
-- the bus agrees the name is owned, and by which uid --
  … GetNameOwner       -> string ":1.1"
  … GetConnectionUnixUser -> uint32 0
```

**The unprivileged client drives the whole API with no device access of its
own.** uid 1001, not in any input group; there is nothing under `/dev/input` for
it to open even if it were:

```
===== 5. what the unprivileged uid can reach without the helper =====
  uid=1001(vorssaint-test) gid=65534(nogroup) groups=65534(nogroup)
  ls: cannot access '/dev/uinput': No such file or directory

===== 6. the unprivileged client calls the helper =====
-- get-devices --
  GetDevices -> [{"node":"fake:keyboard0","name":"vorssaint fake keyboard","kind":"keyboard","grabbed":false},
                 {"node":"fake:mouse0","name":"vorssaint fake mouse","kind":"mouse","grabbed":false}]
-- enable --
  Enable(true) -> ok
-- set-rules {"tap_hold":true,"tap_threshold_ms":150,"chatter":true,"chatter_ms":25} --
  SetRules -> ok
-- get Rules --
  Rules = {"tap_hold":true,"tap_threshold_ms":150,"chatter":true,"chatter_ms":25,
           "tap_source":58,"tap_output":1,"hold_output":29,"tap_state":"idle",
           "in":2,"out":2,"chatter_dropped":0,"taps":1,"holds":0}
-- set-rules with a malformed document (must be rejected) --
  relayctl: call failed: org.freedesktop.DBus.Error.InvalidArgs: bad rules: bad value for "tap_threshold_ms"
-- listen 2 --
  listening for org.vorssaint.Helper1.Event for 2 s
  Event ts=1448049687467 type=1 code=35 value=1
  Event ts=1448089687467 type=1 code=35 value=0
  Event ts=1448154687467 type=1 code=58 value=1
  Event ts=1448204687467 type=1 code=58 value=0
  …
-- disable --
  Enable(false) -> ok
```

`code=58` is `KEY_CAPSLOCK` and `code=35` is `KEY_H`: tap mode reports the
**raw** event, not the rewritten one, which is what a shortcut recorder needs.

**Disable really releases, and re-enabling is clean.** The devices are handed
back on `Enable(false)`, and `GetDevices()` while disabled re-enumerates in
listen-only mode (no grab, no uinput) rather than replaying a cached answer:

```
-- disable --
  Enable(false) -> ok
-- after Enable(false): devices released, GetDevices re-enumerates without grabbing --
  GetDevices -> [{"node":"fake:keyboard0",…,"grabbed":false},{"node":"fake:mouse0",…,"grabbed":false}]
-- and Enable(true) again must open a clean backend, not a second grab --
  Enable(true) -> ok
  GetDevices -> [{"node":"fake:keyboard0",…},{"node":"fake:mouse0",…}]
  Enable(false) -> ok
```

and the helper log shows two complete claim/release pairs, not one claim and a
silent leak:

```
  helper: relay enabled on backend 'fake': [{"node":"fake:keyboard0",…}]
  helper: relay disabled, devices released
  helper: relay enabled on backend 'fake': [{"node":"fake:keyboard0",…}]
  helper: relay disabled, devices released
```

**The interface the bus advertises:**

```
===== 7. the interface as the bus sees it =====
   <interface name="org.vorssaint.Helper1">
    <method name="SetRules">
    <method name="GetDevices">
    <method name="Enable">
    <signal name="Event">
    <property name="Rules" type="s" access="read">
    <property name="Backend" type="s" access="read">
    <property name="Authorization" type="s" access="read">
```

**The authorisation gate actually gates.** Same binary, same bus, same client;
only the verdict changes. If these calls still succeeded, the gate would be
decorative:

```
===== 9. the authorization gate actually gates =====
-- enable --
  relayctl: call failed: org.freedesktop.DBus.Error.AccessDenied: org.vorssaint.helper.enable denied for :1.16
-- set-rules --
  relayctl: call failed: org.freedesktop.DBus.Error.AccessDenied: org.vorssaint.helper.set-rules denied for :1.17
-- get-devices --
  relayctl: call failed: org.freedesktop.DBus.Error.AccessDenied: org.vorssaint.helper.get-devices denied for :1.18
```

**The real polkit call is on the path, not stubbed.** The default build links
`libpolkit-gobject-1` and calls `polkit_authority_check_authorization_sync()`
against the caller's unique bus name. Run on the same private bus, that call
reaches libpolkit and fails from *inside* it, because no polkit authority is
running in this container:

```
===== 10. the same helper built WITH polkit, on the same bus =====
-- polkit symbols the binary imports --
                   U polkit_authority_check_authorization_sync
                   U polkit_authority_get_sync
                   U polkit_authorization_result_get_is_authorized
                   U polkit_authorization_result_get_is_challenge
                   U polkit_system_bus_name_new
  Authorization = polkit (polkit_authority_check_authorization_sync)
-- Enable() with the real authority check --
  relayctl: call failed: org.freedesktop.DBus.Error.Failed: authorization check failed:
    check_authorization failed: GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown:
    The name org.freedesktop.PolicyKit1 was not provided by any .service files
```

That error is the proof: `org.freedesktop.PolicyKit1` is polkit's own bus name,
and only real libpolkit code would look for it. The stub is reachable only with
`-DWITH_POLKIT=OFF`, which the installed build never uses, and it announces
itself in the `Authorization` property and in every log line so a stubbed build
cannot be mistaken for a real one.

**What could not be verified here:** that polkit *grants* `auth_admin_keep`
after an administrator authenticates, and that systemd starts the unit through
`Type=dbus` activation. Both need a machine with systemd and polkit running.
`vorssaint-helper.service` and the `.policy` file are written but have not been
loaded by systemd or `pkaction`; WP-S1 must run `systemd-analyze security
vorssaint-helper.service`, `pkaction --verbose`, and a real prompt before this
ships.

## 6. Recommended privilege model

**A root helper on the system bus with per-method polkit actions. Not the
`input` group, and not a udev rule that widens device permissions.** Full
argument, threat model, API, install/uninstall steps and the Flatpak limits are
in `docs/linux-port/PRIVILEGES.md`; the short version:

- The capability being granted is *keylogging plus input forgery*. Injection is
  the worse half: it is a privilege-escalation primitive against every other
  program the user runs, including their own `sudo` and polkit prompts.
- `usermod -aG input $USER` grants exactly that to **every process the user ever
  runs, permanently and invisibly**, so that one utility can remap Caps Lock. It
  survives uninstalling the app.
- The helper confines it to one audited binary with a three-method API, gated
  twice (bus policy for who may talk and own the name; polkit per method for who
  may make it act), revocable with `systemctl stop`, and removed completely when
  the package is.
- `70-vorssaint-uinput.rules` deliberately widens access for nobody. It only
  starts the helper when uinput appears and tags the relay's own output device
  so the relay does not grab its own output.
- Under Flatpak this feature is **unavailable**, and not because of a missing
  permission: a Flatpak cannot install a systemd unit, a polkit action or a
  D-Bus system policy at all, and `--device=all` removes the sandbox's
  restriction on `/dev/uinput` but not the kernel's `root:input 0660` on it.
  The hub must say so rather than degrade silently.

## 7. Reproducing all of this

```sh
apt-get install -y -o DPkg::Lock::Timeout=600 \
    cmake build-essential pkg-config dbus \
    libevdev-dev libudev-dev libxkbcommon-dev libsystemd-dev libpolkit-gobject-1-dev

cd spikes/wp03-input-relay
cmake -S . -B /tmp/wp03/build      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/wp03/build -j4                       # builds clean under -Werror
cmake -S . -B /tmp/wp03/build-stub -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_POLKIT=OFF
cmake --build /tmp/wp03/build-stub -j4

bash scripts/probe-uinput.sh /tmp/wp03/build            # § 1
/tmp/wp03/build/test_rules                              # § 2.1
(cd /tmp/wp03/build && ctest --output-on-failure)        # § 2.2
/tmp/wp03/build/gen-stream /tmp/wp03/stream.bin
/tmp/wp03/build/vorssaint-relay --backend fake --replay /tmp/wp03/stream.bin -v
/tmp/wp03/build/vorssaint-relay --backend fake --bench 10000                    # § 3
/tmp/wp03/build/vorssaint-relay --backend fake --bench 10000 --write-to /dev/null
/tmp/wp03/build/xkb-layout-demo                         # § 4
POLKIT_BUILD=/tmp/wp03/build scripts/private-bus.sh /tmp/wp03/build-stub        # § 5
```

On a machine with real input hardware, the same tree runs the real path:

```sh
sudo /tmp/wp03/build/vorssaint-relay --backend evdev --tap           # listen only, no grab
sudo /tmp/wp03/build/vorssaint-relay --backend evdev                 # grab + relay
sudo /tmp/wp03/build/vorssaint-relay --backend evdev --tap --record capture.bin
/tmp/wp03/build/vorssaint-relay --backend fake --replay capture.bin -v
evtest                                                               # shows "Vorssaint Relay"
```

## 8. What WP-S1 must still do

1. Run the relay against real hardware on GNOME (Wayland), Plasma 6 and Sway:
   confirm the grab takes, the virtual device appears to libinput with the right
   capabilities, and the compositor applies the session keymap to it.
2. Re-measure latency end to end on hardware (§ 3) with a second uinput device
   as a known-time stimulus. Do not repeat the 0.3 µs figure to users; it is the
   relay's own cost, not what they feel.
3. Load `vorssaint-helper.service` under a real systemd and run
   `systemd-analyze security` on it; confirm `Type=dbus` activation works.
4. Verify the polkit actions against a real authority, including the
   `auth_admin_keep` prompt and the `AUTHZ_CHALLENGE` path, which never ran here.
5. Decide the four open questions in `PRIVILEGES.md` § 7 — in particular whether
   `Enable(true)` should be bound to the logind session that requested it, and
   whether enabling the relay should be journalled.
6. Handle hot-plug: the relay currently enumerates once at `Enable(true)`. A
   keyboard plugged in afterwards is not grabbed, so its events reach the
   compositor unfiltered. A `udev_monitor` on the `input` subsystem is the fix
   and is not written yet.
