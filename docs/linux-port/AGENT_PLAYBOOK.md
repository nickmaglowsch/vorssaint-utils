# Linux port: agent team playbook

How the port is executed by a team of agents coordinated by one lead. This
file is the operating manual; the *what* lives in `PLAN.md`,
`FEATURE_TRIAGE.md` and `WORK_PACKAGES.md`. Read all four before taking a
work package. Read `docs/AI-CONTRIBUTIONS.md` too: everything in it about
restraint, reading the whole path, and "a system call returning success is not
evidence" applies unchanged.

## Roles

| Role | Count | Owns |
|---|---|---|
| **Lead** | 1 | Backlog order, dispatch, integration branch, gate decisions, user-facing decisions. Never implements a work package itself beyond spikes. |
| **Core porter** | 1 | `Sources/VorssaintCore` extraction: making the platform-free Swift compile on Linux, the `Platform` protocol layer, the macOS adapter that keeps the Mac build green. |
| **Shell squad** | 1 to 2 | Linux app shell: tray, panel, settings, shortcut recorder, onboarding, persistence, localization plumbing, autostart, notifications. |
| **Feature squads** | 2 to 4 in parallel | One work package each from `WORK_PACKAGES.md` Phase 3. A squad owns a feature end to end: platform backend, UI, tests, docs. |
| **Systems squad** | 1 | Privileged helper (`vorssaint-helper`), evdev/uinput, hwmon, DDC, polkit policy, udev rules. Security review of every privileged path. |
| **Packaging/CI squad** | 1 | Linux build script, AppImage and Flatpak recipes, GitHub Actions Linux jobs, headless GUI test harness, smoke matrix. |
| **QA/reviewer** | 1 | Adversarial review of every PR before the lead merges; runs the smoke matrix; keeps the support matrix in `FEATURE_TRIAGE.md` truthful. |

An agent may hold several roles in sequence but never two work packages at
once. A work package is owned by exactly one agent until it is merged or
handed back.

## How work is dispatched

1. The lead picks the next unblocked work package in `WORK_PACKAGES.md`
   (lowest ID first inside the current phase), and spawns an agent with a
   prompt that contains: the WP text verbatim, the branch name, the
   acceptance criteria, the list of files it may touch, and the `Definition
   of done` below.
2. The agent works on its own branch `linux/<wp-id>-<slug>` cut from the
   integration branch `linux/main`.
3. The agent reports back with: what was done, what was verified and how
   (command output, not claims), what was skipped and why, and any decision
   it needs from the lead. A report without verification output is returned.
4. The QA/reviewer agent reviews the diff adversarially against the
   acceptance criteria and the conventions below, and either approves or
   lists concrete defects. The lead merges into `linux/main` only after
   approval and a green Linux CI run.
5. The lead updates the status column in `WORK_PACKAGES.md` in the same
   merge (it is the single source of truth for progress).

The lead runs at most as many squads in parallel as there are independent,
unblocked work packages. Contention on shared files (`Package.swift`,
`Sources/VorssaintCore/Platform/*.swift`, `Localization.swift`) is resolved
by serializing those changes through the core porter.

## Branches and repository layout

- `main` stays the macOS product and must remain buildable with `./build.sh`
  at every merge. The macOS CI jobs are a hard gate on every Linux PR.
- `linux/main` is the integration branch for the port. It is rebased onto
  `main` by the lead weekly, and merged back to `main` at each milestone
  gate (see `PLAN.md`, Phases) once the macOS build is proven unchanged.
- Work branches: `linux/<wp-id>-<slug>`, one PR each, into `linux/main`.
- PR size: the upstream guidance stands. A package that would exceed roughly
  800 changed lines is split by the agent before opening the PR, and the
  split is described in the PR. Generated bindings and vendored protocol
  XML are exempt but must land in their own PR.

## Conventions every agent follows

- **One Swift core, two platform targets, one Qt shell.** Platform code
  lives only under `Sources/VorssaintCore/Platform` (protocols),
  `Sources/VorssaintMac` and `Sources/VorssaintLinux`; the Linux UI lives
  under `linux/shell` (Qt Quick) and talks to the core only through the
  `CoreBridge` snapshot/command surface (`BRIDGE.md`). `#if os(Linux)`
  inside shared code is a smell to be justified in the PR; the default is a
  protocol plus two implementations.
