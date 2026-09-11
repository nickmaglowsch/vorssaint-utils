# Privilege model: `vorssaint-helper`

**Status: draft.** Written during WP-03 (`spikes/03-input-relay.md`) and scoped
to the input relay, which is the most dangerous thing the helper will ever do.
WP-S1 extends it to hwmon `pwm` writes and DDC/CI I2C, reviews it, and removes
this notice. Nothing here is installed yet.

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
| Granularity | all input devices, read and write | the three device classes the relay needs |
| Revocation | log out, log in again after `gpasswd -d` | `Enable(false)` releases every grab immediately; `systemctl stop` ends the process |
| Visible to the user | no: a group membership nobody reads | yes: a polkit prompt naming the action |
| Survives uninstalling the app | yes | no: the unit and rules go with the package |
| Blast radius of a bug in *our* code | same as any other bug | confined to a root process with a 3-method API |

The decisive line is the first. Adding a user to `input` means a malicious
npm postinstall script, a browser extension's helper process, or any other
program that user runs — none of which we wrote or reviewed — can silently read
every keystroke on the machine. That is a permanent, invisible, system-wide
downgrade of the user's security, handed out so that one utility can remap Caps
Lock. The helper keeps the capability inside a process whose entire source is
the ~1600 lines under `spikes/wp03-input-relay/src` that the helper links
(`helper.c`, `device_evdev.c`, `device_fake.c`, `rules.c`, `polkit_check.c`).

The udev rule the port *does* ship (`70-vorssaint-uinput.rules`) deliberately
widens access for nobody. It only makes sure the helper is started when uinput
appears, and tags the relay's own output device so the relay does not grab it.

## 4. The design

```
  ┌─────────────────────────┐         ┌──────────────────────────────────┐
  │ Vorssaint (the app)     │         │ vorssaint-helper (root)          │
  │ uid = the desktop user  │         │ systemd system unit, Type=dbus   │
  │ no device access at all │         │                                  │
  │ not in group input      │         │  /dev/input/event*  (read, GRAB) │
  │                         │         │  /dev/uinput        (write)      │
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
| `GetDevices()` | `org.vorssaint.helper.get-devices` | `yes` | only names, already readable in `/proc/bus/input/devices`; kept promptless so the capabilities page can show what *would* be grabbed without handing out the power to grab |

`auth_admin_keep`, not `auth_admin`: one administrator prompt per session, not
one per rule edit. `allow_inactive` is `no` on both, so a user on a switched-away
session cannot enable the relay on the active one.

### 4.1 The API

Three methods and one signal. The narrowness is the point: the app never
receives a file descriptor for an input device, and the helper exposes no way
to ask "what was typed" outside tap mode.

```
interface org.vorssaint.Helper1 {
  methods:
    SetRules(in  s json)       # the rule document; see the schema below
    GetDevices(out s json)     # [{node,name,kind,grabbed}, …]
    Enable(in  b enable)       # claim/release devices, start/stop the relay
  signals:
    Event(t timestamp_ns, q type, q code, i value)
  properties (read-only, no prompt):
    Rules          s   active configuration + counters
    Backend        s   "evdev" or "fake"
    Authorization  s   which authorisation backend is compiled in
}
```

`Event` is emitted **only in tap mode**, which is the shortcut recorder: the
relay listens without grabbing and without an output device, the app shows the
user what they pressed, and the mode ends when recording ends. Outside tap mode
the helper emits nothing, so the ordinary state of an enabled relay is that no
keystroke ever crosses the bus. That is deliberate: a helper that streamed
every event to a session process would have moved the keylogger into the
unprivileged half and gained nothing.

The rule document is a fixed seven-key schema, parsed by a hand-written reader
in `rules.c` rather than a JSON library, because it is the one untrusted input
a root process accepts:

```json
{"tap_hold": true, "tap_threshold_ms": 200,
 "chatter": true, "chatter_ms": 40,
 "tap_source": 58, "tap_output": 1, "hold_output": 29}
```

Every value is range-checked (`tap_threshold_ms` ≤ 5000, keycodes ≤ `KEY_MAX`);
anything unparseable is rejected with `org.freedesktop.DBus.Error.InvalidArgs`
and the previous rules stay in force.

### 4.2 Hardening

The unit (`vorssaint-helper.service`) removes everything the helper does not
need: `DevicePolicy=closed` with exactly two `DeviceAllow=` lines,
`CapabilityBoundingSet=` empty, `PrivateNetwork=yes` and `IPAddressDeny=any`
(a keylogger that cannot reach the network is a much smaller problem),
`ProtectSystem=strict`, `ProtectHome=yes`, `MemoryDenyWriteExecute=yes`, and a
`SystemCallFilter` of `@system-service` minus `@privileged @resources @mount
@reboot @swap @obsolete`. It is not `DynamicUser` because it must open
root-owned device nodes; it keeps no state of its own, so there is nothing for
a dynamic user to own.

Deliberate implementation choices that belong to the threat model:

- **No shell-outs, ever.** The helper execs nothing.
- **A refused `EVIOCGRAB` is fatal, not a warning.** It means another relay
  (`keyd`, `interception-tools`) already owns the device; two grabbers split
  the event stream in ways the user experiences as random dropped keys.
- **`/dev/uinput` is opened and closed before any grab.** Grabbing every
  keyboard and *then* discovering there is no output device would leave the
  session with no working input at all.
- **`SYN_DROPPED` resyncs and discards the lost window.** Replaying a partial
  window is how relays leave modifiers stuck down.
- **`SetRules` while a modifier is held emits its release first**, so a rule
  edit cannot strand Control down.
- **`Enable(false)` really releases.** The helper holds no backend object while
  disabled: `Enable(true)` creates one and claims the devices, `Enable(false)`
  closes it, which ungrabs every source and destroys the uinput device. There is
  no state in which the helper reports itself disabled while still holding an
  `EVIOCGRAB`, and no path — including a source dying mid-stream — that stops
  the relay without going through that release. This matters more than it
  sounds: a grabbed device is invisible to the compositor, so a leaked grab is
  not untidiness, it is a keyboard the user cannot type on until the daemon is
  killed. The same invariant is why `GetDevices()` re-enumerates in listen-only
  mode when disabled rather than replaying its last answer: a cached list would
  report devices as claimed after they were released.

## 5. Install and uninstall

Installed from the app's Capabilities page, which runs these steps and shows
one polkit prompt. There is no `sudo`, and no `pkexec` of a script.

```sh
# install
install -Dm0755 vorssaint-helper              /usr/libexec/vorssaint-helper
install -Dm0644 org.vorssaint.Helper1.conf    /usr/share/dbus-1/system.d/org.vorssaint.Helper1.conf
install -Dm0644 org.vorssaint.Helper1.service /usr/share/dbus-1/system-services/org.vorssaint.Helper1.service
install -Dm0644 org.vorssaint.helper.policy   /usr/share/polkit-1/actions/org.vorssaint.helper.policy
install -Dm0644 vorssaint-helper.service      /usr/lib/systemd/system/vorssaint-helper.service
install -Dm0644 70-vorssaint-uinput.rules     /usr/lib/udev/rules.d/70-vorssaint-uinput.rules

