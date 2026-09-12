# The `Platform` protocol layer (WP-12)

Deliverable of WP-12. What each protocol is, what its capability flags mean,
which file implements it on macOS, which directory under `linux/platform` will
implement it, and which features consume it.

Everything here lives in `Sources/VorssaintCore/Platform/`, one file per
concern. Nothing in it imports AppKit, CoreGraphics beyond the geometry family,
IOKit or a D-Bus library: these are the questions the core asks, never the
answers.

## 0. Where this sits

`VorssaintCombine` is gone as of this work package — the target, its product
and its sources — on WP-13's recommendation in `COMBINE.md` § 7, which the
lead accepted. Nothing could import it: `build.sh` compiles
`Sources/Vorssaint`, `Sources/VorssaintCore` and `Sources/VorssaintMac` in one
`swiftc` invocation where that module does not exist. The OpenCombine products
stay on `VorssaintCore`, Linux-conditional, because they are what make the
per-file `#if canImport(Darwin)` guard of `COMBINE.md` § 1 resolve. The Linux
CI step that used to build it now builds `VorssaintCoreTestSupport` instead.

## 1. The three rules

**1. Capability first, call second.** Every protocol carries a capability
value, and a feature reads it before it acts. This is `PLAN.md` § 4.5 —
desktop support is *declared, not assumed* — expressed in code: the same Linux
binary runs on GNOME (no window-management protocol), KDE (KWin scripting),
Sway (wlroots foreign-toplevel) and X11 (EWMH), and under Flatpak with none of
`/dev/uinput`, `/dev/i2c-*` or an arbitrary `Process`. A feature that cannot
run says why; it never fails silently.

The uniform form is `PlatformCapabilitySet`, an open set of string-named
`PlatformCapability` values with a per-capability reason. Open rather than an
enum because a backend added later — WP-C2's GNOME extension, a new portal
revision — must be able to declare a capability without editing the core, and
an unknown name read back from a C vtable is data, not a crash.

**2. Read back after writing.** `PlatformError.notApplied` exists because
compositors, D-Bus bridges, X11 window managers, DDC monitors and macOS
Accessibility all acknowledge a request and then do something else. It mirrors
`VS_ERR_NOT_APPLIED` in `linux/platform/include/vorssaint_platform.h`, and it
is the same judgement `WindowLayoutService` already makes when it re-reads a
frame after setting it.

**3. The Swift side adds no behaviour.** Retries, tolerance checks and
read-back live in the C backends, where they can be tested against a real
compositor without a Swift toolchain (`linux/platform/README.md`). The Swift
wrapper in `Sources/VorssaintLinux` marshals and nothing else. That is why
`WindowSystem.moveResize` takes a `tolerance` instead of the core comparing
frames itself.

## 2. What mirrors what

Two protocols are mirrors of contracts that already landed, not designs of
their own. Their names and semantics are owned by the other side, and a field
renamed there is renamed here in the same PR.

### `WindowSystem` ↔ `vs_window_system` (WP-C1)

`linux/platform/README.md` § "How WP-12 mirrors it" prescribes the mapping and
this is it, line for line:

| C | Swift |
|---|---|
| `vs_window_system` | `protocol WindowSystem` |
| `vs_window_capability` | `WindowSystemCapabilities: OptionSet` |
| `vs_window_info` | `struct WindowInfo` |
| `vs_window_flag` | `WindowInfo.Flags: OptionSet` |
| `vs_rect` | `WindowRect` (`Int32` members, `.cgRect` on demand) |
| `vs_result` | `PlatformError`; `VS_OK` is a normal return |
| `vs_window_event` + callback | `WindowEvent`, delivered from `dispatch()` |
| `list` + `free_list` | one call returning `[WindowInfo]` |
| `name`, `capabilities` | `var name`, `var capabilities` |

Three C conventions survive into Swift because dropping them would lose
information:

- **"Unknown is never zero."** `pid` and `workspace` are `-1` in C and `nil`
  in Swift, never `0`, because 0 is a real pid and a real workspace.
- **Capabilities only shrink, and only on an event.** A method whose bit is
  clear still exists and throws `PlatformError.unsupported`; callers never
  test for `nil`. The bitmask changes only on `WindowEvent.backendLost`.
- **One instance, one thread, no surprises.** Nothing starts a thread; events
  are delivered only from inside `dispatch()`, on the calling thread.

