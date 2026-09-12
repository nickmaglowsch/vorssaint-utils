# Linux port: work packages

The executable backlog. Each package is sized for one agent, one branch, one
PR (split further if it grows past ~800 changed lines). IDs are stable;
status is edited in place by the lead. Dependencies are hard: a package does
not start until every `after` package is merged into `linux/main`.

Sizes: S ≈ a day of agent work, M ≈ two to three days, L ≈ a week. They are
planning weights, not promises.

Status values: `todo`, `in progress (agent)`, `review`, `merged`, `blocked`,
`dropped`.

Every package inherits the acceptance criteria in `AGENT_PLAYBOOK.md` §
Definition of done. The criteria listed here are the package-specific ones.

---

## Phase 0: spikes (go/no-go evidence, throwaway code)

Spikes live under `spikes/` on their own branches and are never merged into
`linux/main`; their reports are committed to `docs/linux-port/spikes/`.

| ID | Title | Size | after | Owner role | Status |
|---|---|---|---|---|---|
| WP-00 | Swift core on Linux | M | – | Core porter | merged (GO WITH CONDITIONS) |
| WP-01 | Shell toolkit bake-off | M | – | Shell squad | merged (Qt 6 Quick recommended; layer-shell needs vendoring) |
| WP-02 | Portal ScreenCast + PipeWire capture proof | M | – | Feature squad | merged (GO on wlroots; 3 xdpw defects found) |
| WP-03 | evdev/uinput input relay proof | M | – | Systems squad | merged (GO; uinput untestable here) |
| WP-04 | Self-contained packaging proof | M | WP-01 | Packaging/CI | merged (AppImage primary, reduced Flatpak secondary) |

**WP-00 Swift core on Linux.** Install the current Swift toolchain (6.x) on
Ubuntu 24.04 and Fedora. Compile, unmodified, these twelve files (`FeatureCatalog.swift`,
`Localization.swift` and one locale file, `Defaults.swift`,
`GlobalShortcut.swift` minus the Carbon table, `URLCleaning.swift`,
`SwitcherSupport.swift`, `CommandBarSupport.swift`, `CommandBarMath.swift`,
`ScreenshotSupport.swift`, `RecorderTimeline.swift`,
`ClipboardHistorySupport.swift`) with stubs for what is missing. Record every
compile error by category (Combine, CoreGraphics types, AppKit leaks,
Foundation gaps). Try OpenCombine as a drop-in for `Combine`. Measure static
linking of stdlib + Foundation (`-static-stdlib`, Static Linux SDK).
*Deliverable:* `spikes/00-swift-core.md` with the error census, the OpenCombine
verdict, the static-link result, and a go/no-go on "keep the core in Swift".

**WP-01 Shell toolkit bake-off.** Build the same three screens
(tray icon + panel with a live-updating CPU sparkline; a preferences window
with a toggle, a slider, a shortcut recorder; a transparent fullscreen
overlay with a drag-rectangle) in each candidate, all driven by the same
Swift fake service through the snapshot/command bridge described in
`PLAN.md` § 4.2: (a) Qt 6 Quick with a C++ `CoreModel` over the `@_cdecl`
bridge (the plan's default), (b) Qt Bridge for Swift (Qt's own preview:
QML views, Swift logic, no C++), (c) Adwaita for Swift (GTK4/libadwaita,
all Swift). Score: maintenance activity, binary size, memory, build
reproducibility, AppImage bundling effort, `layer-shell` support, look on
GNOME and KDE, and how declarative the view code is next to the SwiftUI it
replaces.
*Deliverable:* `spikes/01-toolkit.md` with the scored matrix and a
recommendation; screenshots of each build on GNOME and KDE; the bridge
prototype (three C functions, JSON snapshots and commands) kept for
WP-18.

**WP-02 Portal ScreenCast proof.** A CLI that, through
`org.freedesktop.portal.ScreenCast`, selects a monitor, a window and a
region, receives PipeWire frames, writes 10 seconds of H.264 with bundled
ffmpeg libraries plus system audio from a PipeWire monitor node, and takes a
single frame via `org.freedesktop.portal.Screenshot`. Try restore tokens for
permission persistence. Run on GNOME 48, Plasma 6, Sway.
*Deliverable:* `spikes/02-capture.md` with a support matrix (which sources
work where, latency, restore token behaviour), and the ffmpeg link approach.

