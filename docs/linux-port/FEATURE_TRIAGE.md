# Linux port: feature triage

Every feature in `AppFeature` (`Sources/Vorssaint/Core/FeatureCatalog.swift`),
what it stands on today, what it would stand on under Linux, and the verdict.
This is the file a feature squad reads before touching a work package, and the
file the QA role keeps truthful as support changes.

Sizes are the lines of the feature's service directory as of `main` at
3.3.5 (UI excluded). "Reusable" is the estimated share of that service that
is Foundation-only logic (the `*Support.swift` files and models) and moves
to `VorssaintCore` unchanged.

## Verdicts

| Verdict | Meaning |
|---|---|
| **Port** | Same behaviour, new backend. Reuses the `Support` logic. |
| **Reduced** | Ships, but with a documented subset that depends on the desktop session. The hub explains what is missing on the running desktop. |
| **Re-imagine** | Same user need, different mechanism; UI copy and settings change. |
| **Drop** | No Linux counterpart. Hidden from the hub on Linux. |

## Desktop support legend

G = GNOME on Wayland (Mutter), K = KDE Plasma 6 on Wayland (KWin),
W = wlroots-based (Sway, Hyprland, river) on Wayland, X = any X11 session.
✓ supported, ◐ reduced, ✗ not possible without a compositor extension.
The research findings behind each column are in `PLAN.md` § 6; anything listed there as unverified is marked with `?` here until a
Phase 0 spike settles it.

## Windows and Dock

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| switcher | 7.7k / 28% | CGEvent tap, CGWindowList, AX raise/close/minimize, SCK previews, private CGS Spaces | Toplevel list + activate/close/minimize over `wlr-foreign-toplevel-management` / `ext-foreign-toplevel-list-v1` (wlroots, Hyprland, COSMIC); a KWin script we load over D-Bus (KWin does not expose those protocols to arbitrary clients); our GNOME Shell extension; EWMH on X11. Live previews: portal ScreenCast window streams (GNOME, KDE, Hyprland), `ext-image-copy-capture-v1` toplevel source (wlroots 0.19), XComposite on X11. Hotkey: GlobalShortcuts portal (GNOME 48+, KDE, Hyprland) or relay. | ◐ (our extension; previews via portal) | ✓ (KWin script) | ✓ | ✓ | Reduced | C |
| dockPreview | 2.4k / 25% | AX hit-test of Dock tiles, CGWindowList, SCK | There is no Dock. Nearest: hover previews on the panel's task list (KDE already has them, GNOME dash needs an extension). | ✗ | n/a | n/a | n/a | Drop | – |
| dockClick | 1.3k / 22% | CGEvent tap on Dock clicks, AX minimize/raise | Same; the panel owns its clicks. | ✗ | ✗ | ✗ | ✗ | Drop | – |
| windowMaximizer | 0.5k / 15% | Tap on title-bar clicks, AX zoom button | Compositors own title bars and double-click-to-maximize is a native setting on all four. | – | – | – | – | Drop (point to the DE setting) | – |
| windowLayout | 3.9k / 38% | AX position/size set, Carbon hotkeys, 3 taps for drag/snap | Keyboard snapping: no Wayland protocol moves another client's window; KWin script (`window.frameGeometry`), our GNOME extension (`Meta.Window.move_resize_frame`), Hyprland `hyprctl dispatch` / Sway IPC, X11 EWMH `_NET_MOVERESIZE_WINDOW`. Edge-drag snapping: native in all DEs, ours only on X11. Display move: same channels. | ◐ (our extension) | ✓ (KWin script) | ✓ (IPC) | ✓ | Reduced | C |
| autoQuit | 1.2k / 20% | AXObserver window lifecycle, NSWorkspace, listen tap | Toplevel list events (closed/opened) + `SIGTERM` to the owning pid (`app_id` → pid via `/proc` or `org.freedesktop.application` D-Bus); X11 `_NET_WM_PID`. | ◐ (our extension) | ✓ | ✓ | ✓ | Reduced | C |
| quitWindowProtection | 0.7k / 30% | Modifying CGEvent tap on ⌘Q/⌘W | evdev → uinput interception in `vorssaint-helper` (Ctrl+Q, Ctrl+W hold/double press), per-app rule needs the focused `app_id` from the toplevel list. | ◐ | ✓ | ✓ | ✓ | Port (via input helper) | D |

## Mouse and keyboard

