# Vorssaint on GNU/Linux: port plan

Status: proposal, September 2026, against `main` at 3.3.5.
Companion files: `FEATURE_TRIAGE.md` (per-feature mapping and verdicts),
`WORK_PACKAGES.md` (the executable backlog), `AGENT_PLAYBOOK.md` (how the
agent team runs it).

## 1. Summary

Vorssaint is a 222k-line Swift app (100k in services, 45k in SwiftUI views,
43k in core of which 25k are compiled-in translations, plus tests) whose
value is deep macOS integration: about twenty modifying CGEvent taps, the
Accessibility API, ScreenCaptureKit, CoreAudio process taps, IOKit and a
handful of private SkyLight/CGS symbols. None of that exists on Linux, and
Linux itself is not one platform for these purposes: GNOME, KDE Plasma,
wlroots compositors and X11 sessions each expose a different set of
capabilities for global shortcuts, window control, clipboard access and
overlays.

The plan is therefore not a "recompile". It is:

1. **Keep the core in Swift and share it.** Roughly 35 to 40 % of the
   service code and effectively all of the catalog, settings, presets and
   thirteen-language string tables are Foundation-only and compile on Linux
   with Swift 6.3 once `Combine` is shimmed. This is the single largest
   asset and the reason to stay on Swift rather than rewrite in another
   language: the Linux port can track upstream instead of forking it.
2. **Rewrite the platform layer per concern behind protocols**, with a
   Linux backend for each and capability flags so features degrade honestly
   per desktop session.
3. **Rewrite the UI in Qt 6 (Qt Quick / QML)**, driven by the Swift core
   through one generic state/command bridge rather than per-view bindings.
   The SwiftUI views are the specification.
4. **Ship one self-contained AppImage** with the full feature set (plus a
   tiny privileged helper for input interception, fan control and DDC), and
   a Flatpak with a documented reduced mode.
5. **Triage every feature** (`FEATURE_TRIAGE.md`): 32 port, 11 ship reduced,
   5 are re-imagined around Linux equivalents, 9 are dropped because they
   have no counterpart (Dock features, Finder features, `.dmg` installer, XDR
   brightness, Music blocker, middle-click emulation, window maximizer).

Delivery is in phases with gates: spikes (Phase 0), shared core (1), Linux
app shell (2), feature waves A to D (3), release (4). The first usable
preview (tray, panel, system monitor, mixer, keep awake, clipboard,
snippets-less) is reachable at the end of Wave A, well before the hard
window-management and input-relay waves.

## 2. Goals and non-goals

Goals

- One repository, one Swift core, two platform targets. Upstream macOS
  behaviour is unchanged by the port (the macOS CI gate proves it).
- Runs on any mainstream desktop distro from a single download, without
  distro packaging, on GNOME (Wayland), KDE Plasma 6 (Wayland), wlroots
  compositors (Sway, Hyprland) and X11 sessions.
- The same product idea: one tray icon, features installed one by one,
  honest energy and capability badges, no accounts, no telemetry.
- Every feature states which desktop sessions it supports, and the app
  explains what is missing on the running one instead of failing quietly.

Non-goals

- Feature parity with macOS where the desktop has no mechanism (see Drop in
  the triage).
- Supporting distros or desktops outside the four session types above in
  the first release (Xfce/MATE/Cinnamon on X11 fall under "X11" and get what
  X11 gets).
- Distro packages (deb/rpm/AUR) in the first release. The AppImage is the
  product; packages can follow from the same build.
- Porting the tests that exercise Apple frameworks (`RecorderWriterTests`
  and similar stay macOS-only).

## 3. Current state (what the inventory found)

Numbers are from `main` at 3.3.5.

| Layer | Files / lines | Portable share | Notes |
|---|---|---|---|
| `Sources/Vorssaint/Services` | 258 / 99.3k | ~35 % | Every feature has a `*Support.swift` with pure logic; the rest is AppKit, CGEvent, AX, IOKit, CoreAudio, SCK |
| `Sources/Vorssaint/UI` | 110 / 45.3k (232 views) | ~0 % | SwiftUI over AppKit-hosted `NSPanel`/`NSPopover`; full rewrite |
| `Sources/Vorssaint/Core` | 42.9k | ~95 % | `FeatureCatalog`, `Defaults` (616 keys), `GlobalShortcut`, `Localization` + 40 string structs + 11 locale files |
| `Sources/Vorssaint/App` | 5.4k | ~20 % | `AppDelegate` popover machinery, `StatusItemController`, `MenuBarRenderer`, `FeatureRuntime` (portable) |
| Tests | 9 files + `--selftest` | ~80 % | `swiftc`-compiled checks of pure helpers; no XCTest |

