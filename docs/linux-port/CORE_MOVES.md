# WP-11: what moved into `Sources/VorssaintCore`, and what did not

Deliverable of WP-11 (`WORK_PACKAGES.md`). Every claim here is reproducible
from the repository with the commands quoted next to it.

**Result.** `Sources/VorssaintCore` holds **97 files / 44 272 lines** of real
Vorssaint code (96 moved plus the `VorssaintCoreVersion.swift` stub WP-10 left
there) and `swift build --target VorssaintCore` is green on Linux Swift 6.1.3.
`Sources/Vorssaint` keeps 351 files / 148 561 lines, of which 103 275 are
outside `UI/`. The macOS product is the same set of sources compiled into the
same single module: `build.sh` globs `Sources/Vorssaint`,
`Sources/VorssaintCore` and `Sources/VorssaintMac` together, so a file
crossing that boundary changes nothing for the Mac build.

```
$ find Sources/VorssaintCore -name '*.swift' | wc -l
97
$ find Sources/VorssaintCore -name '*.swift' | xargs wc -l | tail -1
  44272 total
$ find Sources/Vorssaint -name '*.swift' | xargs wc -l | tail -1
 148561 total
$ find Sources/Vorssaint -name '*.swift' -not -path 'Sources/Vorssaint/UI/*' \
      | xargs wc -l | tail -1
 103275 total
```

The core is dependency-closed, and the move order it was built from has no
cycle:

```
$ find Sources/VorssaintCore -name '*.swift' | sort > /tmp/core.txt
$ python3 Tools/linux-port/declgraph.py closure --from-file /tmp/core.txt --quiet
seeds:    97 files, 44272 lines
closure:  97 files, 44272 lines
--- unresolved names: 86 (73 outside the Apple prefixes) ---
$ python3 Tools/linux-port/declgraph.py order --from-file /tmp/core.txt | grep -c '^set '
97
```

`closure == seeds` is the whole claim: nothing in `Sources/VorssaintCore`
references a declaration that stayed behind. The 86 unresolved names are all
stdlib and Foundation (`String`, `Data`, `URLComponents`, `NSNumber`,
`JSONDecoder`, `ObservableObject`, …), which the graph does not model.

## 1. The tool: `Tools/linux-port/declgraph.py`

WP-00 condition 1 (`spikes/00-swift-core.md` § 8) is that WP-11 must be
sequenced by the declaration graph, not by feature: 3052 of the spike's 4173
diagnostics were `cannot find X in scope`, because the candidate list in
`WORK_PACKAGES.md` is a feature list and the app has no module boundaries.

`declgraph.py` builds that graph. It is python3 with no dependencies, and it
is lexical rather than a parser: it strips comments and string literals
(keeping `\(...)` interpolation, which is real code), then matches
`enum|struct|class|actor|protocol|typealias` declarations, `extension X` —
counted both as a reference to `X` and as a hard requirement on the file that
declares it — top-level `func`s, and every capitalised identifier plus every
`name(` call. Over-reporting is deliberate: a closure it calls closed really
is closed.

```
declgraph.py decls                    every declaration, by file
declgraph.py closure FILE...          the closure, and the unresolved names
declgraph.py order   FILE...          the closure as closed sets, in move order
declgraph.py rdeps NAME               who references NAME
declgraph.py why FILE --in FILE...    the shortest dependency path to FILE
```

`order` condenses strongly connected components (iterative Tarjan) and emits
them in an order where every set only depends on sets already listed. For the
set moved here it produced **97 singleton sets and no cycle**.

### Verification against the WP-00 census, and the discrepancies

The census (`spikes/wp00-swift-core/census.py`, `files-all.txt`) classified
120 candidate files. Comparing its verdict with what WP-11 actually did:

```
$ python3 Tools/linux-port/core_audit.py census
census rows parsed: 120
agree 102  disagree 18
```

The 18 disagreements are all in one direction — the census says *clean*, the
graph says *not yet movable* — and every one of them is a dependency the
census never looked at, exactly the "~2 % false-clean, the compiler is the
gate" caveat PLAN.md § 4.1 carries. There was **one disagreement the other
way**: `Core/Localization.swift`, classified "needs OpenCombine only", moved
with a five-line import guard and nothing else.