udevadm control --reload-rules && udevadm trigger --subsystem-match=misc --action=add
systemctl daemon-reload
systemctl reload dbus.service          # picks up system.d/ without dropping connections
systemctl enable --now vorssaint-helper.service
```

```sh
# uninstall: the capability goes away with the files
systemctl disable --now vorssaint-helper.service
rm -f /usr/libexec/vorssaint-helper \
      /usr/share/dbus-1/system.d/org.vorssaint.Helper1.conf \
      /usr/share/dbus-1/system-services/org.vorssaint.Helper1.service \
      /usr/share/polkit-1/actions/org.vorssaint.helper.policy \
      /usr/lib/systemd/system/vorssaint-helper.service \
      /usr/lib/udev/rules.d/70-vorssaint-uinput.rules
systemctl daemon-reload
systemctl reload dbus.service
udevadm control --reload-rules
```

Nothing is left behind: no group membership, no lingering udev permission, no
state directory. That is the property the `input` group cannot offer.

Verifying it worked (read back, do not trust the exit status):

```sh
busctl --system status org.vorssaint.Helper1      # owner uid must be 0
busctl --system introspect org.vorssaint.Helper1 /org/vorssaint/Helper1
pkaction --action-id org.vorssaint.helper.enable --verbose
systemd-analyze security vorssaint-helper.service
```

## 6. What the Flatpak build cannot do

`PLAN.md` § 4.3 already treats Flatpak as the secondary format. The input relay
is the clearest reason, and it is not a matter of asking for more permissions.

- **A Flatpak cannot install the helper.** The unit, the polkit action, the
  D-Bus system.d policy and the udev rule all live outside the sandbox, in
  `/usr/lib` and `/usr/share` on the host. A Flatpak has no mechanism to place
  a file there, by design. Flatpak has no equivalent of a system service.
- **`--device=all` is not enough even if granted.** It exposes `/dev/input` and,
  on many runtimes, `/dev/uinput` — but the *permissions* on those nodes are the
  host's (`root:input 0660`). The sandboxed process still runs as the user, so
  it still cannot open them. `--device=all` removes the sandbox's restriction,
  not the kernel's.
- **`--system-talk-name=org.vorssaint.Helper1` works — but only if the helper
  was installed by something else.** So the honest Flatpak story is: the relay
  is available if the user has separately installed the helper from a distro
  package or the AppImage; otherwise the feature is absent.
- **polkit inside a sandbox identifies the sandbox, not the app.** A polkit
  prompt raised for a Flatpak names the Flatpak's process; the action is still
  checked correctly, but the message the user sees is less informative.

Consequence for the feature hub: under Flatpak, without a separately installed
helper, the capability probe fails and every feature that depends on the relay
(mouse-button triggers, paste-at-cursor, relay-based global shortcuts, chatter
filtering, Caps Lock remapping) is shown as unavailable with the reason, not
silently degraded. `FEATURE_TRIAGE.md` carries the per-feature rows.

## 7. Open questions for WP-S1

1. **Should `Enable(true)` be bound to the calling session?** Today a second
   caller can `Enable(false)` a relay the first turned on. Binding the enable to
   the logind session that requested it (`sd_bus_creds_get_session`) is the fix;
   it needs a decision about what happens on fast user switching.
2. **Rule persistence.** The helper currently keeps rules in memory only, so a
   restart drops them and the app must re-send. Persisting them means the helper
   owns a root-writable file that decides how keystrokes are rewritten — a new
   thing to defend. The memory-only default is the safer starting point.
3. **Should `SetRules` really be `auth_admin_keep`?** Once `Enable` has been
   authorised for the session, a rule edit adds no capability the caller does
   not already have. Downgrading it to `auth_self_keep` would cut prompts; it
   also lets a second, unrelated process on the same session reprogram a relay
   it did not enable. Left at `auth_admin_keep` pending review.
4. **Audit trail.** Nothing currently records that the relay was enabled. A
   line to the journal on every `Enable(true)`/`(false)` with the caller's uid
   and pid is cheap and is the only way a user can later discover the capability
   was on.