Mechanism clusters that decide the port:

- **Modifying event taps** (~20): every mouse/keyboard feature, quit
  protection, snippets, cleaning mode. Linux has no session-wide modifying
  tap; the only universal equivalent is an evdev grab with uinput
  re-injection, which needs privilege.
- **Accessibility API** for window list/move/resize/raise, Dock
  introspection and app-menu traversal. Linux splits this into
  per-compositor protocols and scripts (window control) and AT-SPI2 (menus,
  partial).
- **ScreenCaptureKit and CoreAudio taps**: map cleanly to the ScreenCast
  portal + PipeWire, which is simpler than the macOS path.
- **Private APIs** (CGS Spaces, SymbolicHotKeys, DisplayServices,
  IOAVService, IOBluetooth, MediaRemote, IOHIDEventSystemClient, SMC): each
  has a public Linux counterpart or is dropped; none is a blocker.
- **Global shortcuts**: Carbon hotkeys + taps today; on Linux the
  GlobalShortcuts portal (GNOME 48+, KDE, Hyprland), `XGrabKey` on X11, and
  the input relay everywhere else.

## 4. Decisions

Each decision names the alternative considered and the evidence. Phase 0
spikes confirm or revise the ones marked *to confirm*.

### 4.1 Language: Swift core on Linux (to confirm in WP-00)

Swift 6.3 ships official Linux toolchains; `Foundation` on Linux is
swift-foundation plus `FoundationNetworking` (`URLSession`) and
`FoundationXML`. Missing on Linux: AppKit, CoreGraphics beyond the
`CGFloat`/`CGRect` family, IOKit, `os.log`, and Combine.

- Combine is replaced by **OpenCombine** (active in 2026) behind a
  `VorssaintCombine` re-export module, or by migrating singletons to
  `@Observable` (the Observation module ships on Linux). WP-00 measures
  which costs less across 78 `ObservableObject`s.
- Static linking: `--static-swift-stdlib` for the GUI process (glibc stays
  dynamic and is bundled by the AppImage). The musl Static Linux SDK cannot
  `dlopen` or link distro shared libraries, so it is used only for the
  privileged helper, where zero dependencies is exactly what we want.
- Swift 6 strict concurrency meets Qt's main-thread rule: all UI-facing
  bridge calls are `@MainActor`, and the core keeps its "main thread only"
  convention.

Alternative: rewrite the core in Rust or C++. Rejected because it discards
the translations and the tested `Support` logic, and because a fork in a
different language can never take upstream fixes again. It stays as the
fallback in § 9 if WP-00 fails.

### 4.2 UI toolkit: Qt 6 Quick shell with the Swift core behind one bridge (to confirm in WP-01)

Evidence from the ecosystem research:

- Swift-native declarative toolkits (Adwaita for Swift, SwiftCrossUI) are
  active but unproven at this size; none has large-list virtualization,
  drag-and-drop or accessibility parity. Tokamak is archived. Qt Bridge for
  Swift (from Qt itself) is an early preview with unstable APIs.
- Qt 6 has what the shell needs out of the box: StatusNotifierItem,
  `layer-shell-qt`, portal integration, Qt Multimedia over PipeWire,
  Wayland and X11 platform plugins, and mature AppImage tooling
  (`linuxdeploy-plugin-qt`). GTK4/libadwaita has no AppImage plugin and
  looks foreign on KDE; Qt with the platform theme looks acceptable on both.
- Licence: Qt 6 under LGPL-3 is compatible with this project's
  GPL-3.0-or-later.

Design of the bridge (so agents do not write 232 view-specific bindings):

- The Swift core exposes, over a C ABI (`@_cdecl`), three calls:
  `subscribe(service, callback)`, `command(service, json)`, and
  `snapshot(service) -> json`. Each service publishes a `Codable` **state
  snapshot** (its `@Published` properties, already what SwiftUI observes)
  and accepts a `Codable` **command enum** (what the views call today).