All modifying features here become one shared component: `InputRelay`, an
evdev grab of the physical devices with re-injection through a uinput virtual
device, running inside `vorssaint-helper` (root or `input` group via udev
rule). This is desktop-independent, works on Wayland and X11 alike, and is the
same architecture as `keyd`, `evsieve` and `input-remapper`. The security
design is WP-S1. The listen-only features use the same relay in tap mode.

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| scrollInverter | 0.2k / 15% | Tap negating scroll deltas | InputRelay: flip `REL_WHEEL`/`REL_HWHEEL`/`REL_WHEEL_HI_RES` per device type (mouse vs touchpad distinguished by udev `ID_INPUT_TOUCHPAD`). libinput natural scroll exists per device natively; ours adds separate axis control. | ✓ | ✓ | ✓ | ✓ | Port | D |
| focusFollowsMouse | 0.3k / 30% | Global mouse monitor + AX window under pointer | Native setting on X11 WMs and KWin ("focus follows mouse"), GNOME `org.gnome.desktop.wm.preferences focus-mode`, Sway `focus_follows_mouse`. Ours: write the DE setting with the chosen delay where the DE supports one. | ✓ (setting) | ✓ (setting) | ✓ (setting) | ✓ | Re-imagine (setting switch, not our loop) | A |
| smoothScroll | 0.7k / 30% | Tap re-posting interpolated wheel events | InputRelay: consume `REL_WHEEL`, emit `REL_WHEEL_HI_RES` interpolation with the existing easing math. | ✓ | ✓ | ✓ | ✓ | Port | D |
| mouseAcceleration | 0.7k / 35% | Private IOHIDEventSystemClient property | libinput "flat" acceleration profile per device: GNOME `org.gnome.desktop.peripherals.mouse accel-profile`, KDE kcminput / `kwriteconfig6`, Sway/Hyprland config, X11 `xinput set-prop … "libinput Accel Profile Enabled"`. Recovery logic reused. | ✓ | ✓ | ✓ | ✓ | Re-imagine (DE setting writer) | A |
| mouseNavigation | 0.6k / 40% | Tap on side buttons + AX menu traversal | Side buttons already navigate in every Linux browser/file manager. Keep only the "remap to app-specific keys" part via InputRelay. | ✓ | ✓ | ✓ | ✓ | Reduced | D |
| mouseButtonShortcuts | 1.0k / 40% | Tap on extra buttons + synthesized combos | InputRelay maps `BTN_SIDE`/`BTN_EXTRA`/`BTN_TASK` and hi-res tilt to key chords; "Spaces gestures" become workspace switch via the compositor (portal has no API; KWin D-Bus, Hyprland/Sway IPC, GNOME shell ext., X11 `_NET_CURRENT_DESKTOP`). | ◐ | ✓ | ✓ | ✓ | Port | D |
| middleClick | 0.8k / 18% | Tap + multitouch device | libinput already has `middle-emulation` and three-finger tap on touchpads is configurable in every DE. | – | – | – | – | Drop (point to DE setting) | – |
| mouseClickDebounce | 0.4k / 32% | Modifying tap on button events | InputRelay: `BTN_LEFT/RIGHT/MIDDLE` timing window with the existing `MouseClickDebounceSupport`. | ✓ | ✓ | ✓ | ✓ | Port | D |
| keyboardDebounce | 0.5k / 45% | Modifying tap on keyDown/keyUp | InputRelay: per-keycode chatter filter with `KeyboardDebounceSupport`. | ✓ | ✓ | ✓ | ✓ | Port | D |
| textSnippets | 1.4k / 62% | Tap reading unicode + synthesized typing / paste | Trigger detection through InputRelay (keycode → xkb keysym via libxkbcommon with the active layout); expansion by typing through uinput (xkb keymap of the virtual device) or clipboard-paste (Ctrl+V) with save/restore where a data-control protocol exists. IME text is not seen (documented). | ✓ | ✓ | ✓ | ✓ | Port | D |
| superKey | 1.5k / 35% | Two taps + IOHID caps-lock state | InputRelay: tap-vs-hold on `KEY_CAPSLOCK`/right modifiers, LED via `EV_LED` on the device; tap actions (layout switch) call the DE (`org.gnome.desktop.input-sources`, KDE `kded_keyboard`, xkb group). | ✓ | ✓ | ✓ | ✓ | Port | D |

