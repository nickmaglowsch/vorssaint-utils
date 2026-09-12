# `linux/platform`: the C platform layer

Linux platform code is C under `linux/platform`, one directory per concern,
each a library with a CLI harness that can be driven headless. This is the
Phase 0 decision recorded in `docs/linux-port/PLAN.md` § 5: the toolkit is Qt,
the system libraries are C, and Swift cannot be compiled in the team's
execution environment, so the Swift `Platform` protocols (WP-12) are mirrored
one-to-one by a C header rather than reimplemented in Swift.

```
linux/platform/
  include/vorssaint_platform.h   the contract; one section per concern
  window/                        WP-C1: X11, wlroots, Hyprland, KWin, GNOME
    kwin/vorssaint-window.js     the KWin bridge script
    protocols/                   vendored Wayland protocol XML
    tools/vs_window_cli.c        the `vs-window` harness
    tests/                       ctest suites, real and fake compositors
```

Concerns still to land add their own directory and their own section of the
header: capture (WP-B1), audio, sensors, power, input, portals, helper-client.

## The contract

`include/vorssaint_platform.h` is the only file the Swift side reads. Each
concern is a `vs_<concern>_system` struct: a name, a `capabilities` bitmask, an
opaque `impl` pointer, a set of function pointers, and a `destroy`. A
constructor probes the running session and returns the struct, or NULL with a
reason.

Rules every section follows, and every reviewer should check:

- **Every call returns `int`**: `VS_OK` or a negative `vs_result`. No call
  returns a pointer that means "failed" without also saying why.
- **Capabilities are fixed for the life of the instance.** A member whose
  capability bit is clear still exists and returns `VS_ERR_UNSUPPORTED`; the
  caller never has to test a function pointer for NULL.
- **Success is read back, not assumed.** Compositors, window managers and
  D-Bus bridges all acknowledge a request and then do something else.
  `VS_ERR_NOT_APPLIED` is the answer for "we asked, it agreed, the state did
  not change" — the same judgement the macOS `WindowLayoutService` makes when
  it re-reads a window's frame after setting it.
- **"Unknown" is never zero.** `pid` and `workspace` are -1 when the backend
  cannot say, because 0 is a real pid and a real workspace.
- **Ownership is explicit.** Every array a backend returns is released by that
  backend's matching `free_*`.
- **Events are delivered from `dispatch`, never from another thread.** A
  backend exposes a pollable `event_fd` (or -1) and drains into the callback
  when the caller asks it to. Nothing in this layer starts a thread.

## How WP-12 mirrors it

`Sources/VorssaintCore/Platform/WindowSystem.swift` (WP-12) declares the same
shape in Swift; `Sources/VorssaintLinux` implements it as a thin wrapper over
this table and nothing else. The mapping is mechanical:

| C | Swift |
|---|---|
| `vs_window_system` | `protocol WindowSystem` |
| `capabilities` (`vs_window_capability`) | `var capabilities: WindowSystemCapabilities` (`OptionSet`) |
| `vs_window_info` | `struct WindowInfo` |
| `vs_window_flag` | `WindowInfo.Flags` (`OptionSet`) |
| `vs_rect` | `CGRect`-shaped value with integer members |
| `vs_result` | `enum WindowSystemError: Error`, `VS_OK` mapped to a normal return |
| `vs_window_event` + callback | an `AsyncStream<WindowEvent>` or a Combine publisher |
| `list` + `free_list` | one call returning `[WindowInfo]` |

Two rules keep the mirror honest:

1. **The C side is the source of truth for names and semantics.** A field
   renamed here is renamed there in the same PR.
2. **The Swift side adds no behaviour.** Retries, tolerance checks and
   read-back all live in C, where they can be tested against a real compositor
   without a Swift toolchain. The Swift wrapper marshals and nothing else.

The macOS adapter implements the same Swift protocol over AppKit and
Accessibility, so the services above it (`WindowEnumerator`,
`WindowLayoutService`, `AutoQuitService`) never learn which platform they are
on.

## Building

```sh
cmake -S linux/platform -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build dependencies on Ubuntu 24.04:

```sh
apt-get install -y cmake pkg-config libxcb1-dev libxcb-ewmh-dev libxcb-icccm4-dev \
                   libwayland-dev wayland-protocols libjson-c-dev libsystemd-dev
```

Test dependencies (the suites skip with ctest's "not run" code 77 when one is
missing): `xvfb`, `openbox`, `xterm`, `x11-utils` for X11; `sway`, `foot` for
wlroots; `dbus-x11` (`dbus-run-session`), `python3-dbus`, `python3-gi` for the
D-Bus fakes; `nodejs` to lint the KWin script.

Everything is built with `-Wall -Wextra -Werror` plus `-Wshadow`,
`-Wstrict-prototypes`, `-Wmissing-prototypes`, `-Wpointer-arith` and
`-Wwrite-strings`. The only exception is `wayland-scanner`'s generated code,
which is compiled into its own object library with `-Wno-missing-prototypes`
because it is not ours to fix.

## Per-concern notes

- [`window/`](window/) — backends, capability matrix and measured behaviour:
  `docs/linux-port/WINDOW_BACKENDS.md`.