Two further census blind spots the compiler found and the census did not:

| blind spot | evidence |
|---|---|
| `kCG…` constants are lower-case and were not matched as type references. `SessionActivitySupport.swift` looked clean and failed the Linux build on `kCGSessionOnConsoleKey`. | run 34661455499, `error: no such module 'CoreGraphics'`; fixed in `54b1d82` |
| The census counts imports, not APIs. `PointerTapRunLoop.swift` imports only Foundation and uses `CFRunLoopGetCurrent`, `CFMachPortInvalidate` and `NSMachPort`; `ScratchpadSupport.swift` uses `AttributedString(markdown:options:)`. | `core_audit.py hazards`, and the Linux build of run 34661825794 |

The audit that produced all of this is
[`Tools/linux-port/core_audit.py`](../../Tools/linux-port/core_audit.py),
which sits next to `declgraph.py` and uses it:
`hazards` lists API swift-corelibs-foundation lacks, `closed` takes the
greatest fixed point of "portable and all of whose dependencies are portable",
`census` is the comparison above and `globalshortcut` is the proof in § 2.
After the moves, `closed` reports **0 files** — nothing further can move
without one of the three corner extractions in § 4.3:

```
$ python3 Tools/linux-port/core_audit.py closed | head -3
portable by import and API: 35 of 351 files
dependency-closed subset:   0 files, 0 lines
dropped for dependencies:   35 files
```

## 2. The two hub splits

WP-00 condition 2: `Defaults.swift` and `GlobalShortcut.swift` produced 1003
and 1115 diagnostics between them — half the spike census — and everything
else depends on them.

### `Core/Defaults.swift` → split, the key table moved (`a26a64b`)

`enum DefaultsKey` references exactly two names — `String` and itself — and is
the single edge that `Localization.swift` (and through it every
`*Strings.swift`) had into `Defaults.swift`:

```
$ python3 Tools/linux-port/declgraph.py why Sources/Vorssaint/Core/Defaults.swift \
      --in Sources/Vorssaint/Core/Localization.swift
Sources/Vorssaint/Core/Localization.swift -> Sources/Vorssaint/Core/Defaults.swift   (DefaultsKey)
```

So the 664-line table moved verbatim to
`Sources/VorssaintCore/Core/DefaultsKeys.swift` and unlocked 95 more files.

What stayed, and why: the `Defaults` enum around it is the sanitiser hub. Its
1073 lines reference 30 service types — `SwitcherSupport`,
`ScreenshotSupport`, `RecorderSupport`, `MixerRoutingSupport`,
`DockPreviewSupport`, `RadialMenu*`, `MediaImage*`, `FanControl*`,
`WindowGestureSupport`, `SuperKeySupport` and more — plus
`Bundle.main.bundleIdentifier` for the persistent-domain migration and
`GlobalShortcut(keyCode: Int64(kVK_ANSI_Grave), …)` for the Carbon shortcut
defaults. It cannot move until those services have platform protocols
(WP-12). `OnboardingInfo`, `UpdateHighlightsInfo`, `KeepAwakeIconTint`,
`KeepAwakeActiveIcon` and `PreviewSizing` stayed with it: they read
`AppInfo`, `UpdateServiceSupport.SemanticVersion` and `Defaults` itself.

The WP text asks for "the pure `DefaultsKey` table + `register()` defaults".
`Defaults.register()` is 330 lines that register a default for every one of
those 30 sanitisers and build eighteen `GlobalShortcut` defaults out of
`kVK_` constants; it is not separable from `Defaults`, so it stayed. The keys
it registers are the ones now in the core, which is the part WP-12's settings
store actually needs.

### `Core/GlobalShortcut.swift` → **not split**, with proof

The brief allows this file to stay whole "if you prove they are only used by
files that will stay in the Mac layer". That is the case, and the reason is
the one WP-00 named: the *key table* is not portable in this codebase. It is
not a table at all — it is 46 `static let …Default = GlobalShortcut(keyCode:
Int64(kVK_ANSI_K), …)` constants written directly against Carbon, and the
printable-key path (`keyLabel`, and therefore `isValid`, `hasPrintableKey`
and `init?(storageValue:)`) goes through `TISCopyCurrentKeyboardInputSource`
and `UCKeyTranslate`. Replacing the `kVK_` names with literals is a rewrite,
not a mechanical extraction, and would change source the macOS tests pin
(`Tests/MetricsTests.swift:4646` reads this exact path and requires
`TISCopyCurrentASCIICapableKeyboardLayoutInputSource`,
`TISCopyCurrentKeyboardInputSource` and `kTISPropertyInputSourceType` in it).

