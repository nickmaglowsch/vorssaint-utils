# Privilege model: `vorssaint-helper`

**Status: current for WP-S1 (daemon side).** Written during WP-03
(`spikes/03-input-relay.md`) scoped to the input relay, and extended here to
the two other privileged paths the port needs: hwmon `pwm` writes and DDC/CI
over `/dev/i2c-*`. The implementation it describes is `linux/helper/`; the
WP-03 spike is left untouched as the record of the gate decision.

Read § 8 before trusting any of it: this was developed in a container with no
uinput, no input devices, no hwmon, no i2c, no polkit authority and no systemd
as pid 1, and that section says exactly which claims here are proven and
which are only argued.

`PLAN.md` § 4.4 settles the shape: one helper, a system-bus name, polkit
actions, udev rules. This file is the argument for that shape, the API it
exposes, and how it is installed and removed.

---

## 1. What is actually being asked for

The upstream macOS app uses a modifying `CGEventTap`. Every mouse and keyboard
feature — quit protection, snippets, cleaning mode, the shortcut recorder —
depends on seeing an event *and being able to change or swallow it* before the
focused application does.

Linux has no session-wide modifying tap. Wayland deliberately does not offer
one, and the X11 equivalent (`XGrabKey`, `XRecord`) does not survive on Wayland
sessions. The only mechanism that works on every compositor is the one `keyd`
and `interception-tools` use:

1. open every `/dev/input/event*` that is a keyboard, mouse or touchpad;
2. `EVIOCGRAB` each one, so the kernel stops delivering its events to anyone
   else, including the compositor;
3. apply rules;
4. write the result to one `/dev/uinput` device, which the compositor picks up
   as an ordinary new keyboard and mouse.

Step 2 requires read access to `/dev/input/event*`. Step 4 requires write
access to `/dev/uinput`. On a stock distribution both are `root:input 0660`
(uinput is often `root:root 0660` with no rule at all).

## 2. Threat model

Be blunt about what this capability is. **A process that can do steps 1 to 4 is
a keylogger with a synthetic-input generator attached.** Concretely it can:

- read every keystroke in the session, including passwords typed into a
  password manager, a `sudo` prompt, a browser, or a full-disk-encryption
  unlock on a live system;
- read every pointer event, so it can reconstruct on-screen PIN entry;
- **forge** input into any focused window: type into a root shell the user has
  open, approve a polkit prompt, accept a browser permission dialog, or drive
  a package manager;
- swallow input, including the compositor's own kill-session shortcut.

The last two matter most: interception alone is a confidentiality problem;
injection is a privilege-escalation primitive against every other program the
user runs, and against the user's own authentication prompts. This is why the
capability gets its own boundary rather than a checkbox.

Threats explicitly out of scope: a compromised kernel; an attacker who is
already root; physical hardware keyloggers; a malicious compositor. If any of
those hold, nothing in this document helps.

## 3. Why the `input` group is rejected

The most common packaging shortcut for input tools is one of:

```sh
usermod -aG input "$USER"                        # or
echo 'KERNEL=="uinput", MODE="0660", GROUP="input"' > /etc/udev/rules.d/…
```

This is rejected for the port. The reason is not that it is insecure in the
abstract; it is that **it grants the capability to the wrong subject, for the
wrong duration, with no way to see or revoke it**:

| | `input` group | `vorssaint-helper` |
|---|---|---|
| Who gets keylogger capability | every process the user ever runs, forever | one binary, audited, that we ship |
| Granularity | all input devices, read and write | the three input device classes the relay needs, and nothing else |
| Revocation | log out, log in again after `gpasswd -d` | `Enable(false)` releases every grab immediately; `systemctl stop` ends the process |
| Visible to the user | no: a group membership nobody reads | yes: a polkit prompt naming the action |
| Survives uninstalling the app | yes | no: the unit and rules go with the package |
| Blast radius of a bug in *our* code | same as any other bug | confined to a root process with a nine-method API, each method gated separately |

The decisive line is the first. Adding a user to `input` means a malicious
npm postinstall script, a browser extension's helper process, or any other
program that user runs — none of which we wrote or reviewed — can silently read
every keystroke on the machine. That is a permanent, invisible, system-wide
downgrade of the user's security, handed out so that one utility can remap Caps
Lock. The helper keeps the capability inside one binary whose entire source is
`linux/helper/src`: about 4050 lines of C including its headers and their
comments, built with `-Wall -Wextra -Wshadow -Wformat=2 -Werror`.

The same argument applies unchanged to the `i2c` group, which is the shortcut
every DDC/CI tool documents, and it is worse there: see § 4.3.

Neither udev rule the port ships (`70-vorssaint-uinput.rules`,
`71-vorssaint-i2c.rules`) widens access for anybody. The first makes sure the
helper is started when uinput appears and tags the relay's own output device so
the relay does not grab it; the second only marks which i2c buses are display
channels.

## 4. The design

```
  ┌─────────────────────────┐         ┌──────────────────────────────────┐
  │ Vorssaint (the app)     │         │ vorssaint-helper (root)          │
  │ uid = the desktop user  │         │ systemd system unit, Type=dbus   │
  │ no device access at all │         │                                  │
  │ not in group input      │         │  /dev/input/event*  (read, GRAB) │
  │ not in group i2c        │         │  /dev/uinput        (write)      │
  │                         │         │  /sys/class/hwmon/*/pwmN (write) │
  │                         │         │  /dev/i2c-*  (addr 0x37 only)    │
  └───────────┬─────────────┘         └───────────────┬──────────────────┘
              │                                       │
              │  D-Bus SYSTEM bus                     │
              │  org.vorssaint.Helper1                │
              └───────────────┬───────────────────────┘
                              │
              ┌───────────────┴────────────────┐
              │ gate 1: dbus-daemon busconfig  │  who may talk / own the name
              │ gate 2: polkit actions         │  who may make it act
              └────────────────────────────────┘
```