`WindowSystemID` is `UInt64`, like `vs_window_id`. It is deliberately **not**
`PlatformWindowID` (`UInt32`): X11 uses an XID, wlroots a handle serial, and
Hyprland a 64-bit window *address*, so nothing narrower can carry it.
`PlatformWindowID` stays `UInt32` because that is what `CGWindowID` and
`CGDirectDisplayID` already are, which is what let `RecorderSupport` move into
the core without touching a single caller. The macOS adapter widens one into
the other losslessly; the reverse narrowing is valid only on macOS and nothing
promises it.

### `InputInterceptor` ↔ `org.vorssaint.Helper1` (WP-S1)

| D-Bus (`PRIVILEGES.md` § 4.1) | Swift |
|---|---|
| `Enable(b)` | `setEnabled(_:)` |
| `SetRules(s json)` | `setRules(_:)`, `InputRules` |
| `GetDevices(out s)` | `devices()`, `InputDevice` |
| `GetCapabilities(out s)` | `inputCapabilities`, `InputCapabilities` |
| `Event(t,q,q,i)` | `startRecordingTap(onEvent:)`, `RecordedInput` |
| `Backend` property | `backendName` |
| `Error.NotOwner` | `InterceptionLoss.takenByAnotherSession` |

The helper's central property is preserved rather than flattened: **rules go
in, events do not come out.** Outside tap mode the helper emits nothing,
deliberately — streaming every event to a session process "would have moved
the keylogger into the unprivileged half and gained nothing". So the core
declares what it wants done and the privileged half does it; only the shortcut
recorder, for the seconds it is open, asks to see keys. The macOS adapter
honours the same contract over a `CGEventTap`.

`InputCapabilities` keeps the helper's `GetCapabilities` discipline: answer by
*trying*, never by inferring from a name, and keep the errno, because `ENOENT`
(no node) and `ENODEV` (a node whose driver is missing) are different problems
and only the first is a packaging mistake.

## 3. The fourteen protocols

`Linux backend` names the directory under `linux/platform/` that will hold the
C implementation. Only `window/` exists today.

| Protocol | File | macOS implementation | Linux backend | Consumed by |
|---|---|---|---|---|
| `ShortcutRegistrar` | `ShortcutRegistrar.swift` | `MacShortcutRegistrar` (`MacPlatformAdapters.swift`) | `shortcuts/` (XDG GlobalShortcuts portal; KWin/Hyprland/Sway IPC; helper evdev grab) | `HotkeyManager`, every feature with a shortcut |
| `ClipboardAccess` | `ClipboardAccess.swift` | `MacClipboardAccess.swift` | `clipboard/` (`ext_data_control_manager_v1`, `zwlr_data_control_manager_v1`, clipboard portal, GNOME bridge) | `ClipboardHistoryService`, snippets, command bar, shelf |
| `WindowSystem` | `WindowSystem.swift` | `MacWindowSystem.swift` | `window/` (**landed**, WP-C1) | `WindowEnumerator`, `WindowActivator`, `WindowLayoutService`, `AutoQuitService` |
| `ScreenCapturer` | `ScreenCapturer.swift` | `MacScreenCapturer` (`MacPlatformAdapters.swift`) | `capture/` (WP-B1: ScreenCast portal/PipeWire, `ext_image_copy_capture_manager_v1`) | screenshot, recorder, OCR, barcode |
| `AudioGraph` | `AudioGraph.swift` | `MacAudioGraph` (`MacPlatformAdapters.swift`) | `audio/` (PipeWire) | mixer, volume roller, mic mute, sound output switcher |
| `SystemSensors` | `SystemSensors.swift` | `MacSystemSensors.swift` | `sensors/` (`/proc`, `/sys/class/hwmon`, `/sys/class/power_supply`, UPower) | monitor panel, menu-bar metrics, fan control, battery |
| `PowerControl` | `PowerControl.swift` | `MacPowerControl.swift` | `power/` (logind, Inhibit portal, `/sys/class/backlight`, helper DDC) | keep awake, brightness, extra dimming, fan control |
| `InputInterceptor` | `InputInterceptor.swift` | `MacInputInterceptor` (`MacPlatformAdapters.swift`) | `input/` (helper client → `org.vorssaint.Helper1`) | super key, keyboard/click debounce, smooth scroll, middle click, mouse navigation, button remaps |
| `AppLauncher` | `AppLauncher.swift` | `MacAppLauncher.swift` | `launch/` (OpenURI portal, `.desktop` entries, `Process`) | command bar, uninstaller, kill process, auto-quit, Homebrew |
| `PlatformNotifier` | `PlatformNotifier.swift` | `MacNotifier.swift` | `notify/` (`org.freedesktop.Notifications`, Notification portal) | cleaner, WhatsApp organizer, updates, monitor alerts |
| `TrashAndFiles` | `TrashAndFiles.swift` | `MacTrashAndFiles.swift` | `files/` (freedesktop trash spec, `org.freedesktop.FileManager1`) | junk cleaner, uninstaller, screenshots, managed downloads, disk-image installer, bundle migration |
| `PackageManager` | `PackageManager.swift` | `MacPackageManager` (`MacPlatformAdapters.swift`) | `packages/` (apt, dnf, pacman, zypper, flatpak, snap) | Homebrew feature, app updates, cleaner |
| `SessionEvents` | `SessionEvents.swift` | `MacSessionEvents.swift` | `session/` (logind, Settings portal, window layer) | keep awake, bluetooth sleep, appearance, auto-quit, clipboard |
| `Capabilities` | `Capabilities.swift` | `MacCapabilities.swift` | none — aggregates the other thirteen | feature hub, Capabilities page, energy badges, `FeatureCatalog.requiredCapabilities` (WP-15) |