The proof that splitting it would move nothing into the core — every non-UI
file that references `GlobalShortcut`, and which half of the type it needs:

```
$ python3 Tools/linux-port/core_audit.py globalshortcut | tail -1
27 of 30 non-UI users need the Carbon/CGEvent half; 3 use only the value type,
and all of those are Mac-layer files.
```

27 of the 30 need a `kVK_` default, `carbonKeyCode`/`carbonModifiers`,
`cgFlags`, `syntheticEventFlags`, `matches(event:)`, `displayString`, or
`GlobalShortcut(storageValue:)`, which routes through `keyLabel`. The three
that use only the value type — `App/AppDelegate.swift`,
`Services/CommandBar/CommandBarRowShortcuts.swift` and
`Services/QuitProtection/QuitProtectionService.swift` — are AppKit or service
files that stay in the Mac layer for their own reasons. A split would
therefore be pure churn for WP-11. The `UCKeyTranslate` → xkbcommon work is a
Platform-protocol item the plan already assumes (WP-12, PLAN.md § 4.1).

## 3. The moves, in declaration order

Four commits, all on `claude/linux-desktop-port-plan-smyfba`.

| commit | what | files | edits beyond `git mv` |
|---|---|---:|---|
| `a26a64b` | split `Core/Defaults.swift`, `DefaultsKey` → `Core/DefaultsKeys.swift` | 1 | the split itself; `build.sh --test` path |
| `ecd5546` | `Core/Localization.swift` | 1 | the Combine import guard |
| `2c41ddf` | the localization and strings layer (sets 3–55) | 52 | none — pure `git mv`; 41 `build.sh` paths |
| `6a9ee2e` | the service `Support` layer (sets 56–97) | 43 | one import guard in `RecorderMotion.swift`; 43 `build.sh` paths |
| `54b1d82` | `SessionActivitySupport.swift` back to the Mac layer | 1 | none |
| `e8fbe89` | split the Markdown preview out of `ScratchpadSupport.swift` | 1 | the split itself; `build.sh --test` path |

Two files were split at a platform corner into a `<Original>+Mac.swift`
sibling left under `Sources/Vorssaint` — `Core/Defaults.swift` (the sibling
keeps the original name, since the key table is the part that moved) and
`Services/QuickTools/ScratchpadSupport.swift`. In both the extracted text is
verbatim and the core-side diff is deletions only.

Beyond those two splits, only **three** source edits exist in the whole work
package, and all three are import lines:

1. `Core/Localization.swift` — `L10n` is the one `ObservableObject` in the
   candidate set:
   ```swift
   #if canImport(Darwin)
   import Combine
   #else
   import OpenCombine
   #endif
   ```
   It cannot import `VorssaintCombine`: `build.sh` compiles this directory
   straight into the single macOS app module, where that target does not
   exist (PLAN.md § 5 spells out this exact rule).
2. `Services/Recorder/RecorderMotion.swift` — uses `CGPoint` and `CGRect` and
   nothing else from CoreGraphics. Linux Foundation has the geometry family
   but there is no CoreGraphics *module*, so the import (not the code) is
   wrapped in `#if canImport(CoreGraphics)`.
3. The `DefaultsKey` table's own file header.

Nothing else in any moved file differs by a byte; `git show --stat` on
`2c41ddf` reports `0` changed lines for all 52 renames.

### What is in the core now

`Sources/VorssaintCore/Core` — 38 134 lines: `DefaultsKeys.swift`,
`Localization.swift` (`AppLanguage`, `L10n`, `Strings`), the eleven
`Strings+<Language>.swift` tables, 38 `*Strings.swift` feature string
structs, `AppAppearance.swift`, `QuitProtectionSupport.swift`,
`URLCleaning.swift`.

