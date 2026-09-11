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
| WP-00 | in progress | worktree → plan branch (CI workflow) | |
| WP-01 | in progress | worktree | |
| WP-02 | in progress | worktree | |
| WP-03 | in progress | worktree | |
| WP-04 | todo | | blocked by WP-01 |