**WP-03 Input relay proof.** A small daemon that grabs all keyboards and mice
(`EVIOCGRAB`), re-emits through one uinput device, and implements two rules:
Caps Lock tap → Escape / hold → Ctrl, and a 40 ms key chatter filter. Measure
added latency. Test with a non-US xkb layout that the uinput device must
mirror. Prototype the privilege split: unprivileged app ↔ D-Bus ↔ helper
started by systemd (system unit with polkit action) and the udev-rule /
`input` group alternative.
*Deliverable:* `spikes/03-input-relay.md` with latency numbers, the layout
mirroring result, and a recommended privilege model with its install steps.

**WP-04 Packaging proof.** Take the WP-01 winner and produce (a) an AppImage
with the toolkit bundled, built on the oldest supported glibc, and (b) a
Flatpak manifest with the permissions the port will need
(`--device=all`, `--filesystem=host`, `--socket=session-bus`,
`--system-talk-name=org.freedesktop.login1`, and so on). Launch both on a
clean Ubuntu 24.04, Fedora 42 and CachyOS/Arch KDE VM. Document what breaks
under Flatpak's sandbox (evdev, hwmon writes, DDC) and what portals cover.
*Deliverable:* `spikes/04-packaging.md`, with the recommended primary
format and the list of features that must degrade under Flatpak.

**Phase 0 gate (lead):** update `PLAN.md` § 4 with the outcome of the
five spikes. If WP-00 is a no-go, the plan switches to the fallback in
`PLAN.md` § 9 (core rewritten in Rust); the rest of this file is then
re-sized, not rewritten.

---

## Phase 1: shared core

Goal: `VorssaintCore` compiles and tests on Linux and macOS, and the macOS
app is behaviourally unchanged.

| ID | Title | Size | after | Owner role | Status |
|---|---|---|---|---|---|
| WP-10 | Package layout: VorssaintCore / VorssaintMac / VorssaintLinux targets | M | WP-00 | Core porter | merged (both gates green) |
| WP-11 | Move Foundation-only files into VorssaintCore | L | WP-10 | Core porter | todo |
| WP-12 | Platform protocol layer | L | WP-11 | Core porter | todo |
| WP-13 | Combine abstraction (OpenCombine on Linux) | S | WP-10 | Core porter | merged (per-file guard convention in COMBINE.md; lead accepted deleting the VorssaintCombine target, done after WP-11 lands) |
| WP-14 | Settings store abstraction (UserDefaults ↔ JSON/GSettings) | M | WP-12 | Core porter | todo |
| WP-15 | Feature catalog: platform support flags and Linux presets | S | WP-12 | Core porter | todo |
| WP-16 | Test harness: `swift test` on both platforms, port `Tests/*` pure checks | M | WP-11 | QA/reviewer | todo |
| WP-17 | macOS CI job proves `main` unchanged (selftest + ui-smoke on the new layout) | S | WP-11 | Packaging/CI | todo |
| WP-18 | `CoreBridge`: `@_cdecl` subscribe/command/snapshot surface, `Codable` snapshots and commands per service, diffing, fake-service tests | M | WP-12, WP-13 | Core porter | todo |

**WP-10.** `Package.swift` gains three targets. `Vorssaint` (macOS app)
depends on `VorssaintCore` and `VorssaintMac`; `VorssaintLinux` (executable)
depends on `VorssaintCore` and the toolkit chosen in WP-01. `build.sh` keeps
building the Mac app from the same sources (it passes file lists to `swiftc`,
so it needs the new directories). Nothing moves yet.
*Accept:* `./build.sh` and `swift build` (macOS) produce the same app;
`swift build --target VorssaintCore` succeeds on Linux with an empty core.