`Sources/VorssaintCore/Services` — 6 222 lines, 43 files: the
`*Support`/policy/model layer for Audio, Bluetooth, Cleaner, CleaningMode,
Clipboard auto-clear, CommandBar (emoji, file search, math, system
settings), DiskImageInstaller, Display brightness, Finder cut/paste and
rename, KeepAwake automation, KillProcess, LaunchAtLogin, Metrics (battery
time, disk, `MetricFormat`, `MonitorSamplingPolicy`, peripheral batteries,
`SustainedAlertGate`), MiddleClick, MouseAcceleration, MouseNavigation,
QuickTools (mic mute, toggles, scratchpad, screenshot sharing),
`RecorderMotion`, scroll wheel and smooth scroll, snippets, Spotlight names,
sudoers, system-shortcut takeover.

## 4. Everything left behind, with the reason

Three kinds of reason, in order of how many files they hold.

### 4.1 Not portable by import or API

`Services/Audio/MixerRoutingSupport.swift` (CoreAudio),
`Services/AutoQuit/AutoQuitSupport.swift` (ApplicationServices),
`Services/CommandBar/CommandBarSupport.swift` (CryptoKit, Security),
`Services/DockPreview/DockPreviewSupport.swift` (AppKit — the WP already
keeps this one for Mac), `Services/Finder/FinderPasteImageSupport.swift` and
`Services/ManagedDownloads/WhatsAppDownloadSupport.swift`
(UniformTypeIdentifiers), `Services/Homebrew/HomebrewSupport.swift`,
`Services/Metrics/NetworkProcessSupport.swift`,
`Services/Uninstall/UninstallerSupport.swift` (Darwin/libc),
`Services/Media/MediaSupport.swift` (Darwin, ImageIO,
UniformTypeIdentifiers), `Services/QuickTools/QuickToolsSupport.swift`
(AppKit), `Services/QuickTools/ScreenshotSupport.swift` and
`Services/SuperKey/SuperKeySupport.swift` (Carbon),
`Services/RadialMenu/RadialMenuSupport.swift` (AppKit, ImageIO, SwiftUI),
`Services/Switcher/SwitcherSupport.swift` (AppKit),
`Services/Update/UpdateServiceSupport.swift` (CryptoKit),
`App/FeatureRuntime.swift` (AppKit), and the two hub remainders.

CoreGraphics beyond the geometry family, which is the same thing by another
route: `Services/DockClick/DockClickSupport.swift` and
`Services/FocusFollowsMouse/FocusFollowsMouseSupport.swift` (`CGWindowID`),
`Services/MouseClickDebounce/MouseClickDebounceSupport.swift`
(`CGEventType`), `Services/Switcher/SpaceHopSupport.swift` and
`Services/WindowLayout/WindowGestureSupport.swift` (`CGEventFlags`),
`Services/WindowLayout/WindowLayoutSupport.swift` and
`Services/Recorder/RecorderSupport.swift` (`CGWindowID`,
`CGDirectDisplayID`).

Caught by API rather than by import — the census counted only imports, so
these are new findings:

| file | API Linux Foundation does not have |
|---|---|
| `Services/PointerTapRunLoop.swift` | `CFRunLoopGetCurrent`, `CFRunLoopAddSource`, `CFMachPortInvalidate`, `NSMachPort` |
| `Services/QuickTools/ScratchpadSupport+Mac.swift` (split out) | `AttributedString(markdown:options:)` and `PresentationIntent` — found by the Linux compiler, run 34661825794 |
| `Services/SessionActivitySupport.swift` | `kCGSessionOnConsoleKey` (found by the Linux compiler, run 34661455499) |
| `Core/AppInfo.swift`, `Core/ReleaseNotes.swift`, `Services/PrivateFileStore.swift` | `Bundle.main` — present on Linux but meaningless for a SwiftPM binary (WP-00 § 1 marks this `!`) |
| `Core/LiquidGlassSupport.swift` | `#available(macOS 26, *)`, which is not false on a non-Darwin platform |
| `Services/Metrics/SpeedTest.swift` | `URLSession` (Linux needs `FoundationNetworking`) and `CFAbsoluteTimeGetCurrent` |
| `Services/FanControl/FanControlSupport.swift` | `ProcessInfo.ThermalState` — one of the four gaps WP-00 § 8 condition 3 hands to WP-12 |
| `Services/Metrics/TemperatureSensorSelector.swift`, `Services/DetachedProcess.swift` | `import Darwin` + `sysctlbyname` / `posix_spawn` |
| `Services/AppUpdates/AppUpdateFeedSupport.swift` | `XMLParser` → `FoundationXML`, the other named WP-12 gap |