Two gates, because they answer different questions and fail differently.

**Gate 1, the bus policy** (`org.vorssaint.Helper1.conf`, installed to
`/usr/share/dbus-1/system.d/`). Only `root` may *own* the name — so no
unprivileged process can squat it and collect the rules the app sends, or
impersonate the helper to the app. Any local user may *send* to the interface,
deliberately: a refusal should come back from the helper as a typed error the
capabilities page can explain, not as an opaque bus rejection.

**Gate 2, polkit** (`org.vorssaint.helper.policy`, installed to
`/usr/share/polkit-1/actions/`). Per method, against the caller's identity as
resolved by the bus daemon from its unique name — the caller never states its
own pid or uid.

| Method | Action | Default | Why |
|---|---|---|---|
| `Enable(b)` | `org.vorssaint.helper.enable` | `auth_admin_keep` | turns the capability on; this is the moment the user should see a prompt |
| `SetRules(s)` | `org.vorssaint.helper.set-rules` | `auth_admin_keep` | a rule is a program that rewrites keystrokes; changing it is as strong as enabling |
| `SetContext(s)` | `org.vorssaint.helper.set-context` | `auth_admin_keep` | one string, no rule change, but called on every focus change; separated so it can be allowed on its own and so it does not bury the audit line for a rule change |
| `SetFanPwm`, `SetFanAuto`, `FanHeartbeat` | `org.vorssaint.helper.fan-control` | `auth_admin_keep` | a fan set too slow damages hardware silently, hours later |
| `DdcWrite`, `DdcRead` | `org.vorssaint.helper.ddc` | `auth_admin_keep` | `/dev/i2c-*` is a general bus, not a monitor API; see § 4.3 |
| `GetDevices()` | `org.vorssaint.helper.get-devices` | `yes` | only names, already readable in `/proc/bus/input/devices` |
| `GetCapabilities()` | `org.vorssaint.helper.get-capabilities` | `yes` | reports what the machine can do, changes nothing |

The two promptless actions are split out deliberately: the capabilities page
must be able to show what *would* be grabbed, and which fans and buses exist,
without handing out the power to use any of them. Everything that acts is
`auth_admin_keep`, not `auth_admin`: one administrator prompt per session, not
one per rule edit or brightness step. `allow_inactive` is `no` on all seven,
so a user on a switched-away session cannot enable the relay, drive the fans or
change the brightness on the active one.

Seven actions rather than one, because a user who wants a quieter fan should
not have to grant a keylogger to get it, and the hub can say precisely which
capability a feature needs.

### 4.1 The API

Ten methods, one signal and five properties. The narrowness is the point: the
app never receives a file descriptor for an input device, an i2c bus or a
sysfs attribute, and the helper exposes no raw transfer or raw write method
for any of them.

```
interface org.vorssaint.Helper1 {          # at /org/vorssaint/Helper1
  methods:
    Enable(in  b enable)          # claim/release devices, start/stop the relay
    SetRules(in  s json)          # the rule document; schema below
    SetContext(in  s json)        # {"focused_app_id": "…"}; see below
    GetDevices(out s json)        # [{node,name,kind,grabbed}, …]
    GetCapabilities(out s json)   # uinput, hwmon pwm, i2c buses; see below

    SetFanPwm(in s hwmon, in u channel, in y value)   # 0..255 duty cycle
    SetFanAuto(in s hwmon, in u channel)              # back to the chip's curve
    FanHeartbeat()                                    # defer the watchdog

    DdcWrite(in s bus, in y vcp, in q value)
    DdcRead(in s bus, in y vcp, out q current, out q max)
  signals:
    Event(t timestamp_ns, s kind, u device,
          q type, q code, i value, s detail)
  properties (read-only, no prompt):
    Rules          s   active configuration + context + counters
    Backend        s   "evdev" or "fake"
    Authorization  s   which authorisation backend is compiled in
    Fan            s   channels under manual control, watchdog time remaining
    Owner          s   which session holds the relay, and how that was resolved
}

errors:
    org.vorssaint.Helper1.Error.NotOwner      another session holds the relay
    org.freedesktop.DBus.Error.InvalidArgs    a malformed document or name
    org.freedesktop.DBus.Error.AccessDenied   polkit refused
    org.freedesktop.DBus.Error.IOError        the device did not cooperate
```

#### The `Event` signal

```
t  timestamp_ns   CLOCK_MONOTONIC, the same clock the rules run on
s  kind           "input" | "rule" | "hotplug"
u  device         source device index; 0 for "rule" and "hotplug"
q  type, q code, i value    the evdev triple; 0/0/0 when kind is not "input"
s  detail         JSON object for "rule", plain text for "hotplug",
                  empty for "input"
```

- **`kind: "input"`** is emitted **only in tap mode**, which is the shortcut
  recorder and the snippet trigger: the relay listens without grabbing and
  without an output device, the app shows the user what they pressed, and the
  mode ends when recording ends. Outside tap mode the helper emits no input at
  all, so the ordinary state of an enabled relay is that no keystroke ever
  crosses the bus. That is deliberate: a helper that streamed every event to a
  session process would have moved the keylogger into the unprivileged half
  and gained nothing.

  Only `EV_KEY` transitions are sent — a key or a button going down, up or
  repeating, with the source device index so a recorder can tell two keyboards
  apart. Pointer motion and the wheel are never sent: they are a continuous
  stream with nothing to record.

  **It is rate-limited**, to 400 signals per second, because this is the one
  place the helper hands raw keystrokes to a session process and a stuck key
  or a 1 kHz gaming mouse would otherwise turn the recorder into an unbounded
  D-Bus sender. No human produces 400 key transitions a second. Events dropped
  by the limiter, and events dropped because the ring filled, are both counted
  and reported, so a client that hits either learns that it did.

