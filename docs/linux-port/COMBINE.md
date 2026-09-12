# Combine on Linux (WP-13)

Audit and convention document for replacing `Combine` with `OpenCombine`
0.14 on the Linux build, narrowed from the `VorssaintCombine`-module design
in `PLAN.md` § 5 and `WORK_PACKAGES.md` WP-13 to the per-file guard that
`build.sh`'s single-module compile forces (§ 1 below). WP-11 (moving files
into `Sources/VorssaintCore` concurrently) uses exactly the guard in § 1;
this document does not move or edit any file under `Sources/`.

## 0. Why not the `VorssaintCombine` module for core code

`build.sh` compiles `Sources/Vorssaint`, `Sources/VorssaintCore` and
`Sources/VorssaintMac` into **one** `swiftc` invocation for the shipped
macOS app (`PLAN.md` § 5, "Rules of the architecture"). `VorssaintCombine`
is a separate SwiftPM target that exists only for `swift build` on Linux —
it is never part of that `swiftc` invocation, so a file under
`Sources/VorssaintCore` that wrote `import VorssaintCombine` would build
under `swift build --target VorssaintCore` and fail `build.sh` with "no
such module". The module is real (WP-10 wired it into `Package.swift` and
it re-exports the right thing per platform — see § 4 below) but it can only
be consumed by a target that SwiftPM builds standalone, i.e.
`VorssaintLinux`, never by anything `build.sh` also compiles.

## 1. The convention

Every file that touches Combine, in `VorssaintCore` or anywhere else that
must build both ways, spells the import as:

```swift
#if canImport(Darwin)
import Combine
#else
import OpenCombine
#endif
```

Add the matching product only where the file actually needs it (see § 2):

```swift
#if canImport(Darwin)
import Combine
#else
import OpenCombine
import OpenCombineFoundation   // Timer.publish, NotificationCenter.publisher, RunLoop scheduling
import OpenCombineDispatch     // DispatchQueue as a Combine Scheduler
#endif
```

Rules that follow from `spikes/00-swift-core.md` § 6 and `PLAN.md` § 5:

- Key the switch on **`canImport(Darwin)`**, never `canImport(Combine)`. A
  `canImport(Combine)` check inside a module that itself contains a target
  named `Combine` resolves to that sibling and Swift 6.1 rejects it as a
  circular dependency — this is what broke WP-00's first iteration. There is
  no such shim target in the real port, but the convention exists precisely
  so nobody adds one that breaks this again.
- No SwiftPM target in this package may be named `Combine`, for the same
  reason (`Package.swift` already carries this as a comment).
- `Sources/VorssaintMac` and `Sources/Vorssaint` (the AppKit/SwiftUI half of
  the app) keep plain `import Combine` — they only ever build on Darwin, so
  the guard is unnecessary machinery there. Add the guard only to a file
  that WP-11/WP-12 is moving into `VorssaintCore` or `VorssaintLinux`.
- `Package.swift` already declares `OpenCombine`, `OpenCombineDispatch` and
  `OpenCombineFoundation` as conditional (`.when(platforms: [.linux])`)
  dependencies of both `VorssaintCombine` and `VorssaintCore` (WP-10). This
  document does not change that wiring.

## 2. The three OpenCombine products

| product | brings | needed for |
|---|---|---|
| `OpenCombine` | `ObservableObject`, `@Published`, `AnyCancellable`, `PassthroughSubject`, `CurrentValueSubject`, `Just`, `Future`, `sink`/`assign`/`map`/`filter`/`merge`/`zip`/`combineLatest`/`removeDuplicates`/`dropFirst`/`prepend`/… | the vast majority of this codebase: every `ObservableObject` singleton and its `@Published` properties, `.sink`, `.removeDuplicates`, `.merge` |
| `OpenCombineFoundation` | `Timer.publish(every:on:in:)`, `NotificationCenter.default.publisher(for:)`, `RunLoop` as a `Scheduler` | the 5 files that use `NotificationCenter.default.publisher` (all SwiftUI `.onReceive` call sites, § 3) and any future `Timer.publish` use — **not** needed for the far more common `Timer.scheduledTimer` + `RunLoop.main.add(_:forMode:)` pattern already used in ~20 service files, which is plain Foundation run-loop registration, not a Combine scheduler |
| `OpenCombineDispatch` | `DispatchQueue` as a `Scheduler`, i.e. `.receive(on: DispatchQueue.main)` | the 8 files that call `.receive(on:)` — all of them schedule onto `DispatchQueue.main` in this codebase |