Per the brief these are WP-12's to close, not WP-11's to shim.

### 4.2 Census-clean, but not dependency-closed

These are the 18 census disagreements. Each is blocked by a file in § 4.1.

| file | lines | blocked by |
|---|---:|---|
| `Core/FeatureCatalog.swift` | 423 | `RadialMenuSupport`, `SuperKeySupport`, `WindowGestureSupport` |
| `Core/FeaturePresets.swift` | 135 | `FeatureCatalog`, `RadialMenuSupport`, `WindowGestureSupport` |
| `Core/SettingsBackupSupport.swift` | 329 | `Defaults`, `FeatureCatalog`, `MediaSupport` |
| `Core/MouseExceptionStrings.swift` | 234 | `MouseAppExceptionSupport` |
| `Core/SuperKeyStrings.swift` | 362 | `SuperKeySupport` |
| `Services/MouseExceptions/MouseAppExceptionSupport.swift` | 218 | `FeatureCatalog` |
| `Services/MouseButtons/MouseButtonShortcutSupport.swift` | 197 | `FeatureCatalog`, `GlobalShortcut`, `RadialMenuSupport` |
| `Services/MouseButtons/MouseSpacesGestureSupport.swift` | 168 | `MouseButtonShortcutSupport` |
| `Services/Clipboard/ClipboardHistorySupport.swift` | 570 | `ScreenshotSupport`, `UI/Settings/SettingsSearchSupport` |
| `Services/AppUpdates/AppUpdateFeedSupport.swift` | 222 | `JunkCleaner`, `InstalledApps`, `ShelfService`, `UI/Recorder/RecorderZoomLane` |
| `Services/AppUpdates/AppUpdatesSupport.swift` | 672 | `AppUpdateFeedSupport`, `HomebrewSupport` |
| `Services/Shelf/ShelfSupport.swift` | 696 | `AppUpdatesSupport`, `JunkCleaner`, `ShelfService`, two UI files |
| `Services/Recorder/RecorderTimeline.swift` | 322 | `RecorderSupport` |
| `Services/Recorder/RecordingSharingSupport.swift` | 141 | `RecorderSupport` |
| `Services/FanControl/FanControlSupport.swift` | 479 | `TemperatureSensorSelector` (and `ProcessInfo.ThermalState`) |
| `Services/KeyboardDebounce/KeyboardDebounceSupport.swift` | 217 | `Defaults` |
| `Services/Update/UpdateInstallerSupport.swift` | 242 | `DetachedProcess` |
| `Services/CommandBar/{Dates,Units,QueryMemory,Preferences}.swift` | 971 | `CommandBarSearch`, inside `CommandBarSupport` |

### 4.3 The three corners that would unlock the most, and why WP-11 did not cut them

Each of these is one small platform call sitting inside an otherwise pure
file, and each of them needs a *seam* — a protocol or an injected closure —
rather than a mechanical extraction. The brief forbids logic edits, and a
seam changes call order and default behaviour, so all three are handed to
WP-12 with the exact line named.

| corner | where | what it unlocks |
|---|---|---|
| `NSImage(data: customData) == nil`, one custom-icon validity check inside `RadialMenuSupport.sanitized`, which `RadialMenuSupport.decode` calls | `Services/RadialMenu/RadialMenuSupport.swift:666` | `FeatureCatalog`, `FeaturePresets`, `MouseAppExceptionSupport`, `MouseExceptionStrings`, `MouseButtonShortcutSupport`, `MouseSpacesGestureSupport` — and through `FeatureCatalog`, `SettingsBackupSupport` |
| `CFStringTransform(…, kCFStringTransformMandarinLatin, …)` in `CommandBarSearch.pinyinKeywords`, called by `CommandBarSearch.applicationKeywords` | `Services/CommandBar/CommandBarSupport.swift:213-225` | `CommandBarDates`, `CommandBarUnits`, `CommandBarQueryMemory`, `CommandBarPreferences` (971 lines) |
| `Set<CGWindowID>` and `struct Region { let displayID: CGDirectDisplayID … }` in `RecorderSupport` | `Services/Recorder/RecorderSupport.swift:228-241` | `RecorderTimeline`, `RecordingSharingSupport`, `RecorderBlurRegion`, `RecorderImageOverlay`, `RecorderTextOverlay` |