- **`kind: "rule"`** is what a rule decided that the app must act on, because
  the helper must not. `detail` is a JSON object naming the rule:

  ```json
  {"rule":"mouse_button_shortcut","action":"workspace_left","button":276}
  {"rule":"quit_protection","shortcut":"quit","outcome":"blocked",
   "app_id":"firefox","native_quit":true}
  {"rule":"super_key","action":"tap_key","key":88}
  ```

  `outcome` is one of `holding`, `armed`, `blocked`, `confirmed`. `action` for
  the gesture is one of `workspace_left`, `workspace_right`, `overview`,
  `app_overview`. These are emitted whether or not the relay is in tap mode —
  they carry no keystroke, and the app cannot do its half of the feature
  without them. They are drained *before* the input queue, so a flood of tap
  events cannot crowd one out, and they are never rate-limited.

- **`kind: "hotplug"`** carries the line the log carries: `added
  /dev/input/event7 (Keychron K2), grabbed`.

#### The rule document

`SetRules` takes one JSON object with a section per rule, parsed by a
hand-written reader in `rules.c` rather than a JSON library, because it is the
one untrusted input a root process accepts. Unknown keys are ignored; a
missing key keeps the default; **a present key whose value is out of range or
of the wrong type rejects the whole document** with
`org.freedesktop.DBus.Error.InvalidArgs`, and the previous rules stay in
force. Every rule ships off: a relay that starts rewriting input the moment it
is enabled is not something a capabilities page can honestly describe.

```json
{
  "keyboard_debounce":     {"enabled": false, "window_ms": 5,
                            "key_windows": "30:20,18:0"},
  "mouse_click_debounce":  {"enabled": false, "window_ms": 25},
  "scroll_invert":         {"enabled": false, "vertical": true,
                            "horizontal": false, "mouse": true,
                            "touchpad": false,
                            "shift_redirects_vertical": false},
  "smooth_scroll":         {"enabled": false, "step": 40, "response": 65,
                            "mouse": true, "touchpad": false},
  "super_key":             {"enabled": false, "source": 58,
                            "modifiers": [29, 56, 125],
                            "tap_action": "escape", "tap_key": 0,
                            "hold_threshold_ms": 500, "led": true},
  "mouse_button_shortcut": {"enabled": false,
                            "bindings": [{"input": 275, "modifiers": [29],
                                          "key": 15}],
                            "gesture_button": 0,
                            "gesture_follows_drag": false},
  "quit_protection":       {"enabled": false,
                            "quit":  {"enabled": false, "mode": "hold",
                                      "hold_ms": 800, "double_ms": 600,
                                      "extra_modifier": "shift",
                                      "scope": "all", "exceptions": []},
                            "close": {"…as quit…": null}}
}
```

Ranges, all enforced: `window_ms` 0..500 for the key filter and 5..100 for the
click filter (out of range falls back to the documented default rather than
clamping, which is what the macOS sanitisers do); `step` 20..100 and
`response` 0..100, both clamped; `hold_threshold_ms` ≤ 5000; `hold_ms`
250..2000 and `double_ms` 200..1500, both clamped; every keycode ≤ `KEY_MAX`;
at most 4 modifiers per chord, 16 bindings, 32 per-key windows and 16
exceptions per shortcut. `mode` is `hold`, `double_press` or `extra_modifier`;
`scope` is `all`, `selected_only` or `all_except_selected`; `tap_action` is
`none`, `escape`, `caps_lock` or `key`; `extra_modifier` is `shift` or `alt`.
An unknown enumeration value is an error, never a silent default. A
`mouse_button_shortcut` binding may only name an extra button
(`BTN_SIDE`…`BTN_TASK`) or one of the two tilt pseudo-inputs (−1, −2); left,
right and middle are refused, because they belong to the click filter.

One cross-rule check runs after the whole document is read: a `super_key`
source that another enabled rule already answers is refused
(`"bad value for \"super_key.source (already claimed by another rule)\""`).
That is `SuperKeyMappingGuard.hasMappingConflict` in the terms this engine
has — see `RELAY_RULES.md` § 5 for what else of that guard survives the move
to Linux, and what has no hazard left to guard against.

Every string the document carries is echoed back inside the `Rules` property,
which the helper builds by hand. So an identifier is restricted to letters,
digits and `. - _ + : ,` and **refused** if it contains anything else, rather
than escaped: a quote arriving from an unprivileged caller and coming back out
inside a JSON object the helper generates would be the helper producing
malformed JSON for every client that reads it.

#### `SetContext`

```json
{"focused_app_id": "org.gnome.TextEditor"}
```

The app pushes which application has focus, because the relay cannot see it: a
grabbed input device says nothing about which window is on top. Only
`quit_protection`'s per-app scope uses it today.

**Why it is a separate method rather than a field of the rules document.** The
focused application changes every time the user moves between windows — many
times a minute, sometimes several times a second. The rules change when a
person opens a settings page. Riding the rules document would mean
re-validating the whole schema and calling `rules_reconfigure()` on every focus
change, and `rules_reconfigure()` deliberately releases everything the engine
is holding down on the user's behalf: a super key held across an alt-tab would
drop its modifiers mid-chord, every single time.

It carries the **same authorisation as `SetRules`**: a polkit action and the
same session-ownership check, because it is the same privilege — telling the
relay how to treat keys it has already claimed. It has its own action id,
`org.vorssaint.helper.set-context`, so an administrator may allow the
frequent, low-consequence one without allowing the other, and so the audit
line for a rule change is not buried under focus changes. It is
`auth_admin_keep` by default like the rest: the app has no business telling a
root daemon anything until the user has said the relay may run.