**WP-11.** Move every file that imports only `Foundation`/`Combine` (and
`CoreGraphics` types that reduce to `CGFloat/CGPoint/CGRect/CGSize`, which
Foundation provides on Linux) into `Sources/VorssaintCore`, grouped by feature
as today. Expected set: all `Core/*Strings.swift`, `Localization.swift` and
`Core/Localizations/*`, `FeatureCatalog.swift`, `FeaturePresets.swift`,
`Defaults.swift`, `URLCleaning.swift`, `SettingsBackupSupport.swift`, every
`Services/*/*Support.swift`, models (`SwitcherModels`, `RecorderTimeline`,
`RecorderMotion`, `RecorderEditDocument`, `ScreenshotSupport`,
`CommandBar{Support,Math,Units,Dates,Emoji,Links,QueryMemory}`,
`ClipboardHistorySupport`, `ShelfSupport`, `RadialMenuSupport` after
splitting its icon rendering out, `MetricFormat`, `MonitorSamplingPolicy`,
`SustainedAlertGate`, `SpeedTest`, `HomebrewSupport` parsers,
`AppUpdatesSupport`, `CleanerPolicy/Support/Schedule`, `UninstallerSupport`,
`KeepAwakeAutomationSupport`, `FanControlSupport/Policy`,
`BluetoothSleepSupport`, `MouseAppExceptionSupport`, `TextSnippetSupport`,
`SuperKeySupport`, `KeyboardDebounceSupport`, `MouseClickDebounceSupport`,
`SmoothScrollSupport`, `WindowLayoutSupport`, `WindowGestureSupport`,
`DockPreviewSupport` (kept for Mac), `FocusFollowsMouseSupport`,
`AutoQuitSupport`, `QuitProtectionSupport`). Files that are "almost" pure get
their AppKit corner extracted into a sibling file left in `VorssaintMac`.
*Accept:* Linux `swift build --target VorssaintCore` green; macOS app
unchanged; the moved-file list is in the PR with the per-file reason for
anything left behind. Per WP-00: the move order follows the declaration
graph (closed sets, every intermediate commit green), `Defaults.swift` and
`GlobalShortcut.swift` are split first (key table separated from
`UCKeyTranslate`), the `Darwin`/`Vision` shim holes are closed so the
208-file service layer gets a real error census, and the WP-00 census is
treated as a prior only (measured ~2 % false-clean).

**WP-12.** Define the protocols the Linux backends and the Mac adapter both
implement, one per concern, each in its own file under
`VorssaintCore/Platform/`: `ShortcutRegistrar`, `ClipboardAccess`,
`WindowSystem` (list, focus, close, minimize, move/resize with capability
flags), `ScreenCapturer`, `AudioGraph` (streams, sinks, sources, volumes),
`SystemSensors` (cpu, memory, temps, fans, battery, network, disk),
`PowerControl` (inhibit, lid, brightness), `InputInterceptor` (rules in,
events out), `AppLauncher`, `Notifier`, `TrashAndFiles`, `PackageManager`,
`SessionEvents` (sleep, wake, lock, active app), `Capabilities` (what this
session can do). Each protocol carries a `capabilities` value so services
can degrade honestly. The Mac adapter wraps the existing services without
changing their behaviour.
*Accept:* every protocol has a Mac implementation that the existing
services call through (no behaviour change proven by selftest + ui-smoke),
and a `Fake*` implementation used by tests. Per WP-00, the Foundation gaps
named in `spikes/00-swift-core.md` § 8 condition 3 (`FoundationXML`,
`ProcessInfo.ThermalState`, `CFGetTypeID`/`CFBooleanGetTypeID`,
`FileManager.trashItem`) each get a protocol home or a shim here.

**WP-13.** `import Combine` becomes `import VorssaintCombine`, a tiny module
that re-exports Combine on Darwin and OpenCombine (with
`OpenCombineFoundation` for `Timer`/`NotificationCenter` publishers) on
Linux. Audit `@Published` + `ObservableObject` usage for anything OpenCombine
lacks.
*Accept:* core builds on both; the audit list is in the PR. Per WP-00 the
OpenCombine verdict is provisional (one candidate file imported Combine):
re-measure against the real `ObservableObject` services, and do not name any
shim target `Combine` (circular-module error).