- The Qt side has one generic `CoreModel` (`QObject` with a `QVariantMap`
  state and `invoke(command)`), instantiated per service and bound in QML.
  Typed wrappers are added only for hot paths (metrics sparklines, window
  previews as shared-memory frames).
- Snapshots are diffed on the Swift side so QML rebinding stays cheap; the
  bridge is tested with a fake service on both ends.

WP-01 bakes this off against Qt Bridge for Swift (whole app in Swift, QML
views) and Adwaita for Swift. If Qt Bridge is stable enough, it replaces the
C++ side of the bridge with the same design and fewer languages; the QML
stays identical, which is why it is a low-risk swap.

### 4.3 Packaging: AppImage primary, Flatpak secondary (to confirm in WP-04)

- AppImage: type2 runtime is static (no `libfuse2` requirement), the
  payload is built on the oldest supported glibc (Ubuntu 22.04, glibc
  2.35), Qt and plugins bundled by `linuxdeploy-plugin-qt`, `RUNPATH` not
  `LD_LIBRARY_PATH`, `QT_QPA_PLATFORM=wayland;xcb`. AppImageUpdate zsync
  gives self-update. A plain tarball with the same layout is the fallback
  for hosts that block FUSE.
- Flatpak: Flathub will not accept `--device=all` + `--filesystem=host` +
  `--socket=system-bus` for a utility, and even with them uinput does not
  work in the sandbox. The Flatpak build therefore has a **reduced mode**
  with no input relay, no fan control, no DDC, and portal-only capture and
  shortcuts. The hub states this on first launch.

### 4.4 Privilege: one helper, `vorssaint-helper`

A musl-static Swift daemon (systemd system unit, D-Bus system-bus name,
polkit actions, udev rules for `uinput`/`i2c`), installed from the
Capabilities page with one polkit prompt and uninstalled the same way. It
owns: evdev grab + uinput re-emit (the `keyd` model), hwmon `pwm` writes,
DDC/CI I2C writes. Everything else (backlight via logind `SetBrightness`,
inhibitors, UPower, BlueZ, PipeWire, portals) is unprivileged and lives in
the app. The API is the narrowest set of methods that the rules engine
needs; the security review is part of WP-S1 and the result is
`PRIVILEGES.md`.

Alternative rejected: adding the user to `input`/`uinput` groups. It makes
every process of that user a keylogger; the helper keeps that boundary.

### 4.5 Desktop capability model

At start-up the app probes and records: portal interfaces and versions
(ScreenCast, Screenshot, GlobalShortcuts, Settings, Inhibit, Background,
Clipboard), Wayland globals (`zwlr_layer_shell_v1`,
`ext_foreign_toplevel_list_v1`, `zwlr_foreign_toplevel_manager_v1`,
`ext_data_control_manager_v1`, `zwlr_data_control_manager_v1`,
`ext_image_copy_capture_manager_v1`), `org.kde.StatusNotifierWatcher`,
KWin scripting, our GNOME extension, X11, helper presence, PipeWire
presence, `/dev/uinput` and `/dev/i2c-*` access. `FeatureCatalog` gains
`requiredCapabilities` per feature; the hub, the Capabilities page and the
energy badges render from it. This replaces the macOS Permissions model.

### 4.6 GNOME companion extension

GNOME (Mutter) implements no layer-shell, no foreign-toplevel protocols and
no data-control, and its tray needs the AppIndicator extension. Window
switcher, layout, auto-quit and clipboard history on GNOME therefore go
through a small GNOME Shell extension (`vorssaint-bridge`, D-Bus interface
on the session bus) that the Capabilities page installs and updates, with
per-GNOME-version compatibility as an ongoing cost. Without it these
features show as unavailable on GNOME; nothing else depends on it.

### 4.7 Branding

`TRADEMARKS.md` requires unofficial builds to carry a different name, icon,
app id, signing identity and update feed. The Linux port from this fork is
an unofficial build until the upstream maintainer says otherwise. See § 10.

## 5. Target architecture