Practical reading: a ported service that is a plain `ObservableObject` with
`@Published` state and no `.receive(on:)`/notification/timer publisher needs
only `OpenCombine`. Add `OpenCombineDispatch` the moment `.receive(on:
DispatchQueue...)` appears; add `OpenCombineFoundation` the moment
`Timer.publish` or `NotificationCenter....publisher` appears.

## 3. The audit

`Tools/linux-port/combine-audit.py` (added by this WP) walks `Sources/` and
regenerates the table below. Re-run it after every WP-11/WP-12 move:

```
$ python3 Tools/linux-port/combine-audit.py
```

### Totals (this run, before WP-11 has moved anything)

- **83** files touch the Combine surface (`ObservableObject`, `@Published`,
  `AnyCancellable`, `PassthroughSubject`/`CurrentValueSubject`,
  `objectWillChange`, `Just`/`Future`, `Timer.publish`,
  `NotificationCenter....publisher`, or an operator chained off one of
  those).
- **48** have an explicit `import Combine`.
- **35** have *no* explicit Combine import and compile today only because
  `import AppKit` or `import SwiftUI` re-exports Combine on Darwin. This is
  the concrete trap for whoever ports these files: on Linux, `AppKit`
  doesn't exist and `SwiftUI` doesn't exist, so the transitive re-export is
  gone and each of these 35 files needs the § 1 guard added explicitly, not
  copied as-is. The full list is in the CSV (`--csv`) and the generated
  table; representative ones: `App/FeatureRuntime.swift`,
  `Services/Display/BrightnessService.swift`,
  `Services/FanControl/FanControlService.swift`,
  `Services/Media/MediaService.swift`, most of `Services/QuickTools/*`.
- **1** of the 83 is a WP-11 core-move candidate by the WP-00 census:
  `Core/Localization.swift` (3193 lines, classified "needs OpenCombine
  only"). This matches WP-00's own finding exactly (`spikes/00-swift-core.md`
  § 1/§ 6: "only one file in the whole candidate set imports Combine"). The
  other 82 are service classes and SwiftUI views that stay in
  `Sources/VorssaintMac`/`Sources/Vorssaint` until WP-12's protocol adapters
  give them a Linux-side implementation to move alongside — WP-13 does not
  change that plan, it only confirms which of them will need the guard when
  their turn comes.
- Zero uses of `.debounce`, `.throttle`, `.catch`, `.retry`, `.replaceError`,
  `.timeout`, `.multicast`, or the `assign(to: &$x)` publish-to-`@Published`
  overload anywhere in the codebase. The Combine surface here is almost
  entirely "singleton is `ObservableObject`, some properties are
  `@Published`, a handful of `.sink`/`.receive(on:)`/`.removeDuplicates`
  chains observe another singleton's `@Published` property" — not a
  publisher-pipeline-heavy codebase. That materially lowers WP-13's risk:
  the operators actually in use are the ones OpenCombine's own README leads
  with.

### OpenCombine 0.14 verdict per operator in use

This is `combine-audit.py`'s knowledge table, not something checked against
OpenCombine's actual source in this environment: `github.com` is 403'd by
the egress proxy here (see `PREAMBLE.md`) and OpenCombine is not vendored
into this worktree, so its implementation cannot be read offline. WP-00
independently confirmed (`spikes/00-swift-core.md` § 6) that
`OpenCombine`/`OpenCombineFoundation`/`OpenCombineDispatch` 0.14.0 resolve
and build with zero errors, but against a candidate set that exercises only
`@Published`/`ObservableObject` (`Localization.swift`) — none of the
operators below have been built against OpenCombine yet. Rows marked
"verify on CI" are exactly the ones with no such evidence:

| operator | files | verdict | note |
|---|---:|---|---|
| `ObservableObject` | 76 | supported | core protocol |
| `@Published` | 72 | supported | core property wrapper |
| `AnyCancellable` | 11 | supported | core type |
| `objectWillChange` | 3 | supported | `ObservableObjectPublisher` |
| `.receive(on:)` | 8 | supported, needs `OpenCombineDispatch` | every call site in this codebase schedules onto `DispatchQueue.main` |
| `.removeDuplicates` | 3 | supported | core operator |
| `.dropFirst` | 7 | supported | core operator (distinct from the ~30 unrelated `String`/`Array.dropFirst` call sites elsewhere in the codebase — the audit script excludes those) |
| `.prepend` | 1 | supported | core operator |
| `.merge` | 3 | supported | core operator |
| `.delay(for:)` | 1 | supported, needs a `Scheduler` product | unexercised by WP-00 — **verify on CI** |
| `NotificationCenter....publisher` | 5 | supported, needs `OpenCombineFoundation` | all 5 sites are SwiftUI `.onReceive` calls (§ 3.1) |
| `.sink` | (counted under core types above where paired with `AnyCancellable`) | supported | core operator |

Not present in this codebase, so untested by this audit but relevant if a
future service adds them: `.debounce`, `.throttle`, `.catch`, `.retry`,
`.replaceError`, `.timeout`, `.multicast`, `.share()`, `Timer.publish`, and
the `assign(to: &$x)` overload — treat all of these as **verify on CI**
before relying on them in a Linux-only file.

### 3.1 The 5 `NotificationCenter....publisher` sites are SwiftUI, not portable regardless

All five hits (`UI/Cleaner/CleanerView.swift`, `UI/MenuPanel/MixerSection.swift`,
`UI/MenuPanel/PanelWindowLayoutView.swift`, `UI/MenuPanel/MenuPanelView.swift`,
`UI/Settings/WindowLayoutSettings.swift`) are `.onReceive(...)` inside a
SwiftUI `View`. SwiftUI itself does not exist on Linux (§ 5), so these
files are never ported as-is — the Qt/QML shell reimplements the same
notification-driven refresh against `CoreModel`, not against
`OpenCombineFoundation`. They are listed here for completeness, not because
they are OpenCombine work.

## 4. SwiftUI-only APIs and the Linux replacement

`@StateObject`, `@ObservedObject`, and `@EnvironmentObject` are SwiftUI
property wrappers, not Combine — they do not exist on Linux because SwiftUI
does not exist on Linux (`PLAN.md` § 4.1: "Missing on Linux: AppKit,
CoreGraphics beyond the `CGFloat`/`CGRect` family, IOKit, `os.log`, and
Combine" — SwiftUI is missing for the same reason AppKit is: it is an Apple
UI framework, Combine's absence is the separate, narrower problem this WP
solves). Porting a view that uses them is not a Combine question at all;
it is the toolkit swap `PLAN.md` § 4.2 already decided:

- The Swift core exposes each service over a C ABI (`@_cdecl`):
  `subscribe(service, callback)`, `command(service, json)`,
  `snapshot(service) -> json`. Each service's `@Published` properties
  become one `Codable` **snapshot** struct; snapshots are diffed on the
  Swift side so the far side isn't re-rendering on no-op updates.
- On the Qt/QML side there is one generic `CoreModel` (`QObject` with a
  `QVariantMap` state and `invoke(command)`), instantiated per service and
  bound in QML — no per-view `@ObservedObject`/`@EnvironmentObject`
  plumbing to replace, because QML binds to `CoreModel.state` the way a
  SwiftUI view bound to the service directly.