**Both documents are capped at 64 KiB** (`RULES_JSON_MAX`), checked on length
before a byte is parsed, and rejected with `InvalidArgs` above that. A full
rules document with every section populated is under 1 KiB, so the cap is
nearly two orders of magnitude of slack — but it has to exist, because the
reader is not a streaming parser: `json_find()` restarts from the beginning of
the string for each key, so the work is O(keys × length) and an unbounded
string would be an unbounded amount of a root process's time for one
unprivileged D-Bus call. D-Bus's own 128 MiB message limit is not a useful
bound here. The check is in `rules_config_from_json` and
`rules_context_from_json` so it holds for every caller, and repeated in
`method_set_rules` and `method_set_context` so the refusal can name the size
that was sent.

`GetCapabilities` answers by *trying*, never by inferring from a name, because
a device node can exist with no driver behind it — which is exactly what
`/dev/uinput` did in the WP-03 container. The shape, taken verbatim from a run
in that container:

```json
{"uinput":{"available":false,"path":"/dev/uinput",
           "reason":"open /dev/uinput: No such file or directory (errno 2)"},
 "input":{"dir":"/dev/input","event_nodes":-1},
 "hwmon":{"root":"/sys/class/hwmon","devices":[]},
 "i2c":{"buses":[]},
 "fan_watchdog_ms":10000,
 "ddc":{"address":"0x37","hardware_tested":false}}
```

`event_nodes: -1` means `/dev/input` does not exist, which is not the same as
zero devices; `reason` carries the errno, because `ENOENT` (no such node) and
`ENODEV` (a node whose driver is not in the kernel) are different problems with
different fixes, and only the first is a packaging mistake.

### 4.2 Session binding

**`Enable(true)` binds the relay to the caller's logind session.** This was the
first open question at the end of WP-03 and the Phase 0 gate settled it: the
helper records `sd_pid_get_session` of the sender's pid, taken from
`sd_bus_creds` — never from anything the caller says about itself — together
with its uid. `Enable(false)` and `SetRules` from a different session are
refused with `org.vorssaint.Helper1.Error.NotOwner`, carrying a message that
names both sessions so the hub can explain it rather than showing a switch
that will not move.

Three deliberate edges:

- **uid 0 is exempt.** Root can `systemctl stop` the unit, so refusing it here
  would buy nothing and would lock an administrator out of a relay left
  enabled by a session that has gone away.
- **A caller outside any session** (a system service; any process on a machine
  without logind) binds with an empty session id, and then only the same uid,
  also outside any session, may change it. A sessionless binding does not
  become a wildcard that every session on the machine inherits.
- **`Enable(false)` clears the binding**, so the next session may take the
  relay. Fast user switching therefore needs no special case: the relay is
  released when its owner releases it or when the daemon stops, and the
  incoming session enables its own.

`SetContext` is ownership-checked on the same terms as `SetRules`: a second
session must not be able to tell the relay which application has focus for a
session it does not hold.

`GetDevices` and `GetCapabilities` are *not* ownership-checked. They report and
change nothing, and a second session being unable to see what is going on is
worse for the user than it is better for security.

The lookup sits behind a one-function interface (`src/session.h`) with the
logind implementation as the daemon's default and a fake for the tests, because
logind cannot run in the container this was developed in. See § 8.

### 4.3 Fans and DDC: two different ways to do harm

**hwmon.** `SetFanPwm` writes `pwmN_enable` (manual) and then `pwmN`, in that
order — the reverse is accepted by most drivers and then overwritten by the
firmware at its next update, which the user experiences as a fan that obeys for
a second and drifts back. Both writes are **verified by reading the attribute
back**: a sysfs store handler can accept a write and clamp, round or drop it,
and the write's return value cannot tell the difference. The helper refuses to
report a duty cycle it has not read back.

The failure mode that matters is not a bad write, it is a **good write and a
dead client**: a fan left at 15 % by an app that crashed cooks the hardware
silently, hours later, with nothing on screen. So manual control is armed with
a deadline. The client must call `FanHeartbeat` within **10 seconds** or every
channel this helper put into manual mode is handed back to the firmware's
automatic curve — and back to *the value it had before the helper touched it*,
not to a fixed `2`, because a board whose fans were at full speed must return
to full speed. The same restore happens on `Enable(false)`, on `SIGTERM` and at
exit. A crash, a disconnect, a suspend and a killed app all converge on the
state the machine boots in.

A `hwmon` name from the bus is untrusted input that becomes a path in a root
process, so it is **validated, not escaped**: exactly `hwmon` followed by one
to eight digits, checked before the filesystem is touched at all.

That stops traversal in the *name*. It does not stop traversal through a
**symlink already on disk** — a `hwmon0` that is a link to somewhere else
resolves to somewhere else, and the name was never wrong. The WP-S1 review
demonstrated this live: a planted link let `SetFanPwm` write through it and
return success. `src/pathguard.c` is the second layer, and the rule is not
"no symlinks", because that would be wrong on every real machine:
`/sys/class/<class>/<name>` is *always* a symlink into `/sys/devices` — that
is how the sysfs class model works. So it is split:

- the class entry (`hwmonN`) may be a symlink, but its resolved target must
  stay inside the boundary: the root's own `realpath`, or anywhere under
  `/sys` when the root is itself under `/sys`. Outside that it is refused with
  `ELOOP`, naming where it would have landed, and the entry is not listed by
  `GetCapabilities` either — offering a control the write path will refuse is
  worse than not offering it;
- the attributes (`pwmN`, `pwmN_enable`) are opened `O_NOFOLLOW` and must be
  regular files. Real sysfs attributes never are anything else, and letting
  the kernel refuse at `open(2)` leaves no window between a check and the use;
- `/dev/i2c-N` is opened `O_NOFOLLOW` and must be a character device, for the
  same reason.