**WP-14.** `DefaultsKey` stays; `UserDefaults.standard` reads/writes go
through a `SettingsStore` protocol with a `UserDefaults` implementation on
macOS and a JSON file (`$XDG_CONFIG_HOME/<app>/settings.json`, atomic
writes, change notifications) on Linux. Settings backup format is unchanged
so a Mac backup imports on Linux for shared keys.
*Accept:* round-trip test of all 616 keys; backup import test across
platforms.

**WP-15.** `AppFeature.isSupportedOnThisPlatform` replaces the fan-control
only `isHardwareSupported`; the Linux set follows `FEATURE_TRIAGE.md`
(dropped features hidden). Linux presets: Essentials (monitor, mixer, keep
awake, clipboard, snippets, screenshot), Windows (switcher, layout,
auto-quit), Battery and quiet (power monitor, keep awake automations,
bluetooth sleep, brightness).
*Accept:* hub unit test enumerates the Linux catalog; strings for the new
hub explanations in all languages.

**WP-16.** Replace the `swiftc`-compiled `Tests/*.swift` harness with
`swift test` targets for the core (keeping the Mac-only files, such as
`RecorderWriterTests`, under `#if os(macOS)`), so both CI legs run the same
suite. Keep `build.sh --test` working by delegating to `swift test` on
macOS.
*Accept:* `swift test` green on Linux and macOS; count of ported assertions
reported.

**WP-17.** Add the Linux job to `.github/workflows/ci.yml` (Ubuntu 24.04 +
Swift toolchain, `swift build`, `swift test`) and keep the macOS jobs as a
gate. Add `Tools/ui-smoke.sh` to the macOS job if it is not there already.
*Accept:* both legs green on `linux/main`.

**WP-18.** The Swift side of the bridge from `PLAN.md` § 4.2. Each
service that the Linux shell renders gets a `Snapshot: Codable` (its
published state) and a `Command: Codable` enum (what its views call), and
registers both with `CoreBridge`. Snapshots are diffed before delivery.
Start with `FeatureRuntime`, `L10n` and one metrics service; the rest are
added by the squads that port each feature.
*Accept:* round-trip tests through the C surface with a fake service; a
documented convention file `docs/linux-port/BRIDGE.md` that feature squads
follow.

**Phase 1 gate:** `PLAN.md` § 8.

---

## Phase 2: Linux app shell

Goal: an installable app that launches, sits in the tray, opens the panel
and settings, records shortcuts, persists, autostarts and can install and
uninstall features. No features yet beyond a CPU readout used as a probe.