- **UI observes services, services never import a UI toolkit.** The
  upstream boundary holds on Linux: services publish through
  `ObservableObject`/Combine (OpenCombine on Linux, see `PLAN.md`) and, on
  Linux, a `Snapshot`; views only render.
- **Every user-facing string goes through `Strings`** and must be provided
  in all thirteen languages in the same PR. The compiler is the check.
  Linux-only strings live in the same catalogs, never in a side file.
- **No new runtime dependency without a line in `PLAN.md` § 7**
  naming what carries it, how it is bundled, and what breaks without it.
  Optional system tools (for example `ddcutil`, `tesseract`) are detected at
  runtime and the feature degrades with an honest message; they are never a
  hard requirement to launch.
- **Privilege is a design item, not an implementation detail.** Anything
  that needs root, a udev rule, `input` group membership or a polkit action
  goes through `vorssaint-helper` with the narrowest possible D-Bus method,
  documented in `docs/linux-port/PRIVILEGES.md` (created by WP-S1). No
  `sudo`, no `pkexec` of arbitrary scripts.
- **Desktop support is declared, not assumed.** Every feature backend
  states which of GNOME (Wayland), KDE Plasma (Wayland), wlroots (Sway,
  Hyprland), and X11 it supports, and the feature hub hides or explains
  what the running session cannot do. The `FEATURE_TRIAGE.md` matrix is
  updated in the same PR that changes support.
- **Read back after writing.** Compositor requests, D-Bus calls and sysfs
  writes report success without effect. Verify (query the state again) and
  log a tolerance where relevant, exactly as the upstream Accessibility code
  does.
- **Comments explain why, rarely.** Match the file you are in.
- **Tests:** pure logic gets `swift test` unit tests in `Tests/`; platform
  backends get a fake compositor or fake D-Bus test where feasible and a
  manual smoke entry otherwise. Never skip or quarantine a test to get green.
- **Commit messages** follow the upstream `type(scope): summary` style, for
  example `feat(linux-audio): per-app volume over PipeWire`.

## Definition of done for a work package

A WP is done only when all of these hold, and the agent's report shows the
evidence for each:

1. Acceptance criteria in `WORK_PACKAGES.md` are met, each one named with
   how it was verified.
2. `swift build` on Linux and `./build.sh` on macOS (CI) are green; no new
   warnings.
3. Unit tests added for the pure logic touched; `swift test` passes.
4. The smoke matrix row(s) for the feature were run headless in CI (weston
   headless or Xvfb, per WP-P3) and, where the WP says so, manually on the
   named desktops with the result recorded in the PR.
5. Strings present in all languages; feature hub entry, permission/privilege
   explanation and energy badge updated.
6. `FEATURE_TRIAGE.md` support matrix and `WORK_PACKAGES.md` status updated.
7. QA/reviewer approval, then lead merge.

## Reporting format (agent → lead)

```
WP: <id> <title>
Branch: linux/<id>-<slug>   PR: <url or "not opened: reason">
Done:      <bullet per acceptance criterion, with the verification command and its result>
Skipped:   <what and why, or "nothing">
Decisions: <questions the lead must answer, or "none">
Risks:     <anything learned that changes the plan>
```

## Gates the lead enforces

- **Phase 0 gate:** the four spikes in `WORK_PACKAGES.md` Phase 0 have
  produced go/no-go evidence; the toolkit and packaging decisions in
  `PLAN.md` are confirmed or revised in writing before Phase 1 begins.
- **Phase 1 gate:** `VorssaintCore` builds and its tests pass on Linux and
  macOS; the macOS app is unchanged in behaviour (selftest and the UI smoke
  script pass).
- **Phase 2 gate:** an AppImage launches on a clean Ubuntu LTS, Fedora
  Workstation and an Arch-based KDE session, shows the tray icon, opens the
  panel and settings, records a global shortcut, survives logout/login via
  autostart, and installs/uninstalls features in the hub.
- **Milestone gates for each feature wave:** the wave's features are green in
  the smoke matrix on GNOME, KDE and one wlroots compositor, or their
  reduced-scope behaviour is documented in the hub and the triage matrix.

## What the lead never delegates

- Renaming/branding decisions (`TRADEMARKS.md` requires a distinct identity
  for unofficial builds; see `PLAN.md` § 10).
- Adding a runtime dependency or a privileged path.
- Dropping a feature from scope, or changing its declared desktop support.
- Merging into `main`.