**This is depth, not the primary defence.** `/sys/class/hwmon`, `/sys/devices`
and `/dev` are root-owned and not writable by the unprivileged caller, so it
cannot plant the link in the first place; the primary defence is that the
caller never names a path at all, only a `hwmonN`. The second layer is for
what the first does not cover: a caller that is already root by another route,
a container that bind-mounts something writable over part of `/sys`, and our
own bugs.

**DDC/CI.** This is the one that deserves more suspicion than it usually gets.
`/dev/i2c-*` is root-only on every distribution, and the common packaging
shortcut for DDC tools is a udev rule granting an `i2c` group write access plus
`usermod -aG i2c $USER`. That is rejected here for the reason the `input` group
is rejected in § 3, and it is worse in one respect: **an i2c bus is not a
monitor API**. It is a general bus, and whatever else is on those pins is
reachable from it — on many boards the SPD EEPROMs of the memory modules, which
are writable on DDR4 and later and where a bad write is a module that no longer
trains, and on some boards the embedded controller.

The helper therefore: refuses any bus name that is not exactly `i2c-N`; opens
only address `0x37` (`I2C_SLAVE`) and offers no way to select another; exposes
`DdcWrite`/`DdcRead` for one VCP feature at a time and **no raw transfer
method**; and ships `71-vorssaint-i2c.rules`, which — like the uinput rule —
widens access for nobody and only tags which buses are display channels, so the
capabilities page can offer those and not the motherboard's SMBus.

The framing is the specification's: `0x51`, `0x80|len`, payload, checksum,
where the checksum is an XOR seeded with the address byte the frame travels
under — `0x6E` outbound and **`0x50`** for the reply, the host's own address
with the read bit cleared. The seed is the classic DDC bug and an
implementation that used `0x6E` for both would pass a naive test and fail
against every real monitor, so the tests assert both.

### 4.4 Hardening

The unit (`vorssaint-helper.service`) removes everything the helper does not
need: `DevicePolicy=closed` with exactly three `DeviceAllow=` lines
(`/dev/uinput`, `char-input`, `char-i2c`), `CapabilityBoundingSet=` empty,
`NoNewPrivileges=yes`, `PrivateNetwork=yes` and `IPAddressDeny=any` (a
keylogger that cannot reach the network is a much smaller problem),
`ProtectSystem=strict`, `ProtectHome=yes`, `MemoryDenyWriteExecute=yes`, and a
`SystemCallFilter` of `@system-service` minus `@privileged @resources @mount
@reboot @swap @obsolete`. It is not `DynamicUser` because it must open
root-owned device nodes; it keeps no state of its own, so there is nothing for
a dynamic user to own.

Two directives are deliberately *not* at their strictest, and both are
load-bearing:

- **`ReadWritePaths=/sys/class/hwmon`.** `ProtectSystem=strict` makes `/sys`
  read-only, and fan control writes `pwmN`. This is the single hole in it, and
  it is the narrowest one that works.
- **`ProtectProc=default`, not `invisible`.** Naming the process that holds a
  refused `EVIOCGRAB` is done by walking `/proc/<pid>/fd`, and an invisible
  `/proc` hides exactly the other processes the message is about. The helper is
  already root, so this hides nothing from it that it could not otherwise
  obtain.

Deliberate implementation choices that belong to the threat model:

- **No shell-outs, ever.** The helper execs nothing. (`install.sh` and
  `uninstall.sh` do run `systemctl` and `udevadm`, but they are run once by
  `pkexec`, never by the daemon.)
- **A refused `EVIOCGRAB` is fatal, not a warning, and it names the holder.**
  It means another relay (`keyd`, `interception-tools`) already owns the
  device; two grabbers split the event stream in ways the user experiences as
  random dropped keys. "Device or resource busy" gives the user nothing to act
  on, so the helper walks `/proc/*/fd` for the device node and reports
  `held open by keyd (pid 812)`. That is a heuristic by construction — the
  kernel does not expose who holds the grab, only who has the node open, so
  every grabber is in the answer and a passive reader (`evtest`) can be too.
  The message says "held open by", not "grabbed by", for that reason.
- **`/dev/uinput` is opened and closed before any grab.** Grabbing every
  keyboard and *then* discovering there is no output device would leave the
  session with no working input at all. The ordering is marked in
  `device_evdev.c` as a lockout guard.
- **Hot-plug is claimed, not ignored.** A `udev_monitor` on subsystem `input`
  runs for the life of an enabled relay; a keyboard plugged in afterwards is
  grabbed on the same terms as the rest, and one unplugged is dropped without
  costing the user the devices still held. A relay that covered only the
  devices present at `Enable(true)` leaves one keyboard on which the rules
  silently do not apply, which is the bug report.
- **`SYN_DROPPED` resyncs and discards the lost window.** Replaying a partial
  window is how relays leave modifiers stuck down.
- **`SetRules` while a modifier is held emits its release first**, so a rule
  edit cannot strand Control down.
- **`Enable(false)` really releases.** The helper holds no backend object while
  disabled: `Enable(true)` creates one and claims the devices, `Enable(false)`
  closes it, which ungrabs every source and destroys the uinput device. There is
  no state in which the helper reports itself disabled while still holding an
  `EVIOCGRAB`, and no path — including the last source dying mid-stream — that
  stops the relay without going through that release. This matters more than it
  sounds: a grabbed device is invisible to the compositor, so a leaked grab is
  not untidiness, it is a keyboard the user cannot type on until the daemon is
  killed. The same invariant is why `GetDevices()` re-enumerates in listen-only
  mode when disabled rather than replaying its last answer: a cached list would
  report devices as claimed after they were released.
- **An audit line on every enable and disable.** `PLAN.md` § 4.4 asked for it:
  the journal records `relay ENABLED by uid=… pid=… session=… on backend …`
  with the device list, and the matching disable. Without it a user has no way
  to discover afterwards that the capability was on.