`PlatformNotifier` is the protocol's name, in a file of the same name, because
`Services/Notifier.swift` already declares `enum Notifier` and `build.sh`
compiles `Sources/Vorssaint`, `Sources/VorssaintCore` and
`Sources/VorssaintMac` into **one** `swiftc` invocation (`PLAN.md` § 5). The
type would collide, and so does the *file*: one invocation rejects two files
with the same basename outright — `error: filename "Notifier.swift" used
twice` on run 34693363900 — because it uses filenames to tell private
declarations apart.

**A rule for everyone working in these three directories:** a new file's
basename must be unique across all of `Sources/Vorssaint`,
`Sources/VorssaintCore` and `Sources/VorssaintMac`, not just within its own.

```
$ find Sources/Vorssaint Sources/VorssaintCore Sources/VorssaintMac \
      -name '*.swift' -exec basename {} \; | sort | uniq -d
(empty)
```

## 4. The capability flags

Grouped by protocol; the string is the `PlatformCapability` raw value, which is
what crosses the C boundary and a `CoreBridge` snapshot.

| Capability | Means | Absent where |
|---|---|---|
| `shortcut.directBinding` | the app chooses the combination | — |
| `shortcut.portalBinding` | the session chooses, user confirms | macOS |
| `shortcut.overrideSystem` | can take a combination the system holds | macOS, most compositors |
| `clipboard.watch` | change notification without polling | macOS (polls `changeCount`), GNOME without the bridge |
| `clipboard.sourceApplication` | can name the clipboard's owner | macOS |
| `clipboard.primarySelection` | the middle-click selection | macOS |
| `clipboard.concealedEntries` | can mark an entry as a password | — |
| `window.list` / `.focus` / `.close` / `.minimize` | `vs_window_capability` bits 0–3 | GNOME without the bridge |
| `window.moveResize` | bit 4 — the most commonly missing one | GNOME, bare foreign-toplevel |
| `window.workspaces` | bit 5 | most foreign-toplevel sessions |
| `window.events` | bit 6 — pushes instead of needing a poll | macOS, X11 polling fallback |
| `window.previews` | bit 7, reserved for WP-C3 | every backend today |
| `capture.display` / `.window` / `.area` / `.stream` | what can be captured | X11 without XSHM |
| `capture.windowExclusion` | can leave our overlay out of the recording | most portals |
| `capture.ownPicker` | the app may draw its own region picker | every Wayland portal |
| `capture.restoreToken` | consent survives between captures | macOS |
| `audio.streamVolume` | per-application volume | macOS only through its own tap |
| `audio.streamRouting` | one app's audio to another device | macOS |
| `sensors.thermalPressure` | `ProcessInfo.ThermalState`'s replacement | — |
| `sensors.memoryPressure` | a real pressure figure, not a ratio | Linux (derived from `MemAvailable`) |
| `power.inhibitLidClose` | awake with the lid shut | macOS |
| `power.externalBrightness` | DDC/CI over `/dev/i2c-*` | Flatpak, machines without the udev rule |
| `input.swallow` | can withhold an event from the focused app | Flatpak, no helper |
| `input.synthesize` | can emit events the user did not make | no `/dev/uinput` |
| `input.recordTap` | tap mode, for the shortcut recorder | no helper |
| `input.requiresHelper` | a prompt must be explained first | macOS (`false`) |
| `launch.runCommand` | can run an arbitrary command | Flatpak without `org.freedesktop.Flatpak` |
| `launch.revealInFileManager` | `FileManager1.ShowItems` | file managers that do not implement it |
| `notify.actions` | action buttons | notification daemons that show none |
| `files.trash` / `.restoreFromTrash` / `.enumerateTrash` | freedesktop trash spec, `FileManager.trashItem` | — |
| `files.emptyTrash` | the app may empty it | macOS (Finder owns the Trash) |
| `packages.upgrade` | can upgrade without privileges the app lacks | every system backend; per-user Flatpak and Homebrew only |
| `session.idleTime` | how long since the user touched anything | most Wayland sessions |