## Clipboard and files

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| clipboardHistory | 2.4k / 40% | NSPasteboard change-count polling | `ext-data-control-v1` / `wlr-data-control-unstable-v1` (background clipboard access without focus) on KWin 6.x, wlroots, Hyprland, COSMIC; X11 `XFixes` selection-owner-change. GNOME has neither: our GNOME Shell extension (`Meta.Selection`, the GPaste model) or the portal `Clipboard` interface inside a RemoteDesktop session with a one-time permission dialog. Source-app attribution is unavailable on Wayland. | ◐ (extension) | ✓ | ✓ | ✓ | Reduced | A |
| pastePlain | 0.2k / – | Rewrites pasteboard + ⌘V | Same data-control protocol + Ctrl+V via InputRelay or portal RemoteDesktop keyboard injection (libei). | ◐ | ✓ | ✓ | ✓ | Reduced | A |
| finderCutPaste | 0.8k / 20% | Finder AppleScript + tap | Nautilus, Dolphin, Thunar and Nemo already cut/paste files with Ctrl+X/Ctrl+V. | – | – | – | – | Drop | – |
| finderRename | – | F2 in Finder | F2 renames in every Linux file manager. | – | – | – | – | Drop | – |
| shelf | 3.1k / 30% | Drag pasteboard polling, NSPanel, NSSharingService | Qt drop-target window (Wayland DnD, XDND on X11); no way to observe a drag that starts in another app on Wayland, so the "appears mid-drag" trigger becomes an edge/shortcut trigger only. Share: portal `OpenURI`/`Email`, or copy path. | ◐ | ◐ | ◐ | ✓ | Reduced | B |
| urlCleaner | 0.3k core / 95% | Pasteboard read/write | Same clipboard backend as history. Logic is 100% reusable. | ◐ | ✓ | ✓ | ✓ | Port | A |
| diskImageInstaller | 0.5k / 15% | hdiutil, spctl, ditto | No `.dmg` concept. Nearest: "drop an AppImage → integrate into `~/.local/bin` + `.desktop`", a different feature. | – | – | – | – | Drop (candidate for a later AppImage integrator) | – |

## Sound

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| mixer | 4.1k / 30% | CoreAudio process taps + private aggregate device | PipeWire: per-stream `Props.volume` on `Stream/Output/Audio` nodes via `libpipewire` (or libpulse sink-input API, which PipeWire serves too). Volume above 100 % is native (measured: `channelVolumes: [1.5, 1.5]` accepted unclamped). Hide apps: same logic. Much simpler than macOS. Backend built and measured, WP-A5: `linux/platform/audio`, `docs/linux-port/AUDIO_BACKEND.md`. | ✓ | ✓ | ✓ | ✓ | Port | A |
| soundOutputSwitcher | (in Audio) | Default device property + headphone detection | WirePlumber default sink metadata (`default.audio.sink`), per-stream `target.object` for per-app output routing, `pw-link`/node listener for headphone disconnect. Backend built and measured, WP-A5: the disconnect arrives as `default-sink-disconnected` naming the sink that left, before WirePlumber's fallback. | ✓ | ✓ | ✓ | ✓ | Port | A |
| micMute | 0.5k | CoreAudio input mute | Mute every `Audio/Source` node via PipeWire; monitors excluded, and the prior mute state is remembered under `$XDG_RUNTIME_DIR` so restoring leaves alone what the user muted. Backend built and measured, WP-A5. | ✓ | ✓ | ✓ | ✓ | Port | A |
| musicBlock | 0.2k | Music.app launch suppression | No DE auto-launches a player on headphone connect. | – | – | – | – | Drop | – |

## Energy and display

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| keepAwake | 0.9k / 40% | IOPMAssertion, pmset + sudoers for clamshell | `org.freedesktop.login1.Manager.Inhibit` (`sleep`, `idle`, `handle-lid-switch`) over the system bus, no root; portal `Inhibit` as fallback. External-display / power-source automations from UPower and the toplevel/output list. | ✓ | ✓ | ✓ | ✓ | Port | A |
| brightness | 2.1k core / 25% | Private DisplayServices, IOAVService DDC | Internal: logind `SetBrightness` on `/sys/class/backlight` (no root). External: DDC/CI over `/dev/i2c-*` via `ddcutil` library or our own I2C writes through `vorssaint-helper` (udev rule for `i2c` group). Keyboard key follow-pointer needs the output under the pointer (compositor-specific; X11 `xrandr`). | ✓ | ✓ | ✓ | ✓ | Port | B |
| extraBrightness | 0.5k / 10% | Metal EDR layer on XDR panels | No EDR compositing model on Linux. | – | – | – | – | Drop | – |
| bluetoothSleep | 0.2k / 20% | Private IOBluetooth toggle | BlueZ `org.bluez.Adapter1.Powered` + logind `PrepareForSleep`. `BluetoothSleepSupport` reused verbatim. | ✓ | ✓ | ✓ | ✓ | Port | A |