## 5. Install and uninstall

Installed from the app's Capabilities page, which runs `dist/install.sh`
through `pkexec` and shows one prompt. There is no `sudo`, and no `pkexec` of
an arbitrary script: the two scripts ship with the app and are the only things
it ever elevates.

```
/usr/libexec/vorssaint-helper                                    0755
/usr/share/dbus-1/system.d/org.vorssaint.Helper1.conf            0644
/usr/share/dbus-1/system-services/org.vorssaint.Helper1.service  0644
/usr/share/polkit-1/actions/org.vorssaint.helper.policy          0644
/usr/lib/systemd/system/vorssaint-helper.service                 0644
/usr/lib/udev/rules.d/70-vorssaint-uinput.rules                  0644
/usr/lib/udev/rules.d/71-vorssaint-i2c.rules                     0644
```

Seven files, and `uninstall.sh` removes exactly those seven. The scripts are
written to four properties, in order of how badly their absence would hurt:

1. **Idempotent.** Every file is compared before it is written; a second run
   prints `unchanged` for all seven and skips the reloads. A file that has been
   modified or has the wrong mode is repaired, and the repair is reported.
2. **Verified by reading back.** The exit status of `install` and `systemctl`
   is not evidence — a full filesystem, a read-only `/usr` and an SELinux
   denial all let `install` look like it worked — so each file is compared with
   its source *after* writing, the modes are checked, and on a real system the
   bus name and each polkit action are queried at the end.
3. **They say what they changed.** A privileged script that prints nothing
   leaves the user with no way to know what was done to their machine.
4. **`DESTDIR`-clean.** With `DESTDIR` set they write only under that root and
   skip every reload, which is what `tests/test_install.sh` drives and what a
   distribution package would use.

After installing, `install.sh` runs the same checks by hand:

```sh
busctl --system status org.vorssaint.Helper1      # owner uid must be 0
busctl --system introspect org.vorssaint.Helper1 /org/vorssaint/Helper1
pkaction --action-id org.vorssaint.helper.enable --verbose
systemd-analyze security vorssaint-helper.service
```

Nothing is left behind: no group membership, no lingering udev permission, no
state directory. That is the property the `input` group cannot offer, and it is
only true if the removal is complete, so `uninstall.sh` verifies afterwards
that each path is gone and that the name is no longer on the bus.

## 6. What the Flatpak build cannot do

`PLAN.md` § 4.3 already treats Flatpak as the secondary format. The input relay
is the clearest reason, and it is not a matter of asking for more permissions.

- **A Flatpak cannot install the helper.** The unit, the polkit action, the
  D-Bus system.d policy and the udev rules all live outside the sandbox, in
  `/usr/lib` and `/usr/share` on the host. A Flatpak has no mechanism to place
  a file there, by design. Flatpak has no equivalent of a system service.
- **`--device=all` is not enough even if granted.** It exposes `/dev/input` and,
  on many runtimes, `/dev/uinput` — but the *permissions* on those nodes are the
  host's (`root:input 0660`). The sandboxed process still runs as the user, so
  it still cannot open them. `--device=all` removes the sandbox's restriction,
  not the kernel's. The same is true of `/dev/i2c-*` and of `pwmN` under
  `/sys`, which is not even exposed.
- **`--system-talk-name=org.vorssaint.Helper1` works — but only if the helper
  was installed by something else.** So the honest Flatpak story is: the relay,
  fan control and DDC are available if the user has separately installed the
  helper from a distro package or the AppImage; otherwise those features are
  absent.
- **polkit inside a sandbox identifies the sandbox, not the app.** A polkit
  prompt raised for a Flatpak names the Flatpak's process; the action is still
  checked correctly, but the message the user sees is less informative.

Consequence for the feature hub: under Flatpak, without a separately installed
helper, the capability probe fails and every feature that depends on it
(mouse-button triggers, paste-at-cursor, relay-based global shortcuts, chatter
filtering, Caps Lock remapping, fan curves, external-monitor brightness) is
shown as unavailable with the reason, not silently degraded.
`FEATURE_TRIAGE.md` carries the per-feature rows.

## 7. Decisions taken, and what is left

The four questions WP-03 left open, answered:

1. **`Enable(true)` is bound to the calling session.** § 4.2. Fast user
   switching needs no special case because the binding is cleared on release.
2. **Rules are still memory-only.** Persisting them would mean the helper owns
   a root-writable file that decides how keystrokes are rewritten — a new thing
   to defend, and a new way for a compromise to survive a restart. The app
   re-sends its rules after a helper restart; that is one D-Bus call and it
   keeps the daemon stateless.
3. **`SetRules` stays `auth_admin_keep`.** Downgrading it to `auth_self_keep`
   would cut prompts, but the session binding is what stops an unrelated
   process on the same session reprogramming a relay it did not enable, and
   that binding is a check on *identity*, not on *authorisation*. Keeping the
   admin prompt means two independent things must both go wrong. It can be
   relaxed later without breaking anything; the reverse is not true.
4. **There is an audit trail.** Every `Enable` logs uid, pid, session, backend
   and the device list to the journal, and every ownership refusal logs both
   sessions. § 4.4.

Still open, for the work packages that follow:

- **Fan curves live in the app, not the helper** (WP-C6). The helper takes a
  duty cycle and a heartbeat; it has no opinion about temperature. Whether the
  10 s watchdog is the right window is a hardware question: too short and a
  busy app loses its fans, too long and a crashed one is not caught quickly
  enough. 10 s with a 3 s advised heartbeat is the starting point.
- **Which i2c buses to offer** (WP-B9). `71-vorssaint-i2c.rules` tags display
  channels by adapter name, which is a list that will need extending; the
  fallback is to offer every bus and let the user find theirs, which is exactly
  the thing § 4.3 argues against.
