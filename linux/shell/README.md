# `linux/shell` — the Linux app

The Qt 6 Quick app: a StatusNotifierItem tray item, a panel with a live CPU
readout, and the preferences and overlay screens WP-01 measured. It talks to
the Swift core only through the `CoreBridge` C ABI in `include/corebridge.h`
(`docs/linux-port/BRIDGE.md`).

This is the **WP-20/WP-21 slice**, not the finished shell. What is here: the
executable, the tray item, `CoreModel`, three screens, the CLI flags and the
packaging. What is not: the full panel with tabs and sections (WP-22),
settings, the features hub and onboarding (WP-23), global shortcuts (WP-24),
the capabilities page (WP-25), layer-shell overlays (WP-29).

## Download and run it

Every push to a `claude/**` branch builds an AppImage in the `shell-appimage`
job of `.github/workflows/linux-port-ci.yml` and uploads it as the
`vorssaint-linux-appimage` artifact. Download it from the run's summary page,
then:

```sh
chmod +x Vorssaint-x86_64.AppImage
./Vorssaint-x86_64.AppImage --version
./Vorssaint-x86_64.AppImage --selftest     # no display needed
./Vorssaint-x86_64.AppImage                # tray item + panel
```

Without FUSE (most containers, some minimal systems), prefix it with
`APPIMAGE_EXTRACT_AND_RUN=1`. The tray item needs a panel that implements
`org.kde.StatusNotifierWatcher`: KDE Plasma and most wlroots panels do out of
the box, GNOME needs the AppIndicator extension.

## Build it

Against the real Swift bridge, which is the default and what ships:

```sh
# 1. the bridge archive, from the Swift core (needs a Swift 6.1 toolchain)
swift build --target VorssaintLinux
#    ... ar the objects into libvorssaintbridge.a; the exact commands are the
#    `shell-appimage` job's, and docs/linux-port/BRIDGE.md § 8 explains them.

# 2. the shell
cmake -S linux/shell -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DVORSSAINT_BRIDGE_LIB=/tmp/libvorssaintbridge.a \
      -DVORSSAINT_SWIFT_RUNTIME_FLAGS="$(swift-autolink-extract ... )"
cmake --build build -j"$(nproc)"

# 3. the AppImage
linux/packaging/build-appimage.sh build Vorssaint-x86_64.AppImage
```

With no Swift toolchain at hand, `-DVORSSAINT_REAL_BRIDGE=OFF` builds against
the WP-01 C stub instead. That configuration draws a **generated** series and
the panel says so on its face; it is for toolkit work, never for a demo.

## Flags

| Flag | What it does |
|---|---|
| `--version` | prints `Vorssaint <version> (linux)` and exits |
| `--selftest` | headless checks, exit 1 on the first failure (see below) |
| `--screen panel\|prefs\|overlay` | which screen to show |
| `--quit-after <ms>` | exit after N ms, for smoke runs and RSS measurement |

`--selftest` forces the `offscreen` platform plugin and checks, in order: the
bridge answers with a JSON object for `metrics`, `featureRuntime` and `l10n`;
the CPU sampler reached `/proc/stat`; a sample pushed into the core comes back
out of the snapshot the panel binds; and all three QML screens instantiate.
That last one is what a package with a missing QML module fails on.

## Where the CPU number comes from

`linux/platform/sensors` — the port's one reader of `/proc/stat` — sampled by
`MetricsSampler` every 500 ms and pushed into the core's `metrics` service as
`{"sample":<percent>}`. The core keeps the 60-sample ring buffer and publishes
it; the panel renders it. When the sampler cannot start, or the build is
against the stub bridge, the panel labels the series **PLACEHOLDER DATA** and
names the reason. WP-A1 moves the sampling into the core (`SystemSensors`) and
deletes `MetricsSampler`; nothing in the QML changes when it does.

## Tests

```sh
linux/shell/tests/smoke-tray.sh <binary-or-AppImage> out.png
```

runs it under Xvfb on a private session bus with the WP-01 fake
StatusNotifierWatcher and fails unless the tray item really registers over
D-Bus. `tests/abi_client.c` is the ABI conformance test for the header and is
built by the `bridge-abi` CI job.