| ID | Title | Size | after | Owner role | Status |
|---|---|---|---|---|---|
| WP-20 | Linux executable skeleton (`linux/shell` Qt Quick app + `VorssaintLinux` core), `CoreModel` QML binding, main loop, single-instance, CLI flags (`--selftest`) | M | WP-18, WP-01 | Shell squad | todo |
| WP-21 | Tray: StatusNotifierItem host with per-readout items, GNOME AppIndicator detection | M | WP-20 | Shell squad | todo |
| WP-22 | Panel window: popover anchored to the tray, tabs/sections, compact layout, drift-free positioning per compositor | L | WP-21 | Shell squad | todo |
| WP-23 | Settings window (Qt Quick preferences, one page per group), Features hub, onboarding, What's new | L | WP-20 | Shell squad | todo |
| WP-24 | Global shortcuts: portal GlobalShortcuts backend + shortcut recorder widget (resolves characters to keycodes with libxkbcommon; the helper only ever receives keycodes, per WP-03) + X11 XGrabKey fallback | L | WP-20 | Shell squad | todo |
| WP-25 | Capabilities page (replaces Permissions): portals, helper, groups, extensions, protocols | M | WP-23 | Shell squad | todo |
| WP-26 | Notifications, autostart, session events (logind sleep/lock, active-app via toplevel list) | M | WP-20 | Shell squad | todo |
| WP-27 | Icon mapping: SF Symbols → symbolic icon set bundled with the app | M | WP-20 | Shell squad | todo |
| WP-28 | Theming: light/dark follow portal Settings, app accent, compact density | S | WP-23 | Shell squad | todo |
| WP-29 | Overlay surfaces: LayerShellQt vendored and built against Qt 6 (Ubuntu 24.04 only packages the Qt 5 build, per WP-01), fullscreen transparent window fallback on GNOME and where layer-shell is absent, override-redirect on X11, per-output; runtime check for a compositing manager on X11 (transparent overlays render black without one) | M | WP-20 | Shell squad | todo |
| WP-P1 | Linux build script `build-linux.sh` + AppImage recipe: linuxdeploy + plugin-qt on the oldest supported glibc with a pinned Qt, the hand-built AppDir from `spikes/wp04-packaging` kept as the audit implementation and diffed in CI, excludelist including libpipewire/libwayland-client/libglvnd, `platformthemes` bundled, launcher names the libglvnd package when `libEGL.so.1` is missing | M | WP-04, WP-20 | Packaging/CI | todo |
| WP-P2 | Flatpak manifest with the reduced permission set only (per WP-04 no Flatpak can run the input relay), AppStream/desktop metadata, and a first-launch notice listing the features the sandbox removes | M | WP-P1 | Packaging/CI | todo |
| WP-P3 | Headless GUI smoke harness in CI (sway headless + Xvfb), smoke matrix runner across ubuntu 22.04/24.04, fedora, arch containers in FUSE and extract-and-run modes (build on oldest Qt, test on newest distro); capture smoke needs the WP-02 `xdpw-shm-only.patch` on the pixman renderer or a DRM-capable runner; re-run the WP-04 Flatpak sandbox probe on a real desktop | M | WP-P1 | Packaging/CI | todo |
| WP-P4 | Self-update: AppImageUpdate zsync feed, reuse feed parser | S | WP-P1 | Packaging/CI | todo |
| WP-S1 | `vorssaint-helper` privileged daemon in C, grown from `spikes/wp03-input-relay`: D-Bus API, polkit policy, systemd unit, udev rules, install/uninstall from the app, `PRIVILEGES.md`. Acceptance adds (per WP-03 review): `Enable(true)` bound to the caller's logind session; on real hardware `evtest` succeeds on a device immediately after `Enable(false)`; latency measured end to end on hardware; refused `EVIOCGRAB` names the holding process in the hub; hot-plug via `udev_monitor` | L | WP-03, WP-20 | Systems squad | todo |

Notes for the shell squad: the SwiftUI views in `Sources/Vorssaint/UI` are
the spec. Port screen by screen (one QML file per SwiftUI view, same
name), and read each view's observed service to find the state it needs;
that state is the service's `Snapshot` from WP-18. The panel's section registry,
ordering and hide/show settings are already pure logic and move to the core
in WP-11.

**Phase 2 gate:** `PLAN.md` § 8.

---

## Phase 3: features, in waves

Each wave is a set of packages that can run in parallel across squads.
A package delivers the backend, the panel/settings UI, tests, hub copy and
the triage matrix update for its feature(s).

### Wave A: no compositor dependence, no privilege

| ID | Feature(s) | Size | after | Status |
|---|---|---|---|---|
| WP-A1 | monitorCPU, monitorMemory, monitorNetwork (+ speed test), monitorDisk from `/proc`, `/sys`, `statvfs` | M | WP-22 | todo |
| WP-A2 | monitorPower via UPower, peripheral batteries, alerts | M | WP-22 | todo |
| WP-A3 | monitorGPU: amdgpu/i915 sysfs + NVML when present, vendor matrix | M | WP-A1 | todo |
| WP-A4 | Temperatures via hwmon, sensor selection rules reused; `--sensors` dump | S | WP-A1 | todo |
| WP-A5 | mixer, soundOutputSwitcher, micMute over PipeWire (libpipewire) with libpulse fallback | L | WP-22 | todo |
| WP-A6 | keepAwake via logind inhibitors, automations, menu bar icon states | M | WP-21, WP-26 | todo |
| WP-A7 | bluetoothSleep via BlueZ + logind | S | WP-26 | todo |
| WP-A8 | clipboardHistory, pastePlain, urlCleaner: data-control backend (KDE/wlroots/Hyprland), XFixes backend (X11), GNOME backend via the extension (WP-C2, may land later) or the portal Clipboard session, quick panel | L | WP-24, WP-29 | todo |
| WP-A9 | scratchpad | S | WP-24 | todo |
| WP-A10 | quickLauncher + killProcess | S | WP-22 | todo |
| WP-A11 | quickToggles (dark mode, lock, eject, trash, keyboard light, display off) | M | WP-22 | todo |
| WP-A12 | cleaner (XDG catalog, Flatpak dirs, journald) + messaging downloads organizer | M | WP-22 | todo |
| WP-A13 | focusFollowsMouse and mouseAcceleration as DE setting writers (GNOME/KDE/Sway/Hyprland/X11) | M | WP-25 | todo |
| WP-A14 | Menu bar readouts (values, bars, battery time, fan) on the tray items | M | WP-A1, WP-A2 | todo |

