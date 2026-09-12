# The relay rule set

Every input feature Vorssaint has on macOS is a tap that watches a `CGEvent`
stream and decides. On Linux there is no such tap: the relay grabs the evdev
device with `EVIOCGRAB`, runs the same decisions, and re-emits through one
uinput device. This file says, rule by rule, which macOS file is the
specification, what the C does differently and why, and how many of the Swift
test vectors were carried over.

The rules engine is `linux/helper/src/rules*.c`. It performs no syscalls and
touches no device, which is what lets the same code run behind the real
evdev/uinput backend and behind the in-process fake — and what lets the
vectors below run at all on a kernel with no `/dev/input` (see
`PRIVILEGES.md` § 8).

## The map

| Rule | macOS specification | Mac service for the semantics | Vectors ported |
|---|---|---|---|
| `keyboard_debounce` | `Services/KeyboardDebounce/KeyboardDebounceSupport.swift` (`KeyboardDebounceState`, `KeyboardDebounceConfig`) | `KeyboardDebounceService.swift` | 29 verbatim + 3 new |
| `mouse_click_debounce` | `Services/MouseClickDebounce/MouseClickDebounceSupport.swift` (`MouseClickDebounceState`) | `MouseClickDebounceService.swift` | 14 verbatim |
| `scroll_invert` | `VorssaintCore/Services/ScrollWheelSupport.swift` (`inversionPlan`) | `ScrollInverterService.swift` | 6 verbatim |
| `smooth_scroll` | `VorssaintCore/Services/SmoothScrollSupport.swift` (all of it) | `SmoothScrollService.swift` | 48 verbatim |
| `super_key` | `Services/SuperKey/SuperKeySupport.swift` (`State`, `soloEffect`), `SuperKeyMappingGuard.swift` | `SuperKeyService.swift` | 14 verbatim + 1 adapted + 5 new |
| `mouse_button_shortcut` | `Services/MouseButtons/MouseButtonShortcutSupport.swift`, `MouseSpacesGestureSupport.swift` | `MouseButtonShortcutService.swift` | 12 verbatim + 5 adapted |
| `quit_protection` | `VorssaintCore/Core/QuitProtectionSupport.swift` | `Services/QuitProtection/QuitProtectionService.swift` | 13 verbatim + 5 adapted + 1 new |

"Verbatim" means the assertion's text in `linux/helper/tests/test_rules.c` is
character-for-character the message from `Tests/MetricsTests.swift`, so the two
suites can be diffed. "Adapted" means the same assertion with a macOS name
replaced by its Linux equivalent (Command → Control, Mission Control →
overview, Space → workspace). "New" means a case the Swift does not have,
because the Linux surface has a hazard the macOS one does not.

Totals, from `test_rules`:

```
136 verbatim + 11 adapted = 147 assertions carried over from MetricsTests.swift
 51 Linux-specific assertions (the relay path, the documents, device lifecycle)
198 assertions, 0 failed -- all rules tests passed
```

Add the replay comparison (`tests/replay_expected.txt`, 114 lines of relay
output against a recorded evdev stream with all seven rules on) and the five
other C suites, and `ctest` is eight suites, all green in all four build types.

---

## 1. `keyboard_debounce`

Per-keycode chatter filter. Replaces the WP-03 spike's flat 40 ms window with
the macOS decisions.

The state machine is `KeyboardDebounceState.shouldSuppress` line for line: a
per-key record of the last accepted press, the last accepted release and the
last event of any kind; a single `lastAcceptedKeyCode` shared across keys; the
five-second stale gap; and the reset when a timestamp moves backwards. The
per-key window overrides come over the wire in the same `"code:ms,code:ms"`
storage string the macOS settings page writes, so a settings export moves
between the two builds unchanged (`decodeKeyWindows` / `encodeKeyWindows`
ported, including the sanitiser that replaces an out-of-range value with the
5 ms default rather than clamping it).

**One behaviour is worth stating out loud**, because it differs from the spike
this rule replaces: *a key release is never suppressed*. The spike dropped the
release that closed a suppressed press, so the downstream key state stayed
balanced. The macOS filter does not, on the grounds that stopping the tap must
never be able to leave a key held down. That is the behaviour ported. On evdev
it costs nothing: `input_handle_event()` in the kernel drops a release for a
key that is not currently pressed, so the extra release the relay emits is
swallowed one layer below the compositor. The replay fixture shows it — three
`KEY_E` releases go out for one accepted press.

New assertions: the five-second stale gap (2), and that the window sanitiser
refuses a value outside 0..500 (1). The Swift exercises the sanitiser only
indirectly, through `decodeKeyWindows`.

## 2. `mouse_click_debounce`

`BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE` only — the extra buttons keep their
navigation, shortcut and gesture ownership, exactly as on macOS. All fourteen
`MouseClickDebounceState` vectors port unchanged, including the ones that
matter most: a healthy click is never delayed, a bounce loses its Down *and*
its matching Up so nothing is left held, and turning the filter off still
delivers the final release of a click it had already suppressed.

**`MouseClickDebounceEvent.dragged` has no evdev counterpart.** On macOS a
`leftMouseDragged` event *is* the pointer motion, so suppressing it both stops
the drag and stops the cursor. On Linux motion is `EV_REL REL_X`/`REL_Y`, a
separate stream that no button owns; dropping it would freeze the cursor,
which is a worse bug than the one being fixed. So the decision function
implements all three kinds — that is what lets the three drag vectors port
unchanged — and the relay never produces the third. `rules_mcd_apply()` feeds
Down and Up and nothing else.

## 3. `scroll_invert`

`REL_WHEEL`, `REL_HWHEEL`, `REL_WHEEL_HI_RES`, `REL_HWHEEL_HI_RES`, per axis
and per device class. The class comes from udev (`ID_INPUT_TOUCHPAD` vs
`ID_INPUT_MOUSE`), reduced to one `rules_dev_class` by the device layer and
carried on every event; the fake backend has the same field, which is how the
per-class behaviour is tested. A device udev could not classify is treated as
a mouse: on a machine with no such property at all, refusing to act would make
the feature look broken, and inverting a wheel is reversible.

`inversionPlan` ports whole, Shift branch included, but **the Shift redirect
is off by default**. On macOS the window server turns a Shift-held vertical
tick sideways *above* the event tap, so once the tap swallows the tick it has
to perform the redirect itself, and the vertical tick therefore has to follow
the *horizontal* setting. On Linux that redirect is done by the application,
below the relay; performing it here as well would scroll sideways twice. The
flag exists (`shift_redirects_vertical`) for a desktop that ever does it in
the input stack, and defaults to false.

**`ScrollWheelSupport.isMouseWheel` is not ported** (7 Swift vectors). It
exists to tell a trackpad from a wheel by inference — continuous flag, gesture
phases, scroll count, a one-second grace window — because `CGEvent` does not
say which device an event came from. evdev does: the relay reads the class off
the source device it grabbed. There is nothing to infer, so there is nothing
to port.

## 4. `smooth_scroll`

The whole of `SmoothScrollSupport`, ported function for function and proven by
all 48 of its vectors: `frameDelta`'s exponential decay, the `1/20 s` frame
clamp that stops a stalled loop dumping the tail in one jump, the
`minimumGlideSpeed` floor, the `finishThreshold` flush, `wholePixels`'
fraction carry, `finalPixels`' landing round, `carry`'s direction reset,
`Engine.add`'s reversal rule, `continuousDistance`, and the two sanitisers.
The composition property is pinned by the same test the Swift uses: twelve
frames at 60 Hz and twenty-four at 120 Hz leave the same remainder to within
10⁻⁶.

Two deliberate differences, and only two:

- **The budget is counted in `REL_*_HI_RES` units, not pixels.** The kernel
  fixes one wheel notch at 120 units. The macOS step is "pixels per tick" with
  a default of 40, so the default step has to travel exactly 120 units or the
  relay would silently change how far one notch scrolls. The step then scales
  that, which is what makes the speed setting work — the same relationship the
  pixel step has on macOS.
- **The low-resolution axis is reported alongside.** A device that emits
  `REL_WHEEL_HI_RES` is expected to emit `REL_WHEEL` too, once per accumulated
  120 units, or an application that reads only the coarse axis sees nothing.
  The relay does that, and the replay fixture shows the arithmetic closing
  exactly: two notches in, −240 hi-res units and −2 notches out.

**The timer.** Smooth scrolling is the first rule that has to emit while the
user is doing nothing, so the relay needed a clock. `rules_deadline_ns()` now
answers for the whole engine (the earliest of the smooth-scroll frame and the
quit-protection hold), and `device_evdev.c` arms a `timerfd` on it inside the
same `poll()` set as the device fds. It replaces the previous millisecond
`poll()` timeout, which rounded up, was computed against a clock read that was
already stale, and was coarse enough to visibly stutter a 60 Hz glide. The
fake backend's deadline handling is the same source driven by a synthetic
clock — the timed rules are tested against it with no kernel involved at all.

## 5. `super_key`

Tap the chosen key for its solo action; hold it and every key pressed while it
is down carries a configured modifier combination. `SuperKeySupport.State`
ports exactly, including the three subtleties the macOS suite pins: a repeat
arriving with nothing held changes no state, a pre-held modifier cancels the
solo action while the key stays held, and a lost release is recoverable by
`reset()` without touching the key again.