## Tools

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| quickLauncher | 0.5k | NSWorkspace app list | `.desktop` files via GIO `GAppInfo`, launch through `GAppLaunchContext` / portal. | ✓ | ✓ | ✓ | ✓ | Port | A |
| quickToggles | 0.4k / 20% | osascript, SkyLight dark mode, SACLockScreen | Dark mode: portal `Settings` read + `gsettings color-scheme` / KDE `plasma-apply-colorscheme`; lock: `org.freedesktop.login1.Session.Lock` or `loginctl lock-session`; eject: UDisks2; empty trash: GIO trash; keyboard light: UPower `KbdBacklight`; hidden files / desktop icons: file-manager setting (`org.gtk.Settings.FileChooser show-hidden`, Dolphin config) or dropped. | ✓ | ✓ | ◐ | ✓ | Reduced | A |
| colorPicker | 0.1k | NSColorSampler + shared selector | Portal `Screenshot.PickColor` (works everywhere, permission-free) plus our magnifier on a portal screenshot. **Measured by WP-B1**: `PickColor` lives on the `Screenshot` interface, which a bare wlroots session does not export, so the magnifier there reads its pixels from a WP-B1 ScreenCast frame instead. | ✓ | ✓ | ◐ (colour from a ScreenCast frame; no `PickColor`) | ✓ | Port | B |
| screenOCR | 0.1k + Vision | VNRecognizeTextRequest | Tesseract (bundled `libtesseract` + `eng` and per-language `tessdata`), QR via ZXing-C++. | ✓ | ✓ | ✓ | ✓ | Port | B |
| cleaningMode | 0.4k / 30% | Tap swallowing all input + shield window | InputRelay grab of all keyboards (EVIOCGRAB) + a fullscreen layer-shell/fullscreen window per output. | ✓ | ✓ | ✓ | ✓ | Port | D |
| mediaTools | 2.3k / 45% | avconvert, AVAssetWriter, Vision, ImageIO | `ffmpeg` (bundled static build or system) for video/GIF, libvips or GdkPixbuf for images, Tesseract for text. | ✓ | ✓ | ✓ | ✓ | Port | B |
| cleaner | 1.2k / 70% | ~/Library path catalog | XDG catalog: `~/.cache`, `~/.local/share/Trash`, `~/.var/app/*/cache` (Flatpak), journald vacuum through `journalctl --user`, thumbnails cache. Messaging downloads: same logic on `~/Downloads`, origin from `user.xdg.origin.url` xattr. | ✓ | ✓ | ✓ | ✓ | Port | A |
| uninstaller | 1.6k / 45% | Spotlight leftovers, receipts, brew | Package-managed apps: PackageKit `RemovePackages`; Flatpak: `flatpak uninstall --delete-data`; AppImage: delete file + `.desktop`; leftovers under `~/.config`, `~/.cache`, `~/.local/share`, `~/.var/app`. | ✓ | ✓ | ✓ | ✓ | Re-imagine | B |
| homebrew | 1.7k / 55% | brew CLI, Terminal fallback | Re-imagined as **Packages**: PackageKit D-Bus (apt/dnf/pacman/zypper behind one API), Flatpak (`libflatpak`), and optionally Homebrew on Linux if present. Command builder and parsers reused. | ✓ | ✓ | ✓ | ✓ | Re-imagine | B |
| appUpdates | 1.7k / 50% | mdfind, MAS lookup, Sparkle appcasts, brew | PackageKit `GetUpdates`, Flatpak remote updates, AppImageUpdate zsync for AppImages; appcast parser reused for apps that publish one. | ✓ | ✓ | ✓ | ✓ | Re-imagine | B |
| screenshot | ~9k / 52% | SCK, per-screen NSPanel overlay, NSPasteboard | Portal `ScreenCast`/`Screenshot` (interactive false) for the frozen frame, our own selection overlay drawn on a fullscreen surface per output (layer-shell where available, fullscreen window on GNOME), editor rewritten in Qt Quick/QPainter with the reusable `ScreenshotSupport` model, clipboard via `QClipboard`. Window capture from the portal's window source. Scrolling capture: stitch logic reused, frames from repeated screenshots. **Measured by WP-B1** (`CAPTURE_ENGINE.md`): a bare wlroots session exports no `Screenshot` portal at all (no `impl.portal.Access`), so the frozen frame comes from a one-frame ScreenCast there (59 ms, same pixels); and it has no window source, so "capture this window" is hidden rather than offered. | ✓ | ✓ | ◐ (screen and area; no window capture; screenshot via ScreenCast) | ✓ | Port | B |
| cameraPreview | 0.4k | AVCaptureSession | Portal `Camera` → PipeWire stream → Qt Multimedia / GStreamer `pipewiresrc`; or v4l2 directly outside Flatpak. **Measured by WP-B1**: the `Camera` portal is behind the same `impl.portal.Access` gate as `Screenshot`, so a bare wlroots session exports none and v4l2 is the only route there. | ✓ | ✓ | ◐ (v4l2 directly; no portal Camera) | ✓ | Port | B |
| radialMenu | 2.4k / 50% | Tap on mouse buttons, NSPanel at pointer, MediaRemote | Trigger via GlobalShortcuts portal or InputRelay button; window at pointer position (layer-shell where available; on GNOME a fullscreen transparent window because clients cannot position surfaces); now-playing via MPRIS (`org.mpris.MediaPlayer2`). | ◐ | ✓ | ✓ | ✓ | Port | B |
| scratchpad | 0.9k / 60% | NSPanel | Plain Qt Quick window with keep-above hint; Markdown preview through a small md → Pango markup renderer. | ✓ | ✓ | ✓ | ✓ | Port | A |
| commandBar | 8.5k / 60% | NSMetadataQuery, AX menu traversal, ⌘V synthesis | Search/math/units/dates/emoji/links logic reused. Apps via GIO; settings panes via `gnome-control-center` / `systemsettings` module names; files via Tracker/`locate`/own walk of chosen folders; app menus via AT-SPI2 (works for GTK/Qt apps that export menus, degraded otherwise); paste-at-cursor via InputRelay Ctrl+V. | ◐ | ✓ | ✓ | ✓ | Reduced | B |
| screenRecorder | 9.8k / 45% | SCStream, AVAssetWriter, process taps, CGS cursor | Portal `ScreenCast` (screen/window/region-cropped) → PipeWire video; system audio from a PipeWire monitor node; mic from a source node; encode with bundled ffmpeg libs (H.264 via libx264 or VA-API); editor rewritten in Qt Quick with the reusable timeline/motion/blur models; cursor sprites from the compositor's cursor theme (XCursor). **Measured by WP-B1**: no portal has a region source, so the region is a crop the capture engine performs; on wlroots there is no window source and frame delivery is damage-driven, so the timeline is driven by capture timestamps. | ✓ | ✓ | ◐ (screen and region; no window recording) | ✓ | Port | B |
| killProcess | 0.6k / 60% | ps, kill, libproc | `/proc` + `kill(2)`. | ✓ | ✓ | ✓ | ✓ | Port | A |