### Wave B: capture, media, tools

| ID | Feature(s) | Size | after | Status |
|---|---|---|---|---|
| WP-B1 | Capture engine: portal ScreenCast/Screenshot → PipeWire frames, restore tokens, output/window enumeration. Per WP-02: read `AvailableSourceTypes` and verify each stream's `source_type` (wlr serves WINDOW as MONITOR); screenshot via a ScreenCast frame where the Screenshot portal is absent (no `impl.portal.Access`); audio on a second ordinary `pw_context_connect`, never the portal fd | L | WP-02, WP-29 | todo |
| WP-B2 | screenshot: selector overlay, frozen frame, window/area/screen, quick preview, save/copy, recent captures | L | WP-B1 | todo |
| WP-B3 | screenshot editor (annotations, crop, redaction, backgrounds, pins) on cairo/GTK4 with `ScreenshotSupport` | L | WP-B2 | todo |
| WP-B4 | screenOCR + QR (bundled Tesseract + tessdata, ZXing-C++) and colorPicker (portal PickColor + magnifier) | M | WP-B2 | todo |
| WP-B5 | screenRecorder capture: video + system audio + mic via PipeWire, ffmpeg encode with the fallback chain libx264 → libopenh264 → h264_vaapi → mpeg4 (encoder decided by WP-04), pause/resume sync (reuse `RecorderSampleTiming`), timeline driven by capture timestamps because wlroots frame delivery is damage-driven (WP-02), floating controls | L | WP-B1, WP-A5 | todo |
| WP-B6 | screenRecorder editor and export (trim/cut/zoom/blur/overlays/GIF), presets | L | WP-B5 | todo |
| WP-B7 | mediaTools (ffmpeg, libvips, Tesseract) | M | WP-B4 | todo |
| WP-B8 | cameraPreview (portal Camera / v4l2 via GStreamer) | S | WP-B1 | todo |
| WP-B9 | brightness: logind backlight + DDC/CI through helper, OSD, custom shortcuts | M | WP-S1 | todo |
| WP-B10 | commandBar: reusable search core + GIO apps, settings panes, folder file search, scripts, math/units/dates/emoji/links, paste-at-cursor; AT-SPI app menus as an optional provider | L | WP-24 | todo |
| WP-B11 | radialMenu: overlay at pointer, profiles, MPRIS now playing, portal/relay triggers | M | WP-29, WP-24 | todo |
| WP-B12 | shelf: drop window, edge trigger, share via portal | M | WP-29 | todo |
| WP-B13 | Packages (re-imagined homebrew): PackageKit + Flatpak search/install/remove/upgrade | L | WP-22 | todo |
| WP-B14 | appUpdates: PackageKit/Flatpak updates + appcast parser + AppImageUpdate | M | WP-B13 | todo |
| WP-B15 | uninstaller: package/Flatpak/AppImage removal + XDG leftover sweep | M | WP-B13 | todo |
| WP-B16 | Share links for captures and feedback: off by default pending `PLAN.md` § 10 decision 4; endpoint configurable | S | WP-B2 | todo |

### Wave C: window management (per-compositor backends)