Three genuinely cheap corners were measured and found to unlock nothing on
their own, so they were not cut either: `SuperKeySource.keyCode` (7 lines of
`kVK_` in `SuperKeySupport.swift`), `enum WindowGestureSupport` (the only
`CGEventFlags` user in `WindowGestureSupport.swift`) and
`WindowLayoutWindowKey` (the only `CGWindowID` user in
`WindowLayoutSupport.swift`). The first two are behind `FeatureCatalog`, and
the `WindowLayout`/`WindowGesture` pair is behind `GlobalShortcut` — every
`WindowLayoutAction.defaultShortcut` is a `kVK_`-built constant.

## 5. Consequence for the macOS test suite (for WP-16)

`Tests/` is out of WP-11's scope and was not touched, but two source-scanning
checks in `Tests/MetricsTests.swift` enumerate `Sources/Vorssaint/Core` and
`Sources/Vorssaint/Core/Localizations` by directory:

- line 13547, the "visible text curls its apostrophes" sweep;
- line 13582, the French non-breaking-space sweep.

Both still pass — they collect violations and assert the collection is empty —
but they now read **zero** files, because every `*Strings.swift`,
`Strings+*.swift` and `Localization.swift` is under
`Sources/VorssaintCore/Core`. They are vacuous, not red. WP-16 should widen
both walks to `Sources` (the other walks in the file already use `Sources`
or `Sources/Vorssaint` deliberately and are unaffected — they hunt
`CGEvent.tapCreate` and `waitUntilAllOperationsAreFinished`, which only exist
in files that stayed).

The four files pinned by exact path in `Tests/` that are also core candidates
were deliberately not moved on that account:
`Services/Clipboard/ClipboardHistorySupport.swift`,
`Services/Switcher/SwitcherSupport.swift`,
`Services/CommandBar/CommandBarSupport.swift` (line 24781, which asserts each
reads back non-empty) and `Core/GlobalShortcut.swift` (line 4646). All four
are blocked for independent reasons above, so nothing was compromised to
satisfy the tests.

## 6. CI

`.github/workflows/linux-port-ci.yml`, both jobs hard gates, on every push to
the branch.

| push | run | `linux-core` | `macos` |
|---|---|---|---|
| 1 | [34661455499](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34661455499) | **red** — one file, see below | *(see § 6.2)* |
| 2 | [34661825794](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34661825794) | *(filled in below)* | *(filled in below)* |

### 6.1 What the first run proved

The whole 226-file core target compiled with exactly **one** distinct error,
and it was the one file the lexical scan had mis-classified:

```
[226/226] Compiling VorssaintCore VorssaintCoreVersion.swift
/__w/vorssaint-utils/vorssaint-utils/Sources/VorssaintCore/Services/SessionActivitySupport.swift:4:8: error: no such module 'CoreGraphics'
 4 | import CoreGraphics
   |        `- error: no such module 'CoreGraphics'