## System monitor

| Feature | Size / reusable | macOS mechanism | Linux mechanism | G | K | W | X | Verdict | Wave |
|---|---|---|---|---|---|---|---|---|---|
| monitorCPU | (3.2k+2.2k / 40%) | host_statistics, SMC temps | `/proc/stat`, hwmon `temp*_input` (k10temp, coretemp, zenpower), `/proc/<pid>/stat` for per-process. | ✓ | ✓ | ✓ | ✓ | Port | A |
| monitorGPU | | IORegistry GPU counters | amdgpu sysfs `gpu_busy_percent`, `mem_info_*`; NVIDIA via NVML `dlopen`ed, never linked; Intel publishes **no** busy percentage without `CAP_PERFMON`, so the panel shows the GT clock and says why. Measured vendor matrix: `SENSORS_BACKEND.md`. | ✓ | ✓ | ✓ | ✓ | Reduced | A |
| monitorMemory | | vm_statistics | `/proc/meminfo` (Used = `MemTotal - MemAvailable`, App = `AnonPages`, Cached = `Buffers+Cached+SReclaimable-Shmem`); pressure from `/proc/pressure/memory`, falling back to the MemAvailable shortfall with a flag saying which. | ✓ | ✓ | ✓ | ✓ | Port | A |
| monitorNetwork | | getifaddrs/sysctl | `/proc/net/dev`; interface type and default route from `/sys/class/net` and `/proc/net/route`; speed test 100 % reused. | ✓ | ✓ | ✓ | ✓ | Port | A |
| monitorDisk | | IOBlockStorage stats, DiskArbitration | `/proc/diskstats`, `statvfs` over `/proc/self/mounts`; eject via UDisks2. Rates and capacity are unprivileged and complete. **Drive health is not**: SMART and NVMe health need `SG_IO`/`NVME_IOCTL_ADMIN_CMD` on the raw device, i.e. root, where on macOS it arrives free with the IOKit stats. Verdict changed from Port to Reduced by the lead on `SENSORS_BACKEND.md` § Decisions: the health rows are absent unless the helper grows a path for them, and the hub says so rather than showing an empty card. | ◐ | ◐ | ◐ | ◐ | Reduced | A |
| monitorPower | | AppleSmartBattery, IOPS | UPower D-Bus (`org.freedesktop.UPower.Device`: percentage, state, energy-rate, time-to-empty, cycle count, temperature) or `/sys/class/power_supply` directly; adapter watts from `power_now`; peripheral batteries via UPower too. | ✓ | ✓ | ✓ | ✓ | Port | A |
| fanControl | 1.6k / 40% | SMC writes via root launchd daemon | hwmon `pwm*` writes through `vorssaint-helper` (polkit action, systemd unit); curve/heartbeat/watchdog logic reused. Hardware coverage depends on the driver (`nct6775`, `thinkpad_acpi`, `dell-smm-hwmon`, `asus-nb-wmi`). | ✓ | ✓ | ✓ | ✓ | Reduced | C |