| ID | Feature(s) | Size | after | Status |
|---|---|---|---|---|
| WP-C1 | `WindowSystem` backends: X11 EWMH, `ext-foreign-toplevel-list` + `wlr-foreign-toplevel-management`, KWin scripting D-Bus, Hyprland/Sway IPC; capability flags per backend | L | WP-12, WP-20 | todo |
| WP-C2 | GNOME Shell extension (`vorssaint-bridge`) exposing window list/activate/move-resize/workspace and clipboard change/read/write over D-Bus, installed and updated from the Capabilities page, CI against the two latest Shell versions | L | WP-C1 | todo |
| WP-C3 | switcher: list, MRU order, search, simple mode, per-app rules, display filtering; previews from portal window streams where available | L | WP-C1, WP-B1, WP-24 | todo |
| WP-C4 | windowLayout keyboard snapping + display move on backends that can move/resize; edge-drag on X11 | L | WP-C1, WP-24 | todo |
| WP-C5 | autoQuit + workspace gestures for mouseButtonShortcuts | M | WP-C1 | todo |
| WP-C6 | fanControl via helper hwmon pwm, curves, watchdog | M | WP-S1, WP-A4 | todo |

### Wave D: input relay features (need the helper)

| ID | Feature(s) | Size | after | Status |
|---|---|---|---|---|
| WP-D1 | `InputRelay` in the helper: device discovery (udev), grab/re-emit, xkb mirroring, rule engine API over D-Bus, latency budget test | L | WP-S1, WP-03 | todo |
| WP-D2 | keyboardDebounce, mouseClickDebounce, scrollInverter, smoothScroll rules | M | WP-D1 | todo |
| WP-D3 | superKey (tap/hold, LED, layout tap action) | M | WP-D1 | todo |
| WP-D4 | textSnippets trigger + expansion (typing and paste paths) and snippet quick menu | L | WP-D1, WP-A8 | todo |
| WP-D5 | mouseButtonShortcuts, mouseNavigation remaps, app exceptions via focused `app_id` | M | WP-D1, WP-C1 | todo |
| WP-D6 | quitWindowProtection (Ctrl+Q / Ctrl+W hold, double press, modifier) | S | WP-D1, WP-C1 | todo |
| WP-D7 | cleaningMode (grab all keyboards + blackout overlay) | S | WP-D1, WP-29 | todo |
| WP-D8 | Relay fallback for global shortcuts where the portal is missing | S | WP-D1, WP-24 | todo |

---

## Phase 4: release readiness

| ID | Title | Size | after | Owner role | Status |
|---|---|---|---|---|---|
| WP-40 | Docs: `README` Linux section, `docs/linux/INSTALL.md`, `TROUBLESHOOTING` Linux cases, `PRIVILEGES.md` final, per-desktop support page generated from the triage matrix | M | all waves | Shell squad | todo |
| WP-41 | Branding: apply the distinct identity decided in `PLAN.md` § 10 (name, icon, app id, feed) | M | decision | Shell squad | todo |
| WP-42 | Release workflow: tag → AppImage + Flatpak bundle + zsync + checksums; signing with a project key | M | WP-P1, WP-P2, WP-P4 | Packaging/CI | todo |
| WP-43 | Full smoke matrix run on GNOME 48, Plasma 6, Sway, Hyprland, Xfce (X11) on Ubuntu, Fedora, Arch; defects filed as WPs | L | all waves | QA/reviewer | todo |
| WP-44 | Energy audit: every feature's idle cost measured (`powertop`, CPU wakeups), badges updated | M | WP-43 | QA/reviewer | todo |
| WP-45 | Merge `linux/main` into `main`; upstream contribution offer to vorssaint/vorssaint-utils | S | WP-43 | Lead | todo |

---

## Dependency sketch

```
Phase 0: WP-00 ─┬─ WP-10 → WP-11 → WP-12 ─┬→ WP-14, WP-15
         WP-01 ─┤                 └ WP-13   └→ WP-20 → WP-21 → WP-22 → Wave A
         WP-02 ─┼──────────────────────────────────→ WP-B1 → Wave B capture
         WP-03 ─┴→ WP-S1 → WP-D1 → Wave D                (WP-24, WP-29 gate B/C/D)
         WP-04 → WP-P1 → WP-P2, WP-P3, WP-P4          WP-C1 → Wave C
```

Critical path: WP-00 → WP-10 → WP-11 → WP-12 → WP-20 → WP-21 → WP-22 →
WP-24 → WP-C1 → WP-C3. Everything the user sees first (tray, panel,
monitor, mixer, keep awake, clipboard) is off the critical path after WP-22
and can ship as a first Linux preview at the end of Wave A.