```
Sources/
  VorssaintCore/          Foundation-only. Catalog, presets, defaults keys,
                          localization, every *Support and model file,
                          feature runtime, settings store protocol,
                          Platform/ protocols with capability flags.
  VorssaintMac/           Today's AppKit/IOKit/SCK/CoreAudio services and
                          SwiftUI views, calling the core through the
                          Platform protocols (adapter written in WP-12).
  Vorssaint/main.swift    macOS executable (unchanged entry point).
  VorssaintLinux/         Linux backends: Portals, Wayland, X11, PipeWire,
                          Sensors, Logind, UPower, BlueZ, PackageKit,
                          Flatpak, HelperClient, GnomeBridgeClient,
                          KWinScriptClient, CompositorIPC. CoreBridge
                          (@_cdecl surface).
  VorssaintHelper/        musl-static privileged daemon: InputRelay,
                          Hwmon, DDC. D-Bus + polkit.
linux/
  shell/                  Qt 6 Quick app: CoreModel bridge, QML screens
                          (one file per SwiftUI view it replaces), tray,
                          panel, overlays, capture UI, editors.
  gnome-extension/        vorssaint-bridge GNOME Shell extension.
  kwin-script/            window control script loaded over D-Bus.
  packaging/              AppImage recipe, Flatpak manifest, desktop file,
                          AppStream metadata, udev rules, polkit policy,
                          systemd unit.
docs/linux-port/          this plan, triage, backlog, playbook, spikes,
                          PRIVILEGES.md.
```

Rules of the architecture:

- Services keep their singleton + `syncWithPreferences()` shape and their
  `Support` split. A Linux service is the macOS service with the platform
  calls replaced by a `Platform` protocol call; where the macOS service is
  too entangled, the Linux service is written fresh against the same
  `Support` file.
- `FeatureRuntime` bindings become per-platform tables over the same
  catalog.
- Strings stay compiled-in `Strings` structs. Linux-only strings are added
  to the same structs (all thirteen languages, compiler-enforced).
- Settings keys are the same `DefaultsKey` names in a JSON store under
  `$XDG_CONFIG_HOME`, so a settings backup moves across platforms.

## 6. Linux building blocks (evidence base)

The research report behind these rows is in the session record; items the
researcher could not verify are marked. Squads re-verify on their target
versions as part of each work package.

| Concern | Mechanism | GNOME | KDE 6 | wlroots / Hyprland | X11 | Privilege |
|---|---|---|---|---|---|---|
| Tray | StatusNotifierItem D-Bus | AppIndicator extension needed (Ubuntu ships it) | native | native (waybar etc.) | native | none |
| Panel/overlay placement | `zwlr_layer_shell_v1` via `layer-shell-qt` | ✗ (fullscreen transparent window fallback) | ✓ | ✓ | override-redirect | none |
| Global shortcuts | portal GlobalShortcuts | ✓ 48+ | ✓ | Hyprland ✓, Sway ✗ (relay) | `XGrabKey` | none / helper |
| Mouse-button triggers | evdev relay | ✓ | ✓ | ✓ | `XGrabButton` or relay | helper |
| Window list/focus/close | foreign-toplevel / KWin script / extension / EWMH | extension | KWin script | ✓ protocol | ✓ | none |
| Move/resize windows | extension / KWin script / IPC / EWMH | extension | KWin script | Hyprland+Sway IPC | ✓ | none |
| Live window previews | portal ScreenCast window source; `ext-image-copy-capture` | ✓ portal | ✓ portal | Hyprland ✓; wlroots 0.19 via ext protocol (unverified matrix) | XComposite | none |
| Screen/region capture | portal ScreenCast + Screenshot, restore tokens | ✓ | ✓ | ✓ (monitor; window depends on backend) | ✓ | none |
| Encoding | bundled ffmpeg libs (openh264 or VA-API/NVENC to avoid GPL x264 in the AppImage) or GStreamer | – | – | – | – | none |
| System audio / mic | PipeWire monitor node / source node; libpulse fallback | ✓ | ✓ | ✓ | ✓ | none |
| Per-app volume/routing | PipeWire `Props.volume`, metadata `target.object` | ✓ | ✓ | ✓ | ✓ | none |
| Clipboard in background | `ext-data-control` / `wlr-data-control`; `XFixes`; portal Clipboard in RemoteDesktop session | extension or portal session | ✓ | ✓ | ✓ | none |
| Paste at cursor | relay Ctrl+V; portal RemoteDesktop keyboard (libei) | portal or relay | portal (verify) or relay | relay | `XTest` | helper or portal |
| Sensors | `/proc`, `/sys/class/hwmon`, UPower, NVML (dlopen), amdgpu/i915 sysfs | ✓ | ✓ | ✓ | ✓ | none |
| Fan control | hwmon `pwm*` writes | ✓ | ✓ | ✓ | ✓ | helper |
| Backlight | logind `SetBrightness` | ✓ | ✓ | ✓ | ✓ | none |
| External DDC | `/dev/i2c-*` (ddcutil udev rule) | ✓ | ✓ | ✓ | ✓ | udev rule via helper |
| Display power | Mutter DisplayConfig, KWin D-Bus, `wlr-output-power-management`, DPMS | ✓ | ✓ | ✓ | ✓ | none |
| Keep awake | logind `Inhibit` / portal Inhibit | ✓ | ✓ | ✓ | ✓ | none |
| Dark mode | portal Settings (read); gsettings / `plasma-apply-colorscheme` (write) | ✓ | ✓ | ◐ | ◐ | none |
| Notifications | `org.freedesktop.Notifications` | ✓ | ✓ | ✓ (daemon needed) | ✓ | none |
| Autostart | XDG autostart; portal Background under Flatpak | ✓ | ✓ | ◐ (manual exec) | ✓ | none |
| OCR / QR | Tesseract + bundled `eng` tessdata; zxing-cpp C API | ✓ | ✓ | ✓ | ✓ | none |
| Packages | PackageKit D-Bus, libflatpak, Homebrew-on-Linux if present | ✓ | ✓ | ✓ | ✓ | polkit prompts by PackageKit |
| Bluetooth on sleep | BlueZ D-Bus + logind delay inhibitor | ✓ | ✓ | ✓ | ✓ | none |
| Headless CI | Xvfb for X11; `weston --backend=headless` or sway headless for Wayland + `xdg-desktop-portal-wlr`; `python-dbusmock` for portals | – | – | – | – | – |