- `@StateObject`/`@ObservedObject` ownership semantics (who allocates the
  service, who merely observes it) map onto "who instantiates the
  `CoreModel` for that service" on the Qt side; `@EnvironmentObject`'s
  implicit environment injection has no equivalent and is not needed, since
  QML resolves a `CoreModel` by name/context property instead of by view
  hierarchy.
- This is exactly `WP-18` in `WORK_PACKAGES.md` (`CoreBridge`:
  `@_cdecl` subscribe/command/snapshot surface, depends on WP-12 **and**
  WP-13). WP-13 hands WP-18 an accurate map of which services are plain
  `@Published`/`ObservableObject` (the vast majority per § 3) versus ones
  with a `.receive(on:)`/`.merge` pipeline worth collapsing into the
  snapshot diff rather than replaying operator-by-operator.

## 5. `@Observable` — considered, not recommended for this codebase

`PLAN.md` § 4.1 raises `@Observable` (the Observation module, which does
ship on Linux) as an alternative to OpenCombine: "or by migrating singletons
to `@Observable`... WP-00 measures which costs less across 78
`ObservableObject`s." Recommendation: **stay with `ObservableObject` +
OpenCombine, do not migrate to `@Observable`.**

Reasons:

1. **The project builds with the Xcode Command Line Tools, not Xcode**
   (`CONTRIBUTING.md`: "You need macOS 14 or newer, Apple Silicon and the
   Xcode Command Line Tools... Singletons are exposed as `Type.shared` and
   publish state with Combine through `ObservableObject`, with no
   Observation macros, since the project builds with the Command Line
   Tools."). The `@Observable` macro is implemented as a compiler plugin;
   Command Line Tools-only Swift toolchains have historically lagged or
   varied in plugin support relative to full Xcode, and this constraint
   predates this port — it is the existing contributor environment, not
   something WP-13 introduces. Migrating to `@Observable` would tighten a
   toolchain requirement the project has deliberately avoided.
2. **Scale of the migration is the whole point of friction, not a detail.**
   78 `ObservableObject` singletons (§ 3's audit counts 76 files carrying
   the symbol today — consistent with `PLAN.md`'s count) is exactly the
   surface a rewrite would have to touch, for a payoff (dropping the
   OpenCombine dependency) that only matters on Linux — the Mac app keeps
   `Combine` either way, so `@Observable` would mean maintaining **two**
   observation mechanisms if migrated selectively, or rewriting all 78 for
   a platform most of them don't ship on yet.
3. **`@Observable` does not replace `.sink`/`.receive(on:)`/`.merge`.**
   Eight files chain `.receive(on:)` and three chain `.merge` off a
   `@Published` property to observe *another* singleton's state (cross-service
   reactions, not view bindings) — the actual use this project makes of
   Combine beyond the property wrapper. `@Observable` has no equivalent;
   those call sites would still need Combine/OpenCombine (or a hand-rolled
   callback), so the migration would not even fully remove the OpenCombine
   dependency — it would add a second mechanism alongside it.
4. **The guard convention (§ 1) already gives Linux a working answer
   without touching Mac-side code**, which is the smaller, reversible
   change `docs/AI-CONTRIBUTIONS.md`'s "deletion beats addition" bias
   favors over introducing a new observation paradigm project-wide.

This is a recommendation, not a decision — if a future WP finds
`@Observable`-only performance or diffing wins large enough to justify
touching all 78 singletons, revisit here.

## 6. Checklist for a service being ported (WP-11/WP-12/WP-18)

For each `ObservableObject` singleton moving into `VorssaintCore` or getting
a Linux-side implementation:

- [ ] Does the file currently rely on a transitive Combine import via
      `AppKit`/`SwiftUI` (check `combine-audit.py`'s "transitive only"
      column)? If yes, add the explicit § 1 guard — it will not compile on
      Linux without it, and `build.sh` will not warn you, because the Mac
      build still has the transitive re-export.
- [ ] Does it use `.receive(on:)`? Add `import OpenCombineDispatch` in the
      `#else` branch.
- [ ] Does it use `Timer.publish` or `NotificationCenter....publisher`? Add
      `import OpenCombineFoundation` in the `#else` branch. (Plain
      `Timer.scheduledTimer` + `RunLoop.main.add(_:forMode:)` needs neither
      — it is Foundation, not Combine.)
- [ ] Does it use any operator marked "verify on CI" in § 3's table
      (`.delay(for:)`, or anything from the "not present, treat as verify on
      CI" list if it has since been added)? Build it on the Linux CI leg
      before trusting the port; do not assume parity from this document.
- [ ] Is the file a SwiftUI `View` (`import SwiftUI`, not a service)? It is
      out of scope for a Combine-guard fix — it is replaced by QML per § 4,
      not ported.
- [ ] Re-run `python3 Tools/linux-port/combine-audit.py` after the move and
      confirm the file dropped out of the "transitive only" / "WP-11
      candidate" columns and that its operators still show a verdict you
      trust.

## 7. Recommendation: the `VorssaintCombine` target

**Delete `VorssaintCombine`** (the SwiftPM target, its product declaration,
and the OpenCombine dependency lines that exist only to feed it in
`Package.swift`) rather than keep it. This is a recommendation for the lead
to act on — WP-13 does not implement it (`Package.swift` is out of scope
for this WP, and WP-11 is editing it concurrently).

Reasons to delete:

- **Nothing can import it.** § 0 established that no file under
  `Sources/VorssaintCore` (or `Sources/Vorssaint`/`Sources/VorssaintMac`,
  which `build.sh` also compiles as one module) may import it without
  breaking `build.sh`. The only target left that *could* import it is
  `VorssaintLinux` — and § 1's guard convention means `VorssaintLinux`
  files don't need to either: they write the same `#if canImport(Darwin)`
  guard as everything else, both because that is now the one convention
  used everywhere in the port (consistency WP-11 is already relying on) and
  because `VorssaintLinux` files that get promoted into `VorssaintCore`
  later (as the Platform-protocol adapters mature) must not need an import
  rewritten out from under them.
- **A module nobody imports is exactly what
  `docs/AI-CONTRIBUTIONS.md`'s "deletion beats addition" rule is about**:
  "Remove the code you added that nothing calls." `VorssaintCombine` was
  scaffolded in WP-10 for a design (`PLAN.md` § 5's original text: `import
  Combine` becomes `import VorssaintCombine`) that WP-13's own lead-narrowed
  brief supersedes with the per-file guard, precisely because of the
  single-module `build.sh` constraint. Keeping an unused target around
  because it was expensive to build is the addition side of that tradeoff,
  not the deletion side.
- **It adds a second place the OpenCombine-vs-Combine switch can drift
  from the real one.** `VorssaintCombine.swift`'s `canImport(Darwin)` guard
  and every file's own `canImport(Darwin)` guard have to agree forever;
  one fewer copy of that logic is one fewer place for it to rot.

Reasons someone might keep it (and why they don't outweigh deletion here):

- It is small (38 lines) and already correct — deleting it is not urgent
  cleanup of a bug, just of a dependency that isn't wired to anything.
- If a future WP decides `VorssaintLinux`-only code (never `VorssaintCore`,
  never touched by `build.sh`) benefits from re-exporting Combine's
  minor-version-independent surface through one seam instead of the
  per-file guard, that WP can resurrect the same 38 lines from git history
  in an afternoon — nothing about deleting it now forecloses that later, and
  no code anywhere currently depends on it existing.

If the lead prefers to keep it "for the Linux-only `VorssaintLinux`
executable" as the WP-13 brief's alternative frames it: note that framing
doesn't hold up on inspection, because `VorssaintLinux` needing Combine at
all is not yet demonstrated (`Sources/VorssaintLinux/main.swift` is a stub
today) and, per § 1, the convention it would need is identical to every
other file's guard regardless of whether `VorssaintCombine` exists — the
module would be an unused alternative path, not a requirement.
