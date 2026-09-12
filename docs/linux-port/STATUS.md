# Linux port: execution status

Kept by the lead. One row per work package that has been dispatched; the
detailed backlog stays in `WORK_PACKAGES.md`.

## Execution environment constraints (Phase 0, September 2026)

The lead's session runs in a headless Ubuntu 24.04 container without a
desktop session and behind an egress policy that allows the Ubuntu
archive, PyPI, npm and crates.io but blocks swift.org and GitHub release
downloads. Consequences for Phase 0:

- Swift cannot be compiled locally. WP-00 runs its real compilation on
  GitHub Actions through a workflow pushed to the plan branch, and does a
  static census locally.
- Qt 6, GTK4/libadwaita, PipeWire, xdg-desktop-portal-wlr, sway and
  weston (headless), Xvfb, ffmpeg, Tesseract and zxing come from apt, so
  WP-01, WP-02 and WP-03 build and run here under headless compositors.
- GNOME and KDE sessions are not available; every desk-only claim is
  marked as such in the spike reports and re-verified in Phase 2's smoke
  matrix on real desktops.
- `/dev/uinput` and `/dev/input` are absent; WP-03 establishes what the
  kernel allows and otherwise proves the relay through a fake device layer.

## Team

| Role | Model | Assignment |
|---|---|---|
| Lead | this session | dispatch, corrections, merges, gates |
| Executor WP-00 | Opus | Swift core census + CI compile |
| Executor WP-01 | Opus | toolkit bake-off (Qt Quick vs GTK4/libadwaita, C bridge stub) |
| Executor WP-02 | Opus | portal ScreenCast + PipeWire + ffmpeg proof on headless sway |
| Executor WP-03 | Opus | evdev/uinput relay, privilege prototype, PRIVILEGES.md draft |
| QA verifiers | Sonnet, one per WP | re-run every command in the executor's report, check claims against outputs, approve or list defects |
| Executor WP-04 | Opus, after WP-01 approval | packaging proof |

## Phase 0 board

| WP | Status | Branch | Notes |
|---|---|---|---|
| WP-00 | merged | plan branch | GO WITH CONDITIONS; QA approved with nits (fixed) |
| WP-01 | merged | plan branch | Qt 6 Quick confirmed (45 % of GTK's RSS, tray in 12 lines); Ubuntu 24.04 ships layer-shell only for Qt 5 and GTK 3 |
| WP-02 | merged | plan branch | full chain proven on headless sway (29.5 fps, 1.1 ms latency, audio verified); stock xdg-desktop-portal-wlr needs a one-line patch on the pixman renderer; WINDOW requests are mis-served on wlr |
| WP-03 | merged | plan branch | QA found a grab-release defect, fixed with a mutation-tested regression test; real uinput still needs hardware (WP-S1) |
| WP-04 | merged | plan branch | AppImage green on 4 distros × 2 modes, Qt-free chroot proof; Flatpak cannot open /dev/uinput even with --device=all; QA approved |

**Phase 0 gate: passed 2026-09-12.** Outcomes recorded in `PLAN.md` § 4 and § 8.

## Phase 1 board

| WP | Status | Branch | Notes |
|---|---|---|---|
| WP-10 | merged | plan branch | package split + `linux-port-ci.yml` gate (Linux core build, macOS build.sh + selftest + 31565 checks), QA approved |
| WP-11 | review | plan branch | 97 files / 44 389 lines in VorssaintCore; `Tools/linux-port/declgraph.py`; `CORE_MOVES.md` has every move, split and reason |
| WP-S1 | in progress (daemon side) | worktree | production helper from the WP-03 spike: session binding, hot-plug, hwmon, DDC, systemd/polkit/udev files |
| WP-C1 | in progress | worktree | C window backends: X11 EWMH, wlr foreign-toplevel + Sway IPC, Hyprland IPC, KWin script bridge; `vorssaint_platform.h` contract |
| WP-13 | merged | plan branch | COMBINE.md + audit script (83 Combine-touching files, 35 only via AppKit/SwiftUI re-export); VorssaintCombine target to be deleted after WP-11 |