```

Nothing else in 44 000 lines failed. In particular `AttributedString` markdown
parsing and `PresentationIntent` (`ScratchpadSupport.swift`),
`String.applyingTransform(StringTransform("Any-Name"))`
(`CommandBarEmoji.swift`), `ISO8601DateFormatter`,
`PropertyListSerialization`, `JSONSerialization`, `NSNumber`/`NSDictionary`
bridging and the POSIX `EACCES`/`ENOENT`/`EPERM` constants
(`CutPastePrivilegeSupport.swift`) all compile on swift-corelibs-foundation
6.1.3 — which is new information the WP-00 spike could not get, because its
build aborted on unresolved in-repo names.

## Appendix: every file now in `Sources/VorssaintCore`

97 files. The path under `VorssaintCore/` is the path the file had
under `Vorssaint/`, so the feature grouping is unchanged (PLAN.md § 5).
`git log --follow` works on every one of them.

| path under `Sources/VorssaintCore/` | lines | moved in |
|---|---:|---|
| `Core/AppAppearance.swift` | 31 | `2c41ddf` |
| `Core/AppUpdateStrings.swift` | 629 | `2c41ddf` |
| `Core/AppearanceStrings.swift` | 138 | `2c41ddf` |
| `Core/BackupStrings.swift` | 199 | `2c41ddf` |
| `Core/BatteryTimeStrings.swift` | 110 | `2c41ddf` |
| `Core/BluetoothSleepStrings.swift` | 169 | `2c41ddf` |
| `Core/BrightnessStrings.swift` | 421 | `2c41ddf` |
| `Core/CameraPreviewStrings.swift` | 195 | `2c41ddf` |
| `Core/ClipboardIgnoredAppsStrings.swift` | 124 | `2c41ddf` |
| `Core/CommandBarStrings.swift` | 2270 | `2c41ddf` |
| `Core/DefaultsKeys.swift` | 675 | `a26a64b` (split) |
| `Core/DiskExclusionStrings.swift` | 152 | `2c41ddf` |
| `Core/DiskImageInstallerStrings.swift` | 306 | `2c41ddf` |
| `Core/FanControlStrings.swift` | 656 | `2c41ddf` |
| `Core/FeatureHubStrings.swift` | 1590 | `2c41ddf` |
| `Core/FeatureStrings.swift` | 2534 | `2c41ddf` |
| `Core/FeedbackStrings.swift` | 474 | `2c41ddf` |
| `Core/FinderRenameStrings.swift` | 152 | `2c41ddf` |
| `Core/KeepAwakeStrings.swift` | 389 | `2c41ddf` |
| `Core/KillProcessStrings.swift` | 491 | `2c41ddf` |
| `Core/Localization.swift` | 3201 | `ecd5546` |
| `Core/Localizations/Strings+ChineseSimplified.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+ChineseTraditionalHK.swift` | 1015 | `2c41ddf` |
| `Core/Localizations/Strings+ChineseTraditionalTW.swift` | 1015 | `2c41ddf` |
| `Core/Localizations/Strings+French.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+German.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+Italian.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+Japanese.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+Korean.swift` | 1015 | `2c41ddf` |
| `Core/Localizations/Strings+Russian.swift` | 1015 | `2c41ddf` |
| `Core/Localizations/Strings+Spanish.swift` | 1014 | `2c41ddf` |
| `Core/Localizations/Strings+Turkish.swift` | 1014 | `2c41ddf` |
| `Core/MediaImageStrings.swift` | 850 | `2c41ddf` |
| `Core/MenuBarAppearanceStrings.swift` | 208 | `2c41ddf` |
| `Core/MouseButtonStrings.swift` | 520 | `2c41ddf` |
| `Core/MouseClickDebounceStrings.swift` | 138 | `2c41ddf` |
| `Core/PermissionGuideStrings.swift` | 217 | `2c41ddf` |
| `Core/QuickToggleStrings.swift` | 421 | `2c41ddf` |
| `Core/QuitProtectionStrings.swift` | 478 | `2c41ddf` |
| `Core/QuitProtectionSupport.swift` | 166 | `2c41ddf` |
| `Core/RadialMenuStrings.swift` | 1426 | `2c41ddf` |
| `Core/RecentCaptureStrings.swift` | 180 | `2c41ddf` |
| `Core/RecorderShareStrings.swift` | 194 | `2c41ddf` |
| `Core/RecorderStrings.swift` | 1959 | `2c41ddf` |
| `Core/ScratchpadStrings.swift` | 517 | `2c41ddf` |
| `Core/ScreenshotStrings.swift` | 2141 | `2c41ddf` |
| `Core/ShortcutSettingsStrings.swift` | 110 | `2c41ddf` |
| `Core/SnippetStrings.swift` | 927 | `2c41ddf` |
| `Core/SwitcherAppRulesStrings.swift` | 180 | `2c41ddf` |
| `Core/URLCleaning.swift` | 284 | `2c41ddf` |
| `Core/WhatsAppDownloadStrings.swift` | 505 | `2c41ddf` |
| `Core/WhatsAppOrganizerStrings.swift` | 486 | `2c41ddf` |
| `Core/WindowDirectionalStrings.swift` | 25 | `2c41ddf` |
| `Core/WindowPreviewExclusionStrings.swift` | 138 | `2c41ddf` |
| `Services/Audio/MusicLaunchSupport.swift` | 45 | `6a9ee2e` |
| `Services/Audio/PreciseVolumeRollerSupport.swift` | 68 | `6a9ee2e` |
| `Services/Bluetooth/BluetoothSleepSupport.swift` | 33 | `6a9ee2e` |
| `Services/Cleaner/CleanerPolicy.swift` | 97 | `6a9ee2e` |
| `Services/Cleaner/CleanerSchedule.swift` | 74 | `6a9ee2e` |
| `Services/Cleaner/CleanerSupport.swift` | 241 | `6a9ee2e` |
| `Services/CleaningMode/CleaningUnlockCounter.swift` | 86 | `6a9ee2e` |
| `Services/Clipboard/ClipboardAutoClearSupport.swift` | 42 | `6a9ee2e` |
| `Services/CommandBar/CommandBarEmoji.swift` | 155 | `6a9ee2e` |
| `Services/CommandBar/CommandBarFileSearchSupport.swift` | 212 | `6a9ee2e` |
| `Services/CommandBar/CommandBarMath.swift` | 348 | `6a9ee2e` |
| `Services/CommandBar/CommandBarSystemSettingsSupport.swift` | 114 | `6a9ee2e` |
| `Services/DiskImageInstaller/DiskImageInstallerSupport.swift` | 70 | `6a9ee2e` |
| `Services/Display/BrightnessSupport.swift` | 479 | `6a9ee2e` |
| `Services/Display/ExtraBrightnessSupport.swift` | 167 | `6a9ee2e` |
| `Services/Finder/CutPastePrivilegeSupport.swift` | 48 | `6a9ee2e` |
| `Services/Finder/CutPasteProgressSupport.swift` | 38 | `6a9ee2e` |
| `Services/Finder/FinderRenameSupport.swift` | 15 | `6a9ee2e` |
| `Services/GeneralPasteboardAccess.swift` | 35 | `6a9ee2e` |
| `Services/KeepAwakeAutomationSupport.swift` | 66 | `6a9ee2e` |
| `Services/KillProcess/KillProcessSupport.swift` | 77 | `6a9ee2e` |
| `Services/LaunchAtLoginSupport.swift` | 53 | `6a9ee2e` |
| `Services/Metrics/BatteryTimeSupport.swift` | 22 | `6a9ee2e` |
| `Services/Metrics/DiskSupport.swift` | 161 | `6a9ee2e` |
| `Services/Metrics/MetricFormat.swift` | 433 | `6a9ee2e` |
| `Services/Metrics/MonitorSamplingPolicy.swift` | 100 | `6a9ee2e` |
| `Services/Metrics/PeripheralBatterySupport.swift` | 341 | `6a9ee2e` |
| `Services/Metrics/SustainedAlertGate.swift` | 44 | `6a9ee2e` |
| `Services/MiddleClick/MiddleClickSupport.swift` | 96 | `6a9ee2e` |
| `Services/MouseAcceleration/MouseAccelerationSupport.swift` | 128 | `6a9ee2e` |
| `Services/MouseNavigation/MouseNavigationSupport.swift` | 126 | `6a9ee2e` |
| `Services/QuickTools/MicMuteSupport.swift` | 51 | `6a9ee2e` |
| `Services/QuickTools/QuickTogglesSupport.swift` | 103 | `6a9ee2e` |
| `Services/QuickTools/ScratchpadSupport.swift` | 225 | `6a9ee2e` |
| `Services/QuickTools/ScreenshotSharingSupport.swift` | 106 | `6a9ee2e` |
| `Services/Recorder/RecorderMotion.swift` | 661 | `6a9ee2e` |
| `Services/ScrollWheelSupport.swift` | 77 | `6a9ee2e` |
| `Services/SmoothScrollSupport.swift` | 195 | `6a9ee2e` |
| `Services/Snippets/TextSnippetSupport.swift` | 546 | `6a9ee2e` |
| `Services/SpotlightNamesSupport.swift` | 49 | `6a9ee2e` |
| `Services/SudoersSupport.swift` | 20 | `6a9ee2e` |
| `Services/SystemShortcutTakeoverSupport.swift` | 58 | `6a9ee2e` |
| `VorssaintCoreVersion.swift` | 33 | WP-10 (stub, not moved) |