## Cross-cutting components

| Component | macOS | Linux | Notes |
|---|---|---|---|
| Menu bar item + metric readouts | NSStatusItem ×N, custom image rendering | StatusNotifierItem over D-Bus (`org.kde.StatusNotifierItem`), one item per readout; GNOME needs the AppIndicator extension (Ubuntu ships it, Fedora does not: the hub says so at first launch). Text readouts render to a pixmap with Pango, reusing `MenuBarRenderer` layout math. | Shell WP |
| Panel (NSPopover anchored to the item) | | Wayland cannot anchor a popover to a tray icon (SNI `Activate` x,y hints are often 0,0). A layer-shell `top` surface anchored top-right with margins on KWin, wlroots, Hyprland, COSMIC (`layer-shell-qt`); a normal always-on-top window at the last position on GNOME; a positioned window on X11. | Shell WP |
| Settings, onboarding, hub | SwiftUI windows | Qt Quick (QML) preferences window with one page per feature group, following the platform theme (Breeze on KDE, Adwaita-like via `qt6ct`/platform theme on GNOME). | Shell WP |
| Global shortcuts | Carbon hotkeys + taps + private SymbolicHotKeys | GlobalShortcuts portal (GNOME 48+, KDE 6, Hyprland); fallback: InputRelay chord matching (works everywhere, needs the helper); X11 `XGrabKey`. Conflict detection: read the DE's binding tables where an API exists, otherwise skip. | Shell WP |
| Permissions page | TCC polling | Replaced by "capabilities": portal availability, helper installed, group memberships, extension installed, compositor protocol list. Same UI shape, different rows. | Shell WP |
| Notifications | UserNotifications | `org.freedesktop.Notifications` over D-Bus, or portal `Notification` under Flatpak. | Shell WP |
| Autostart / login item | SMAppService | XDG autostart `.desktop` in `~/.config/autostart`, portal `Background` under Flatpak. | Shell WP |
| Self-update | dmg + codesign swap | AppImageUpdate (zsync) for AppImage; Flatpak updates itself; feed parser reused. | Packaging WP |
| Feedback | HTTPS POST | Unchanged. | Shell WP |
| Settings backup | UserDefaults export | Same JSON, same keys, so a backup moves between Mac and Linux for shared settings. | Core WP |
| Localization | Compiled `Strings` structs | Unchanged: the compiled catalogs are the largest reusable asset (~25k lines, 13 languages). | Core WP |

## Totals

| Verdict | Features |
|---|---|
| Port | 31 |
| Reduced | 12 |
| Re-imagine | 5 |
| Drop | 9 (dockPreview, dockClick, windowMaximizer, middleClick, finderCutPaste, finderRename, diskImageInstaller, musicBlock, extraBrightness) |

57 rows. The Port/Reduced split moved by one when the sensors backend landed
and `monitorDisk` became Reduced (drive health needs ioctls on the raw
device); these four numbers are asserted against the table in code by
`FeatureSupportTests.testTheVerdictsMatchTheTriageTotals`, so the next verdict
that changes here fails the Linux gate until the catalog changes with it.

Dropped features stay in `AppFeature` so settings backups and the catalog keep
their identity; `isHardwareSupported` (already present for fan control)
becomes `isSupportedOnThisPlatform` and hides them.