- **DDC has never spoken to a display.** § 8.

## 8. What was and was not proven, and where

This is the honest half of the document. The container WP-S1 was developed in
has none of the hardware or system services the helper talks to, and every
claim above falls into one of three buckets.

**Proven here, against the real thing**

| Claim | How |
|---|---|
| The shipped bus policy admits the helper's name claim and an unprivileged caller's method calls | a private `dbus-daemon` declared `<type>system</type>`, loading `dist/org.vorssaint.Helper1.conf` from an `includedir` under the stock `system.conf` default-deny policy (`scripts/private-bus.sh` § 1–4) |
| An unprivileged process cannot own `org.vorssaint.Helper1` | the same harness, § 3: `helper: cannot own org.vorssaint.Helper1: Permission denied` |
| The client library drives every method end to end as a user with no device access | § 6, 8–10, all through `vorssaint-helperctl`, which is nothing but calls on `client/vorssaint_helper_client.h` |
| The session binding refuses a second seat with `…Error.NotOwner`, and the refusal names both sessions | § 7, two unprivileged uids standing in for two seats |
| Every method is gated, including `SetContext` | § 13, the same binary with the stub denying: nine methods, nine `AccessDenied`, each naming its own action id |
| The real `polkit_authority_check_authorization_sync` is on the path in the default build | § 14: the failure comes from *inside* libpolkit, looking for `org.freedesktop.PolicyKit1` |
| The fan watchdog restores automatic control with no client involvement | § 9: ten seconds without `FanHeartbeat` and `pwm1_enable` goes `1` → `2`, with the journal line to match |
| A hwmon or i2c name that is not `hwmonN` / `i2c-N` is refused before the filesystem is touched | § 9, § 10, and `tests/test_fan.c`, `tests/test_ddc.c` |
| A `hwmonN` that is a symlink out of the root is refused, and nothing is written through it | `tests/test_fan.c` plants one; mutation-tested — disabling the boundary check reproduces the review's finding exactly (`"value":200` written into the outside directory). Also over D-Bus, `private-bus.sh` § 9 |
| A `pwmN` attribute that is a symlink is refused, and a device node where an i2c node belongs is refused | `tests/test_fan.c` and `tests/test_ddc.c`; mutation-tested — removing `O_NOFOLLOW` fails both |
| A `SetRules` document over 64 KiB is refused on length alone, and one at exactly the limit still parses | `tests/test_rules.c`, and over D-Bus in `private-bus.sh` § 6 |
| Every rule reaches the same decisions as the macOS Support type it is a port of | `tests/test_rules.c`: 147 assertions carried over from `Tests/MetricsTests.swift`, 137 of them with the Swift's own message text so the two suites can be diffed, 10 with a named platform rename. The split is produced by `Tools/linux-port/count_ported_vectors.py`, not asserted. See `docs/linux-port/RELAY_RULES.md` for the per-rule counts and for every vector that was *not* ported, with the reason |
| A malformed rule document, an unknown enumeration value, a binding on a button that belongs to another rule, and a `super_key` source another rule already owns are each refused, and the previous rules stay in force | `tests/test_rules.c` and `private-bus.sh` § 6 |
| An identifier containing a quote is refused rather than escaped, so the helper cannot be made to emit malformed JSON in its own `Rules` property | `tests/test_rules.c`, and over D-Bus in `private-bus.sh` § 6 |
| `SetContext` is ownership-checked like `SetRules`: a second seat is refused with `…Error.NotOwner` | `private-bus.sh` § 7 |
| The `Event` signal carries the documented payload, including the source device index | `private-bus.sh` § 6, `listen`: `kind=input device=0 type=1 code=58 value=1 detail=` |
| Naming the process that holds a device open | `tests/test_grabholder.c`, against the real `/proc`, with a forked child holding a file |
| The rules engine and the replay | `ctest`: `rules` (198 assertions, up from 28) and `replay`, byte for byte against `tests/replay_expected.txt` — 114 lines of relay output over a recorded evdev stream with all seven rules on |
| The relay's own latency with every rule enabled | p50 48 ns, p99 72–75 ns over three runs; 277 ns / 379 ns including the `write(2)` that `libevdev_uinput_write_event` makes. Pass-through with every rule off is 37 ns / 53 ns, so the whole rule set costs about 11 ns at p50. `RELAY_RULES.md` § latency |
| The install and uninstall scripts are idempotent, repair tampering, and remove all seven files | `tests/test_install.sh` in a `DESTDIR` fake root |
| The unit file is accepted by systemd's own parser | `systemd-analyze verify` with a real `ExecStart`: no diagnostics |
| Both udev rule files parse | `udevadm verify`: `Success: 1, Fail: 0` each |
| The polkit policy is valid against its DTD | `xmllint --noout --valid`: clean |
| The build is warning-clean under `-Werror` at every optimisation level | `scripts/build-matrix.sh`: no build type, Debug, Release and RelWithDebInfo each configure, build without a warning, and pass all 8 ctest entries. The levels are not interchangeable -- `-Wformat-truncation`, `-Wmaybe-uninitialized` and `-Wrestrict` reason differently as GCC inlines more, and checking only one is how a truncation reached the lead's re-check |
| There is no leak and no undefined behaviour in the suites or in the relay CLI | `scripts/build-matrix.sh`'s fifth leg: ASan + UBSan + LSan (`-DWITH_SANITIZERS=ON`, `-fno-sanitize-recover=all`), all 8 ctest entries plus `vorssaint-relay --bench` and `--tap` run directly. `-Werror` cannot see this class of defect -- a leak is a missing statement, not a wrong one -- which is how `vorssaint-relay` leaking its whole device backend on every `--replay` and `--bench` run survived four warning-clean build types until the WP-D1/D2 review |
| The binaries carry the hardening the build claims | `readelf`: PIE, `BIND_NOW`, `GNU_RELRO`, non-executable stack, `_FORTIFY_SOURCE` `_chk` symbols present |