`soloEffect` ports with one rename. macOS's `inputSource` action becomes
`RULES_SK_KEY`, "emit a configured key": choosing a keyboard layout is a
desktop setting (`org.gnome.desktop.input-sources`, kded, xkb group), and a
root daemon has no business calling any of them. The relay emits the key and
raises an `Event` notice; the app binds it. The tie-break is unchanged — a
long hold still reserves Caps Lock.

**LED handling.** `EV_LED LED_CAPSL` is written back to the *source* devices,
not to the relay's uinput output: the lamp the user looks at is on the
keyboard the relay grabbed, and a grabbed device still accepts `EV_LED`
writes. `device_evdev.c` routes an `EV_LED` write to every keyboard source
that advertises the code; the source fds are opened `O_RDWR` for exactly this,
falling back to `O_RDONLY` (no lamps, everything else works). The fake
backend records it, which is how the test sees it.

**What `SuperKeyMappingGuard` becomes.** On macOS the feature rewrites the
keyboard's HID mapping table with `hidutil`, because Caps Lock is a *lock* —
it flips on one press and off on the next, and nothing above the driver ever
sees it go down and up, so it cannot be held. That table is system state which
outlives the process, hence the whole guard: a shell waiting on a pipe, a
readback confirmation, a write-ahead marker, a cleanup mode that runs before
`NSApplication` exists.

That hazard does not exist here, and saying so is the point. The relay *is*
the remap: the source is held with `EVIOCGRAB`, which the kernel drops when
the fd closes, so the mapping cannot outlive the process — including a
`SIGKILL`. Nothing to clean up, nothing to confirm, no guard process.

Its other two rules do carry over and are implemented:

- `hasMappingConflict` → `rules_sk_source_conflict()`. A source key another
  enabled rule already answers (a quit-protection key or its Control, a key or
  modifier a mouse chord emits, one of this rule's own output modifiers) is
  refused by `SetRules` with `InvalidArgs`, and the previous rules stay in
  force — rather than silently claiming a key that will then do two things.
- `mappingMarkerAfterClear` → `rules_release_held()`. Nothing is marked
  released until its release event has actually been queued, so a full output
  buffer leaves the state eligible for the next attempt instead of recording a
  clear that did not happen.

Not ported: `mappings(enablingSuperKey:)`, `mappingArgument`, `parseMappings`,
`mappingTables`, `mappingReportConfirms`, `consistentMappings`,
`mappingRequestIsAuthorized`, `heldKeyWatchdogOutcome` and the `SuperKeySource`
HID usage table — about twenty assertions, all of them `hidutil` plumbing for
a mechanism Linux does not have. `modifiers(from:)`/`storageValue(for:)` and
`nextInputSourceID` are not ported either: the first is the app's shortcut
storage format and the second is the app's layout cycling.

## 6. `mouse_button_shortcut`

`BTN_SIDE`, `BTN_EXTRA`, `BTN_FORWARD`, `BTN_BACK`, `BTN_TASK` and the
horizontal tilt map to key chords. `canMap` ports to the evdev button block —
left, right and middle stay excluded, because they belong to the click filter,
which is the same split CoreGraphics numbering produced with `buttonRange =
3...31`. The two negative tilt inputs keep their negative values for the same
reason they have them on macOS: they cannot collide with a real button number.

`SideWheelGestureGate` ports whole (250 ms quiet gap, one firing per direction
per burst, reset). The sign rule survives the platform change unchanged: AppKit
calls a positive horizontal delta movement to the left, and so does evdev's
`REL_HWHEEL`.

`MouseSpacesGestureSupport.Tracker` ports whole — 220 px per workspace, 150 px
for the overview, the 0.35 s cooldown, the one-step travel bank, the axis
commitment that stops a diagonal drag doing two things, the toggle rule for the
overview, and the "furthest past its own threshold wins" tie-break. evdev's
`REL_Y` is positive downward, which is the same top-left origin the Swift
tracker measures in, so the vertical directions carry over with no sign
change. Positions are accumulated pointer deltas from the press.

Two Linux-shaped differences:

- **The helper does not switch workspaces.** Which workspace is next has four
  different answers (KWin D-Bus, our GNOME extension, Hyprland/Sway IPC, X11
  `_NET_CURRENT_DESKTOP` — see WP-C1), none of which a root daemon should be
  talking to. The gesture raises an `Event` signal with
  `{"rule":"mouse_button_shortcut","action":"workspace_left",…}` and the app
  acts. `resolved(followsDrag:)` is applied before the notice, so the app is
  told what the hand meant, not what the pointer did.
- **Pointer motion during the drag is not swallowed.** On macOS the dragged
  event is the motion; here they are separate, and the cursor has to keep
  moving. What is withheld is the *button press*: it is held back on the way
  down and replayed as a plain click on release if the gesture never fired,
  which is the `didFire` rule the Swift service uses.

Not ported: `decode`/`encode`/`sortedButtons` (the app's storage),
`firesShortcut`/`claimsButton`/`spacesGestureButton` (UserDefaults reads and
the radial menu's claim, all app-side), `buttonName` (UI strings), and
`isPressed` (recovery after macOS disabled an event tap — a grab has no such
state to recover).

## 7. `quit_protection`

Ctrl+Q and Ctrl+W, in the three macOS modes. `sanitizedHoldDuration`,
`sanitizedDoublePressInterval`, `isWithinDoublePressInterval` and `scopeAllows`
port verbatim; `isBaseShortcut` and `isExtraShortcut` port with Command read as
Control, which is what the same shortcut is called on every Linux desktop.
Extra-modifier mode still deliberately claims the *bare* shortcut, or
protection would be one keystroke away from being bypassed.

In hold mode the press is withheld and the timer decides: released early, it
never reaches the application at all and a `"blocked"` notice goes out;
held past the deadline, the timer emits the real chord and a `"confirmed"`
notice. In double-press mode the first press arms and the second within the
interval fires. In extra-modifier mode the confirming combination is emitted
with the extra modifier momentarily released, so the application sees the plain
shortcut and the user's physical key state and the stream agree again
afterwards.

**`usesNativeQuitRequest` is not ported.** On macOS, ⌘Q protection asks the
target application to terminate itself. That is not an input-device operation
and the helper has no business doing it as root. The decision goes out in the
notice as `"native_quit": true` and the app decides.

**The per-app scope needs `focused_app_id`, which is what `SetContext` is
for.** `matchesKey`'s layout resolution is not ported either: it exists because
macOS resolves ⌘Q through the layout's Command table, and "the key ⌘Q quits
from" types `й` on Russian. The relay works in keycodes, below any layout;
resolving a layout is the app's job through xkb, and the app configures the
rule with the keycode it wants.

---

## Why `SetContext` is its own method

The choice was between a new method and a field of the rules document. It is a
new method, for one reason that is not about tidiness.

The focused application changes every time the user moves between windows —
many times a minute, sometimes several times a second. The rules change when a
person opens a settings page. Riding the rules document would mean
re-validating the whole schema and calling `rules_reconfigure()` on every focus
change, and `rules_reconfigure()` deliberately releases everything the engine
is holding down: a super key held across an alt-tab would drop its modifiers
mid-chord, every single time. `SetContext` writes one field and touches no rule
state.

It carries the same authorisation as `SetRules` — the same class of polkit
action and the same session-ownership check — because it is the same privilege:
telling the relay how to treat keys it has already claimed. It gets its *own*
polkit action id (`org.vorssaint.helper.set-context`) so an administrator can
allow the frequent, low-consequence one without allowing the other, and so the
audit line for a rule change is not drowned in focus changes.

## Latency, with every rule enabled

`vorssaint-relay --bench 60000 --rules-file tests/bench_rules.json` on the
fake backend, measuring the relay's own cost: the interval from `read()`
returning an event to the matching `write()`+`SYN` having been handed on.
Kernel-side evdev and uinput delivery are outside it and are the same for any
relay of this design. The synthetic stream is press/release pairs 12 ms apart
with a wheel notch every sixteenth event, so the scroll rules are on the
measured path too.

| Configuration | p50 | p99 |
|---|---|---|
| all seven rules enabled | **48 ns** | **72–75 ns** (three runs: 75, 73, 72) |
| every rule off (pass-through) | 37 ns | 53 ns |
| all seven rules + the `write(2)` the uinput path performs (`--write-to /dev/null`) | 277 ns | 379 ns |

The whole rule set costs about 11 ns at p50 over a bare pass-through, and the
syscall the real backend has to make is five times the cost of all seven rules
put together. The budget this has to fit in is one 1000 Hz mouse report,
1 ms — three orders of magnitude of headroom.

The p99.9 and max figures (≈2.7 µs and ≈139 µs) are container scheduler noise,
not rule work; they move by an order of magnitude between runs on an
unloaded and a loaded machine, and the p50/p99 do not.

**This is the fake backend.** The kernel this was developed on has no
`/dev/input` and no `CONFIG_INPUT_UINPUT`, so the real path has never run
(`PRIVILEGES.md` § 8). What the number measures is the rules engine and the
relay loop, which are the same code behind either backend; what it does not
measure is evdev read wakeup and uinput delivery, which must be measured on
real hardware before the latency budget is called proven.