---

## WP-15: what each feature needs from the running session

The tables above are the research: what a feature stands on today and what it
would stand on under Linux. This section is the same thing as *code*.
`Sources/VorssaintCore/Core/FeatureSupportCatalog.swift` carries one row per
feature with the platforms it exists on, the verdict above, and the
`PlatformCapability` values (`PLATFORM.md` § 3) a Linux session must have
before the hub offers it. `AppFeature.isSupportedOnThisPlatform` reads it, and
the hub shows `unsupportedOnThisPlatformReason` on the row it greys out.

Three gates, answered in this order:

1. **Platform.** The nine Drop rows are `macOS` only. On Linux they are not
   greyed out, they are absent: the hub never lists a feature that has no
   counterpart. Their availability keys stay in the settings backup, so a
   backup that crosses platforms and comes back is unchanged.
2. **Capabilities.** Every capability listed below must be present. A session
   missing one gets `unsupportedMissingCapabilitiesFormat` with the capability
   named, not a blank greyed row: "This desktop session does not provide what
   this feature needs: audio.streamVolume". The names are deliberately not
   translated, because they are the identities the Capabilities page lists and
   the ones a bug report should carry.
3. **Hardware.** `hardwareUnsupportedReason`, unchanged, still macOS-only and
   still just fan control. It stays in the Mac layer because its answer comes
   from a service, not from a table.

**macOS declares no capability requirement at all, on purpose.** WP-15 must
not change what the macOS product offers, and the macOS gate is the only proof
of that available; a table that could answer "no" on macOS for a new reason
would make that proof worthless. `FeatureSupportTests.testMacOSSupportIsUnconditional`
asserts it for all 57 rows, against a capability reader that answers `false` to
everything.

### The capability requirements

| Feature | Verdict | Needs on Linux |
|---|---|---|
| `switcher` | Reduced | `window.list` + `window.focus` |
| `dockPreview` | Drop | not on Linux |
| `dockClick` | Drop | not on Linux |
| `windowMaximizer` | Drop | not on Linux |
| `windowLayout` | Reduced | `window.list` + `window.moveResize` |
| `autoQuit` | Reduced | `window.list` + `launch.quitApplication` |
| `scrollInverter` | Port | `input.swallow` + `input.synthesize` |
| `focusFollowsMouse` | Re-imagine | none |
| `smoothScroll` | Port | `input.swallow` + `input.synthesize` |
| `mouseAcceleration` | Re-imagine | none |
| `mouseNavigation` | Reduced | `input.swallow` + `input.synthesize` |
| `mouseButtonShortcuts` | Port | `input.swallow` + `input.synthesize` |
| `middleClick` | Drop | not on Linux |
| `mouseClickDebounce` | Port | `input.swallow` |
| `keyboardDebounce` | Port | `input.swallow` |
| `textSnippets` | Port | `input.swallow` + `input.synthesize` |
| `superKey` | Port | `input.swallow` + `input.synthesize` |
| `quitWindowProtection` | Port | `input.swallow` |
| `clipboardHistory` | Reduced | `clipboard.read` + `clipboard.watch` |
| `pastePlain` | Reduced | `clipboard.read` + `clipboard.write` + `input.synthesize` |
| `finderCutPaste` | Drop | not on Linux |
| `finderRename` | Drop | not on Linux |
| `shelf` | Reduced | none |
| `urlCleaner` | Port | `clipboard.read` + `clipboard.write` + `clipboard.watch` |
| `diskImageInstaller` | Drop | not on Linux |
| `mixer` | Port | `audio.streamVolume` |
| `soundOutputSwitcher` | Port | `audio.defaultDeviceSwitch` |
| `micMute` | Port | `audio.sourceVolume` |
| `musicBlock` | Drop | not on Linux |
| `keepAwake` | Port | `power.inhibitSystemSleep` |
| `brightness` | Port | `power.internalBrightness` |
| `extraBrightness` | Drop | not on Linux |
| `bluetoothSleep` | Port | `session.sleepWakeEvents` |
| `quickLauncher` | Port | `launch.applications` + `launch.enumerateInstalled` |
| `quickToggles` | Reduced | `power.lockSession` |
| `colorPicker` | Port | `capture.area` |
| `screenOCR` | Port | `capture.area` |
| `cleaningMode` | Port | `input.swallow` |
| `mediaTools` | Port | none |
| `cleaner` | Port | `files.trash` |
| `uninstaller` | Re-imagine | `packages.list` + `packages.uninstall` |
| `homebrew` | Re-imagine | `packages.list` |
| `appUpdates` | Re-imagine | `packages.list` + `packages.refresh` |
| `screenshot` | Port | `capture.display` + `capture.area` |
| `cameraPreview` | Port | none |
| `radialMenu` | Port | none |
| `scratchpad` | Port | none |
| `commandBar` | Reduced | `launch.applications` + `launch.enumerateInstalled` |
| `screenRecorder` | Port | `capture.stream` |
| `killProcess` | Port | none |
| `monitorCPU` | Port | `sensors.cpu` |
| `monitorGPU` | Reduced | none |
| `monitorMemory` | Port | `sensors.memory` |
| `monitorNetwork` | Port | `sensors.network` |
| `monitorDisk` | Reduced | `sensors.diskActivity` |
| `monitorPower` | Port | `sensors.battery` |
| `fanControl` | Reduced | `power.fanControl` + `sensors.fanSpeed` |