**Proven only against a fake, because the real thing does not exist here**

| Claim | The fake | What that leaves open |
|---|---|---|
| evdev grab and uinput re-emit | `device_fake.c` | the whole kernel path. `CONFIG_INPUT_UINPUT` is unset in this kernel and `CONFIG_MODULES` is unset too, so no module can supply it; `/dev/input` does not exist and `/sys/class/input` is empty. The evdev backend has never executed. |
| The timer source: a quit-protection hold resolving and a smooth-scroll frame going out while the user does nothing | the fake backend's deadline handling, driven by a synthetic clock (`tests/test_rules.c`, and every `TIM>` line in `tests/replay_expected.txt`) | that the `timerfd` armed with `TFD_TIMER_ABSTIME` inside `ev_read`'s `poll()` set wakes when it should on a loaded machine. The `timerfd` code has never executed: it lives in `device_evdev.c`, which needs `/dev/input` |
| `EV_LED` driving the lamp on a grabbed keyboard | the fake records the write; `device_evdev.c` routes `EV_LED` to every keyboard source that advertises the code, over an `O_RDWR` fd | that a grabbed evdev device accepts an `EV_LED` write from the process holding the grab, and that the compositor's own LED state does not immediately overwrite it. This is the one `super_key` claim with no model here at all |
| Hot-plug claims a keyboard plugged in after `Enable(true)` | an injected add/remove event through the same `DEV_READ_HOTPLUG` path the `udev_monitor` feeds | that `udev_monitor_receive_device` returns what is expected on real hardware, and that a real device's `ID_INPUT_*` properties are set when the add event arrives |
| A refused `EVIOCGRAB` names the holder | a forked process holding an ordinary file | that `libevdev_grab` returns `-EBUSY` rather than something else when `keyd` holds the device |
| hwmon `pwm` writes, read-back and restore | a temporary tree of plain files, plus a read-only tmpfs for a driver that refuses the store | drivers that clamp or round; chips with no `pwmN_enable`; laptop ECs that take the write and ignore it at the firmware level. `/sys/class/hwmon` **does not exist on this machine** — checked: `ls: cannot access '/sys/class/hwmon': No such file or directory` — so `fan_enumerate_json` on the real root returns `[]`, and that empty list is in the test output. |
| The read-back catching a store that succeeded and did not stick | *nothing* | this branch of `write_verified` has **no model here at all**, and that is a deliberate trade made during the WP-S1 review. It was previously exercised by pointing the attribute at `/dev/zero`; the symlink guard added in the same review refuses that, correctly, because a sysfs attribute is a regular file. The guard closes a hole that was demonstrated live; the branch it costs is the less important half of a check whose other half (the write failing outright) is still covered. The read-back itself runs on every successful write and is asserted. It needs a real driver that ignores a store — a laptop EC, in WP-C6. |
| DDC/CI framing and transactions | `ddc_transport_fake()`, a monitor model that validates address, length byte and checksum on every frame it is handed | **everything about real displays.** There is no `/dev/i2c-*` and no `/sys/bus/i2c` here. Whether a given monitor answers, tolerates the 40/50 ms spacing, or reports a sane maximum is unknown. `GetCapabilities` reports `"hardware_tested": false` for exactly this reason, and the hub must not claim otherwise until WP-B9 has run it on a display. |
| The logind session lookup | `session_lookup_fake()`, keyed by pid in the unit test and by uid in the bus harness | that `sd_pid_get_session` returns what is expected for a desktop session, and what it returns across a fast user switch. The real lookup is compiled in, is the daemon's default, and is *called* in `tests/test_session.c` — where it returns `-ENXIO`, the honest answer on a machine with no logind. |

**Argued but not run at all**

- **polkit granting.** That `auth_admin_keep` grants after an administrator
  authenticates, and that the prompt says what the `<message>` elements say, has
  not been seen. No polkit authority runs here: `pkaction` fails with
  `Error getting authority`. What *is* proven is that the real check is on the
  path and that a denial stops every method.
- **systemd starting the unit through `Type=dbus` activation**, and
  `systemd-analyze security` scoring it. There is no systemd as pid 1 here
  (`System has not been booted with systemd as init system`), so the unit has
  been parsed but never started, and the security score has never been taken.
- **`ProtectSystem=strict` with `ReadWritePaths=/sys/class/hwmon`** actually
  permitting the `pwmN` writes. It is the right directive on paper; it has not
  been observed letting a write through, because neither systemd nor hwmon is
  here.
- **Everything on real hardware**: the grab taking on GNOME/Plasma/Sway, the
  virtual device appearing to libinput with the right capabilities, `evtest`
  succeeding on a device immediately after `Enable(false)`, and end-to-end
  latency measured with a second uinput device as a known-time stimulus.
- **That the rules feel right**, which is not the same as reaching the same
  decisions. The vectors prove the C agrees with the Swift about every
  threshold and every edge; they cannot prove that 220 px per workspace or a
  65 response is the right number on a machine whose pointer acceleration,
  wheel detent and screen width are all different from a Mac's. Those four
  gesture constants and the two smooth-scroll ones are the part of this that
  needs a real hand, and they are named together in `rules.h` so tuning them
  is one edit, not a hunt.
- **The `Event` rate limiter under load.** It is exercised only through the
  daemon, where the fake source produces four events per burst — nowhere near
  the 400/s cap. That the cap holds, and that the dropped count is what a
  client sees when it is hit, needs a source that can flood: a real 1 kHz
  mouse, or a kernel that has uinput.

The first three of those need a machine with systemd and polkit running; the
last needs hardware. None of them can be closed in this container, and the
capabilities page must not claim any of them until they are.