## 5. What the macOS adapters actually do, and what they declare absent

The brief for WP-12 is explicit that services are **not** rewritten to call
through the protocols — that migration is per-feature work. So each adapter is
a complete conformance, and where the behaviour still lives inside a service
with its own state, the adapter declares that capability *absent with the
reason naming the owner* rather than reimplementing it. A caller reads `false`
and explains itself; it never calls something that quietly does nothing.

**Wired, real:**

| What | How | Why it was safe to wire |
|---|---|---|
| `MacWindowSystem.list()` | `CGWindowListCopyWindowInfo` | a pure read; the same call `WindowEnumerator` makes. Reverses the front-to-back list into the bottom-most-first order the C contract promises, which is the two-way check that the mirror is faithful |
| `MacSystemSensors.thermalPressure` | `ProcessInfo.thermalState` | a pure read; **this is that property's home in the port** (Foundation gap 3) |
| `MacTrashAndFiles` | `FileManager.trashItem` and friends | **Foundation gap 4**; no service owns the trash |
| `MacSessionEvents` | the `NSWorkspace` notifications the app already observes, plus the two screen-lock distributed notifications | observing is additive; nothing else changes |
| `MacClipboardAccess` | `NSPasteboard.general` with the `changeCount` poll | the poll is per-instance and starts only when watched |
| `MacAppLauncher` | `NSWorkspace`, `Process`, `InstalledApps.installedApplications` | `InstalledApps` is the scan the command bar and uninstaller already share |
| `MacPowerControl` assertions | `IOPMAssertionCreateWithName` | assertions are per-handle and independent of `KeepAwakeManager`'s |
| `MacScreenCapturer.displays()` | `NSScreen.screens` | a pure read; the layout feature needs work areas |
| `MacNotifier.post` | delegates to `Notifier.post(title:body:)` | a strict wrap — the notification is byte-for-byte the old one |
| `MacCapabilities` | asks the other thirteen | no per-OS table anywhere |

**Declared absent, with the owner named:**

| Capability | Still owned by | Closed by |
|---|---|---|
| `shortcut.*` registration | `HotkeyManager` (Carbon `EventHotKeyRef` table, layout-change re-registration) | WP-21 |
| `capture.display/window/area/stream` | `ScreenshotCaptureEngine`, `RecorderCaptureEngine` (the `SCStream`, its configuration, the permission state) | WP-B1 and the recorder wave |
| `audio.*` | `AppVolumeMixer`, `MixerRouting`, `SoundOutputSwitcher`, `BoostLimiter` (the CoreAudio tap and render callback) | the audio wave |
| `window.focus/close/minimize/moveResize/workspaces` | `WindowActivator`, `WindowLayoutService` (the `AXUIElement` cache, the retry ladder, the tolerance read-back) | WP-C3, WP-C4, WP-C5 |
| `sensors.cpu/memory/temperature/fanSpeed/battery/network/disk` | `SystemMonitor`, `SMCClient`, `VMStatisticsDecoder`, `PowerSampler` (each a sampler with its own cadence and smoothing; `MonitorSamplingPolicy` exists to hold that) | the monitor wave |
| `power.internalBrightness/externalBrightness/extraDimming/fanControl` | the brightness service's DDC retry ladder, `ExtraBrightnessService`, `FanControlXPC`'s handshake and watchdog | the power wave |
| `input.*` verbs | `PointerTapRunLoop` and the per-feature `CGEventTap` installers | WP-D1 and the input wave |
| `packages.*` | `HomebrewSupport`'s service | the packages wave |
| `notify.actions` | `Notifier.postWhatsAppOrganization` (its `UNNotificationCategory` and transaction UUID) | the managed-downloads wave |