Nine Linux features need nothing: `focusFollowsMouse` and `mouseAcceleration`
are re-imagined as writes to the desktop's own setting, `shelf`, `mediaTools`,
`cameraPreview`, `radialMenu`, `scratchpad` and `killProcess` stand on the file
system, a portal that is always there, or a window of our own, and `monitorGPU`
is Reduced for a reason no capability can carry — coverage depends on the
vendor driver (amdgpu sysfs, NVML, i915), which the sensors backend reports per
machine rather than per session. `SENSORS_BACKEND.md` is the vendor matrix.

Two rows deserve their reasoning spelled out:

- **`monitorDisk` names `sensors.diskActivity` and nothing for health.** Rates
  and capacity are complete and unprivileged; SMART and NVMe health need
  `SG_IO`/`NVME_IOCTL_ADMIN_CMD` on the raw device. There is no capability for
  the health rows yet because there is no backend that could declare one, and
  inventing the name before the path exists is the "plausible-looking second
  implementation" `PLATFORM.md` § 4 warns about.
- **Everything that modifies input names `input.swallow`.** Withholding an
  event from the focused app is the whole mechanism (`InputRelay` in
  `vorssaint-helper`), and it is exactly what a Flatpak sandbox without
  `/dev/uinput` cannot do. A session where the helper is absent therefore
  loses eleven features at once and says so once per row, which is the
  behaviour `PRIVILEGES.md` § 6 describes.

### The three Linux presets

`FeaturePresetCatalog` holds them next to the three macOS ones, tagged by
platform, so neither list can appear on the other's hub.

| Preset | Features |
|---|---|
| **Essentials** (`linuxEssentials`) | the six monitor metrics, `mixer`, `keepAwake`, `clipboardHistory`, `textSnippets`, `screenshot` |
| **Windows** (`linuxWindows`) | `switcher`, `windowLayout`, `autoQuit` |
| **Battery and quiet** (`linuxBatteryQuiet`) | `monitorPower`, `keepAwake`, `bluetoothSleep`, `brightness` |

A preset only offers itself when every feature in it resolves on the running
session (`FeaturePresetDefinition.resolves(has:)`), so **Windows** does not
appear on a GNOME session with no bridge rather than installing three rows that
then explain themselves one by one.

The macOS three are mirrored in the same catalog because
`Sources/Vorssaint/Core/FeaturePresets.swift` stays the macOS hub's surface
until the hub itself is ported — the macOS harness pins which three files may
write an availability key, and moving the writer belongs to that work package,
not to this one. `Tools/linux-port/check-feature-catalog.py` compares the copy
against the original on every CI run, on both legs.

### What the compiler cannot check, and what does

`Core/FeatureCatalog.swift` did not move into the core, and per
`CORE_MOVES.md` appendix A it cannot: it is in a cycle with `RadialMenuSupport`
and behind `GlobalShortcut`, which § 2 of that document proved cannot be split
from Carbon. So the table is keyed by `AppFeature.rawValue` — the stable
identity the availability key and every settings backup are already written
with — and nothing in Swift ties the two lists together.

`Tools/linux-port/check-feature-catalog.py` is what does, on both CI legs: it
parses `enum AppFeature` and the `entry(…)` rows and fails on a case missing
from the table, an id in the table that is not a case, a different order, or a
preset naming a feature that does not exist. A feature whose row is missing
entirely falls back to "on macOS, needs nothing", so the worst a forgotten
entry can do is leave a feature un-triaged for Linux — never change what the
macOS product offers.