## 7. Dependencies

Per `CONTRIBUTING.md` the project has no external dependencies. The port
cannot hold that line, so the rule becomes: every dependency is listed here
with what carries it and how it ships.

| Dependency | Why | Ships how | If missing |
|---|---|---|---|
| Swift 6.3 toolchain (build time) | core | CI container | – |
| OpenCombine | Combine on Linux | SwiftPM, static | – |
| Qt 6 (Core, Gui, Quick, QuickControls2, DBus, Multimedia, WaylandClient), `layer-shell-qt` | shell | bundled in AppImage; KDE runtime in Flatpak | – |
| libpipewire, libpulse | audio, capture frames | bundled; falls back to libpulse | audio features off |
| ffmpeg libs (libavcodec/format/filter, openh264) | recording, media tools, GIF | bundled | recorder/media off |
| Tesseract + Leptonica + `eng` tessdata | OCR | bundled; more languages downloaded on demand | OCR off |
| zxing-cpp | QR | bundled static | QR off |
| libxkbcommon, libudev, libevdev | input relay, layouts | helper links statically where licence allows, else bundled | relay off |
| libddcutil (optional) | external brightness | dlopen if present, else our own I2C path | DDC off |
| NVML (optional) | NVIDIA GPU metrics | dlopen from driver | GPU rows hidden |
| PackageKit, Flatpak (optional, runtime D-Bus/CLI) | Packages, updates, uninstaller | host | those rows hidden |
| GNOME Shell extension (ours) | GNOME window and clipboard features | installed from the app | features hidden on GNOME |

No dependency is added beyond this table without a line here first.

## 8. Phases and gates

| Phase | Content | Exit gate |
|---|---|---|
| 0 Spikes | WP-00 to WP-04: Swift core on Linux, toolkit bake-off, portal capture, input relay, packaging | Written go/no-go per spike; § 4 decisions confirmed or revised |
| 1 Shared core | Package split, file moves, Platform protocols, Combine shim, settings store, catalog flags, `swift test` on both platforms, Linux CI leg | Core builds and tests on both; macOS app behaviour unchanged (selftest + ui-smoke) |
| 2 Linux shell | Executable, tray, panel, settings/hub/onboarding, shortcuts, capabilities page, notifications/autostart, icons, theming, overlays, helper, AppImage, Flatpak, headless CI | AppImage launches on clean Ubuntu, Fedora and Arch/KDE; tray, panel, settings, shortcut recording, autostart and hub install/uninstall work |
| 3 Wave A | Monitor, power, GPU, temps, mixer, keep awake, bluetooth sleep, clipboard, scratchpad, launcher, kill process, quick toggles, cleaner, DE setting writers, readouts | First public preview: green smoke matrix on GNOME, KDE, Sway |
| 3 Wave B | Capture engine, screenshot + editor, OCR/QR/color, recorder + editor, media tools, camera, brightness, command bar, radial menu, shelf, packages, app updates, uninstaller, sharing | Second preview |
| 3 Wave C | Window backends, GNOME extension, switcher, layout, auto-quit, fan control | Third preview |
| 3 Wave D | Input relay features: debounce, scroll, super key, snippets, mouse buttons, quit protection, cleaning mode, relay shortcut fallback | Feature complete |
| 4 Release | Docs, branding, release workflow, full smoke matrix, energy audit, merge to `main`, upstream offer | 1.0 of the Linux build |