A second reader of the same hardware or the same event tap would either
duplicate that state or race it, which is why an honest `false` is better than
a plausible-looking second implementation.

## 6. The fakes

`Sources/VorssaintCoreTestSupport/` holds one `Fake*` per protocol.

Linux CI builds this target (`swift build --target VorssaintCoreTestSupport`),
in the step the deleted `VorssaintCombine` build used to occupy. That is not a
formality: it is the only thing in the package that imports `VorssaintCore`
across a real module boundary, so it is what catches a protocol that forgot to
be `public`. The single-module macOS build cannot show that.

Every fake shares one rule with the real backends: **a call whose capability is
absent throws rather than quietly succeeding**, which is what lets a test prove
a feature degrades honestly. Two of them are scriptable in the way the real
backends differ:

- `FakeWindowSystem.moveResizeLands = false` produces `VS_ERR_NOT_APPLIED` —
  a compositor that agrees and does nothing — and `loseBackend(keeping:)`
  shrinks the capabilities and queues `backendLost`, which is the only way the
  C contract allows them to change. Events are queued by the test and
  delivered only from `dispatch()`.
- `FakeInputInterceptor` defaults to `InputCapabilities.helperMissing`, whose
  reason is the errno the WP-03 container actually produced
  (`open /dev/uinput: No such file or directory (errno 2)`), so a test that
  wants the happy path has to ask for it.

### 6.1 What the fakes actually cover, and what nothing covers

Stated plainly, because "there is a fake for every protocol" and "every
protocol is tested" are not the same claim and only the first is true today.

| Fake | Coverage |
|---|---|
| `FakeWindowSystem` | **driven**: list, activate, close, moveResize (applied and not-applied), geometry, dispatch, backendLost, capability refusal |
| `FakeInputInterceptor` | **driven**: setRules (accepted and rejected), tap mode open/emit/close, the missing-helper capability reason |
| `FakeCapabilities` | **driven**: has, supportsAll, missing, set + onChange |
| `FakeSystemSensors` | **driven**: thermal pressure observation across all four levels |
| `FakeClipboardAccess`, `FakeScreenCapturer`, `FakeAudioGraph`, `FakePowerControl`, `FakeAppLauncher`, `FakeNotifier`, `FakeTrashAndFiles`, `FakePackageManager`, `FakeSessionEvents`, `FakeShortcutRegistrar` | **constructed only** — `testEveryFakeAnswersItsOwnCapabilityQuestion` builds each and reads `platformCapabilities`. Their recorded-call surfaces exist and compile, but nothing asserts on them yet |

**No macOS adapter is executed by any test.** All fourteen
(`MacWindowSystem` … `MacCapabilities`) are compiled by `build.sh`'s app glob
and referenced by nothing:

```
$ grep -rl "MacWindowSystem\|MacClipboardAccess\|MacScreenCapturer\|MacAudioGraph\|\
MacSystemSensors\|MacPowerControl\|MacInputInterceptor\|MacAppLauncher\|MacNotifier\|\
MacTrashAndFiles\|MacPackageManager\|MacSessionEvents\|MacShortcutRegistrar\|MacCapabilities" \
      Sources/Vorssaint Tests/ | wc -l
0
```

That is the compiler gate and nothing more. The exception is the *seams*: the
macOS suite does execute `AppKitImageDataValidator`,
`CoreFoundationTransliterator`, `FoundationMeasurementFormatter` and
`FoundationDurationFormatter`, because `MacPlatformSeams.install()` runs on the
harness's first line and the radial-icon, pinyin, unit-conversion and
days-until assertions then go through them. Those four are behaviour-pinned;
the fourteen adapters are not.

The cheapest way to close this is three pure-read assertions in the macOS
harness — `MacWindowSystem.list()` is non-empty, `MacSystemSensors.pressure(from:)`
maps all four cases, `MacTrashAndFiles.directory(for:)` answers every
`StandardDirectory` — but each one moves the `TESTS OK (N checks)` number that
this work package uses as its "macOS unchanged" proof, so they belong in the
push *after* the baseline is re-established, not in this one.