Wave A can start as soon as WP-22 (panel) lands, in parallel with the rest
of Phase 2, which is how the preview arrives early. Waves B, C and D are
gated by WP-24/WP-29 (shortcuts, overlays), WP-C1 (window backends) and
WP-S1/WP-D1 (helper, relay) respectively, and otherwise run in parallel.

## 9. Risks and fallbacks

| Risk | Signal | Mitigation / fallback |
|---|---|---|
| Swift core does not compile cleanly on Linux (Foundation gaps, Combine) | WP-00 error census | OpenCombine or `@Observable`; if the census shows more than ~15 % of core files need rework, fall back to a Rust core with the string tables mechanically converted, and re-size Phase 1 |
| Qt Bridge for Swift too unstable, C++ bridge too heavy | WP-01 | The generic snapshot/command bridge is small by design (three C functions); the C++ side is under 2k lines |
| GNOME features depend on an extension that breaks per GNOME release | GNOME release cadence | Extension kept minimal (D-Bus facade over `Meta`), CI runs it against the two latest Shell versions, features hide when it is absent |
| Portal GlobalShortcuts missing on Sway/river | probe | Relay fallback via the helper; documented |
| AppImage + Qt breaks on some distro (glibc, OpenSSL, theme plugins) | smoke matrix | Build on glibc 2.35, bundle Qt platform themes, avoid bundling OpenSSL, keep the tarball fallback |
| Privileged helper is a security surface | design review | Narrow D-Bus API, polkit per action, musl-static, no shell-outs, `PRIVILEGES.md`, reviewer sign-off required |
| Encoder licensing (x264 is GPL, fine for a GPL app but not for every downstream) | packaging | Prefer openh264 and hardware encoders; document |
| PR size and review load with many agents | playbook | ~800-line cap, one WP per PR, QA role, weekly rebase |
| Trademark | § 10 | Rename before any public build |

## 10. Open decisions for the project owner

These are not blockers for Phases 0 and 1, which touch no branding and no
public distribution, but they must be settled before Phase 2 ships an
artifact.

1. **Name and identity of the Linux build.** `TRADEMARKS.md` requires a
   distinct name, icon, app id and update feed for unofficial builds.
   Options: ask the upstream maintainer for permission to ship as
   "Vorssaint for Linux" from this fork, or pick a new identity now (the
   backlog carries WP-41 for the rename either way).
2. **Upstream relationship.** Offer Phase 1 (the core split) upstream
   early: it is platform-neutral, reduces the fork's divergence to the
   Linux directories, and the upstream contribution guide asks for exactly
   that kind of small, reviewed change.
3. **Flatpak on Flathub or self-hosted.** Flathub means reduced mode and
   review; self-hosted repo keeps the full permission set.
4. **Share and feedback endpoints.** The macOS app posts captures and
   feedback to `screenshots.vorssaint.com`; an unofficial build should not
   use the project's endpoints without agreement. Default in the plan: those
   two features are off in the Linux build until decided.
5. **Minimum baselines.** Proposed: GNOME 46+, Plasma 6.0+, wlroots 0.18+,
   PipeWire 1.0+, glibc 2.35+, kernel 5.15+. GlobalShortcuts on GNOME needs
   48+, so GNOME 46/47 use the relay fallback.

## 11. How this plan is executed

`AGENT_PLAYBOOK.md` describes the roles, dispatch loop, branch layout,
conventions, definition of done and gates. `WORK_PACKAGES.md` is the
backlog in dependency order with acceptance criteria per package; the lead
dispatches from it, records status in it, and never runs more parallel
squads than there are unblocked packages.