`Tests/VorssaintCoreTests/PlatformProtocolTests.swift` drives them —
seventeen checks, hand-written, and safe from regeneration because
`Tools/linux-port/port-tests.py` only writes `Generated*.swift`. They are the
`Executed 17 tests, with 0 failures` line of the Linux gate:

```
=== totals =======================================
	 Executed 17 tests, with 0 failures (0 unexpected) in 0.505 (0.505) seconds
	 Executed 90 tests, with 0 failures (0 unexpected) in 8.081 (8.081) seconds
```

## 7. The six seams

Separate from the fourteen protocols: six single platform calls that sat
inside otherwise pure files and kept them out of the core. Each is a protocol
with a macOS implementation installed by `MacPlatformSeams.install()`, called
on the first line of `Sources/Vorssaint/main.swift` and of the
`Tests/MetricsTests.swift` harness, before anything can read them.

| Seam | Was | macOS implementation | Portable default |
|---|---|---|---|
| `ImageDataValidator` | `NSImage(data:) == nil` in `RadialMenuSupport.sanitized` | `AppKitImageDataValidator` | `PermissiveImageDataValidator` — keeps the icon, because a platform with no decoder must not silently delete a person's stored icons |
| `Transliterator` | `CFStringTransform` ×2 in `CommandBarSearch.pinyinKeywords` | `CoreFoundationTransliterator` (the original spelling) | `FoundationTransliterator` over `String.applyingTransform` — **measured working on Linux**, see § 7.1 |
| `MeasurementFormatting` | `MeasurementFormatter` in `CommandBarUnits.format` | `FoundationMeasurementFormatter` (identical settings) | `SymbolMeasurementFormatter` — localized number, unit as its own symbol |
| `DurationFormatting` | `DateComponentsFormatter` in `CommandBarDates.daysUntil` | `FoundationDurationFormatter` (identical settings) | `PlainDurationFormatter` — localized number, untranslated unit word |
| `PlatformWindowID` / `PlatformDisplayID` | `CGWindowID` / `CGDirectDisplayID` in `RecorderSupport` | identity (both are already `UInt32`) | `UInt32` |
| `FoundationNetworking` + `Date()` timing | `URLSession` + `CFAbsoluteTimeGetCurrent` in `SpeedTest` | unchanged | an import guard, and `Date().timeIntervalSinceReferenceDate`, which is the same clock and the same epoch |

### 7.1 Does Linux ICU carry the Mandarin-Latin transliterator?

The brief for WP-12 asked this to be measured rather than assumed, and it is
measured twice on every Linux CI run.

**The assertion.** `Tests/VorssaintCoreTests/GeneratedCommandBarSearchAndRanking.swift:136`
pins the exact answer:

```swift
expect(CommandBarSearch.pinyinKeywords("云笔记") == "yunbiji ybj",
       "pinyin keywords run the syllables together and add the initials")
```

That test runs on Linux through `swift test --filter VorssaintCoreTests`,
against `FoundationTransliterator` — the Linux default, since
`MacPlatformSeams.install()` is macOS-only. It passes, so
`String.applyingTransform(.mandarinToLatin)` and `.stripDiacritics` give
swift-corelibs-foundation 6.1.3 the same answer CoreFoundation gives macOS,
for the one string the product actually depends on.

**The probe.** `Sources/VorssaintLinux/main.swift` additionally prints, on
every run, `FoundationTransliterator().capabilities`, the result of
`mandarinLatin("中文")`, and what the raw `StringTransform("Any-Latin")` and
`StringTransform("Mandarin-Latin")` identifiers do, so the answer is in the CI
log of every build rather than only in this document. It is printed, never
asserted: the point is to notice the day a toolchain or a container image
changes it.

So the Linux default is `FoundationTransliterator`, not the `NoTransliterator`
fallback. `NoTransliterator` stays for the case the capability flag exists
for — a build whose ICU data was stripped, which `AppImage` and `Flatpak`
packaging (WP-P1, WP-P2) can produce by accident and which `capabilities`
will then report as `canRomanizeMandarin: false` instead of silently indexing
titles unchanged.

## 8. The Foundation gaps from `PLAN.md` § 4.1

WP-00 § 8 condition 3 named four. All four now have a home:

| Gap | Where it was | Home |
|---|---|---|
| `FoundationXML` (`XMLParser`) | `AppUpdateFeedSupport.swift:97,150` | **open** — the file is still blocked by `JunkCleaner`, `InstalledApps`, `ShelfService` and a UI file, so the guard lands with the move, not before it |
| `ProcessInfo.ThermalState` | `FanControlSupport.swift` | `SystemSensors.thermalPressure`, `MacSystemSensors.pressure(from:)` |
| `CFGetTypeID`/`CFBooleanGetTypeID` | `SettingsBackupSupport.swift:313-324` | **open** — the file is still blocked by `Defaults`, `FeatureCatalog` and `MediaSupport` |
| `FileManager.trashItem` | seven files, `CORE_MOVES.md` § 4.1 | `TrashAndFiles`, `MacTrashAndFiles` |

Two more were found by the Linux compiler rather than by any census, because
both are Foundation *classes marked unavailable* on Linux rather than missing
modules, and an import-counting census cannot see those:

| Gap | Where | Found by | Home |
|---|---|---|---|
| `MeasurementFormatter` | `CommandBarUnits.format` | run 34665056778 | `MeasurementFormatting`, seam 3 |
| `DateComponentsFormatter` | `CommandBarDates.daysUntil` | run 34693363900 | `DurationFormatting`, seam 4 |

A sweep of the rest of that family over the whole core now finds only the
comments that explain the seams, and one call on a seam itself:

```
$ grep -rn "DateIntervalFormatter\|MeasurementFormatter\|DateComponentsFormatter\|\
RelativeDateTimeFormatter\|ListFormatter\|PersonNameComponentsFormatter" \
      Sources/VorssaintCore/ | grep -v "Platform/"
Services/CommandBar/CommandBarDates.swift:172:  // Was a DateComponentsFormatter built inline; WP-12 put it behind
Services/CommandBar/CommandBarUnits.swift:200:  /// Was a `MeasurementFormatter` built inline; WP-12 put it behind
Services/CommandBar/CommandBarUnits.swift:207:      MeasurementFormatters.current.string(
```

## 9. What CI proved

`.github/workflows/linux-port-ci.yml`, both jobs hard gates. The Linux job's
step list is the checklist for this work package, and every step passed on
[run 34693623124, job 103553191935](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34693623124/job/103553191935):

| step | what it proves for WP-12 |
|---|---|
| Build VorssaintCore | the fourteen protocols, the six seams and the newly moved files compile on swift-corelibs-foundation |
| Build VorssaintCoreTestSupport | every protocol is reachable across a real module boundary — the check the single-module macOS build cannot make |
| Build VorssaintLinux | the core's public surface links into an executable |
| Run VorssaintLinux | the transliterator probe of § 7.1 runs |
| Unit tests | `Executed 17 tests, with 0 failures` (PlatformProtocolTests) and `Executed 90 tests, with 0 failures` (the ported suite) |

Three Linux-only compile errors were found along the way, each a Foundation
*API* rather than a missing module, and each closed with a seam:
`MeasurementFormatter` (run 34665056778), `DateComponentsFormatter`
(run 34693363900), and — on the macOS side — the single-module rule that two
files may not share a basename (run 34693363900).

## 10. Open edges for other work packages

- **Displays have no C section.** `vorssaint_platform.h` carries only an
  output *name* on `vs_window_info`, so `PlatformDisplay` is returned by
  `ScreenCapturer.displays()` and its `name` is the join between the window and
  display worlds. WP-C1 adding a `vs_display_system` section is what makes the
  numeric id authoritative.
- **`WindowSystemID` is 64-bit; `PlatformWindowID` is 32-bit.** Any Swift
  wrapper that bridges the two must widen, never narrow: a Hyprland window id
  is an address.
- **`ShortcutBinding` is not `GlobalShortcut`.** `CORE_MOVES.md` § 2 proved
  `GlobalShortcut` cannot be split from Carbon, and 27 of its 30 non-UI users
  need the Carbon half. Wiring `MacShortcutRegistrar` is therefore a
  translation layer, not a wrap — WP-21's work, and the same place the
  `UCKeyTranslate` → xkbcommon question is answered.
- **`FeatureCatalog.requiredCapabilities`** (WP-15) should be spelled in the
  `PlatformCapability` values of § 4, so the hub renders from what the session
  probed rather than from a per-OS table.
