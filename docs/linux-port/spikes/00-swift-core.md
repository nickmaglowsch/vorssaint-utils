# WP-00 spike: the Vorssaint core on Linux Swift

Answers the Phase 0 question "can the Foundation-only part of the Swift core
be compiled on Linux, or does the plan fall back to a Rust core"
(`PLAN.md` § 9).

**Verdict: GO WITH CONDITIONS.** The conditions are in the last section.

Spike code: `spikes/wp00-swift-core/` (`README.md` there explains the
targets). Everything below is reproducible from that directory.

## 0. How this was measured, and what could not be

No Swift toolchain can be installed in the port containers: the egress policy
returns 403 for `swift.org` and for GitHub release downloads, and `apt` has no
Swift. The WP text asks for Ubuntu **and** Fedora; only Ubuntu 24.04 was
reachable, through the official `swift:6.1-noble` container on GitHub Actions,
plus `swiftlang/swift:nightly-6.2-noble` as a second job. **Fedora was not
tested.** Nothing in the findings below is distribution-specific (they are all
properties of swift-corelibs-foundation, not of the distro), but that is an
argument, not evidence.

The measurement is therefore in two parts: a static census run locally, and
four real compiles on GitHub Actions.

## 1. Static census

`spikes/wp00-swift-core/census.py` over the 120 candidate files named in
`WORK_PACKAGES.md` (the twelve named files, every
`Services/*/*Support.swift`, every `Core/*Strings.swift`, every
`Core/Localizations/*.swift`, `FeaturePresets.swift`,
`SettingsBackupSupport.swift`, `App/FeatureRuntime.swift`). The file list is
produced by `candidates.sh` and stored as `files-all.txt`.

```
$ ./spikes/wp00-swift-core/census.py
Counter({'clean': 89, 'not portable': 19, 'needs small extraction': 7,
         'clean (CoreGraphics types only)': 4, 'needs OpenCombine only': 1})
files 120 lines 63956
Counter({'Foundation': 116, 'CoreGraphics': 18, 'AppKit': 6,
         'Carbon.HIToolbox': 4, 'Darwin': 4, 'UniformTypeIdentifiers': 3,
         'CryptoKit': 2, 'ImageIO': 2, 'Combine': 1, 'CoreAudio': 1,
         'ApplicationServices': 1, 'Security': 1, 'SwiftUI': 1})
```

### Totals

| classification | files | lines | share of lines |
|---|---:|---:|---:|
| clean (Foundation only) | 89 | 42 858 | 67.0 % |
| clean (plus the `CGFloat/CGPoint/CGSize/CGRect` family) | 4 | 849 | 1.3 % |
| needs OpenCombine only | 1 | 3 193 | 5.0 % |
| needs small extraction | 7 | 2 867 | 4.5 % |
| not portable | 19 | 14 189 | 22.2 % |
| **total** | **120** | **63 956** | |

93 of 120 files (78 %, 68 % of the lines) carry nothing that Linux Foundation
lacks. Only **one** file in the whole candidate set imports Combine.

Notation in the table below: a trailing `*` marks a symbol that exists on
Linux and is listed only because the WP asked for it (`UserDefaults`,
`ProcessInfo`, `FileManager`, `DispatchQueue`, `@MainActor`); `!` marks one
that exists but is meaningless for a SwiftPM executable (`Bundle.main`); `!!`
would mark one that is missing outright (`FileManager.trashItem` — **not used
anywhere in the candidate set**). `NSHomeDirectory`, `NSCocoaErrorDomain`,
`NSPOSIXErrorDomain`, `NSUnderlyingErrorKey`, `NSFileRead*Error`, `NSRect`,
`NSSize` and `NSPoint` are all present in swift-corelibs-foundation and are
therefore not counted as blockers.

### Per-file census

<!-- Generated: ./spikes/wp00-swift-core/census.py -->

| file | lines | imports beyond Foundation | Linux-blocking symbols | classification |
|---|---:|---|---|---|
| `Core/AppUpdateStrings.swift` | 629 | — | — | clean |
| `Core/AppearanceStrings.swift` | 138 | — | — | clean |
| `Core/BackupStrings.swift` | 199 | — | — | clean |
| `Core/BatteryTimeStrings.swift` | 110 | — | — | clean |
| `Core/BluetoothSleepStrings.swift` | 169 | — | — | clean |
| `Core/BrightnessStrings.swift` | 421 | — | — | clean |
| `Core/CameraPreviewStrings.swift` | 195 | — | — | clean |
| `Core/ClipboardIgnoredAppsStrings.swift` | 124 | — | — | clean |
| `Core/CommandBarStrings.swift` | 2270 | — | — | clean |
| `Core/DiskExclusionStrings.swift` | 152 | — | — | clean |
| `Core/DiskImageInstallerStrings.swift` | 306 | — | — | clean |
| `Core/FanControlStrings.swift` | 656 | — | — | clean |
| `Core/FeatureCatalog.swift` | 423 | — | UserDefaults* | clean |
| `Core/FeatureHubStrings.swift` | 1590 | — | — | clean |
| `Core/FeaturePresets.swift` | 135 | — | UserDefaults* | clean |
| `Core/FeatureStrings.swift` | 2534 | — | — | clean |
| `Core/FeedbackStrings.swift` | 474 | — | — | clean |
| `Core/FinderRenameStrings.swift` | 152 | — | — | clean |
| `Core/KeepAwakeStrings.swift` | 389 | — | — | clean |
| `Core/KillProcessStrings.swift` | 491 | — | — | clean |
| `Core/Localizations/Strings+ChineseSimplified.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+ChineseTraditionalHK.swift` | 1015 | — | — | clean |
| `Core/Localizations/Strings+ChineseTraditionalTW.swift` | 1015 | — | — | clean |
| `Core/Localizations/Strings+French.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+German.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+Italian.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+Japanese.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+Korean.swift` | 1015 | — | — | clean |
| `Core/Localizations/Strings+Russian.swift` | 1015 | — | — | clean |
| `Core/Localizations/Strings+Spanish.swift` | 1014 | — | — | clean |
| `Core/Localizations/Strings+Turkish.swift` | 1014 | — | — | clean |
| `Core/MediaImageStrings.swift` | 850 | — | — | clean |
| `Core/MenuBarAppearanceStrings.swift` | 208 | — | — | clean |
| `Core/MouseButtonStrings.swift` | 520 | — | — | clean |
| `Core/MouseClickDebounceStrings.swift` | 138 | — | — | clean |
| `Core/MouseExceptionStrings.swift` | 234 | — | — | clean |
| `Core/PermissionGuideStrings.swift` | 217 | — | — | clean |
| `Core/QuickToggleStrings.swift` | 421 | — | — | clean |
| `Core/QuitProtectionStrings.swift` | 478 | — | — | clean |
| `Core/RadialMenuStrings.swift` | 1426 | — | — | clean |
| `Core/RecentCaptureStrings.swift` | 180 | — | — | clean |
| `Core/RecorderShareStrings.swift` | 194 | — | — | clean |
| `Core/RecorderStrings.swift` | 1959 | — | — | clean |
| `Core/ScratchpadStrings.swift` | 517 | — | — | clean |
| `Core/ScreenshotStrings.swift` | 2141 | — | — | clean |
| `Core/SettingsBackupSupport.swift` | 329 | — | — | clean |
| `Core/ShortcutSettingsStrings.swift` | 110 | — | — | clean |
| `Core/SnippetStrings.swift` | 927 | — | — | clean |
| `Core/SuperKeyStrings.swift` | 362 | — | — | clean |
| `Core/SwitcherAppRulesStrings.swift` | 180 | — | — | clean |
| `Core/URLCleaning.swift` | 284 | — | — | clean |
| `Core/WhatsAppDownloadStrings.swift` | 505 | — | — | clean |
| `Core/WhatsAppOrganizerStrings.swift` | 486 | — | — | clean |
| `Core/WindowDirectionalStrings.swift` | 25 | — | — | clean |
| `Core/WindowPreviewExclusionStrings.swift` | 138 | — | — | clean |
| `Services/AppUpdates/AppUpdateFeedSupport.swift` | 222 | — | — | clean |
| `Services/AppUpdates/AppUpdatesSupport.swift` | 672 | — | — | clean |
| `Services/Audio/MusicLaunchSupport.swift` | 45 | — | — | clean |
| `Services/Audio/PreciseVolumeRollerSupport.swift` | 68 | — | — | clean |
| `Services/Bluetooth/BluetoothSleepSupport.swift` | 33 | — | — | clean |
| `Services/Cleaner/CleanerSupport.swift` | 241 | — | — | clean |
| `Services/Clipboard/ClipboardAutoClearSupport.swift` | 42 | — | — | clean |
| `Services/Clipboard/ClipboardHistorySupport.swift` | 570 | — | FileManager* | clean |
| `Services/CommandBar/CommandBarFileSearchSupport.swift` | 212 | — | — | clean |
| `Services/CommandBar/CommandBarMath.swift` | 348 | — | — | clean |
| `Services/CommandBar/CommandBarSystemSettingsSupport.swift` | 114 | — | — | clean |
| `Services/DiskImageInstaller/DiskImageInstallerSupport.swift` | 70 | — | FileManager* | clean |
| `Services/Display/BrightnessSupport.swift` | 479 | — | — | clean |
| `Services/Display/ExtraBrightnessSupport.swift` | 167 | — | — | clean |
| `Services/FanControl/FanControlSupport.swift` | 479 | — | ProcessInfo* | clean |
| `Services/Finder/CutPastePrivilegeSupport.swift` | 48 | — | FileManager* | clean |
| `Services/Finder/CutPasteProgressSupport.swift` | 38 | — | — | clean |
| `Services/Finder/FinderRenameSupport.swift` | 15 | — | — | clean |
| `Services/KeyboardDebounce/KeyboardDebounceSupport.swift` | 217 | — | — | clean |
| `Services/KillProcess/KillProcessSupport.swift` | 77 | — | ProcessInfo* | clean |
| `Services/Metrics/BatteryTimeSupport.swift` | 22 | — | — | clean |
| `Services/Metrics/DiskSupport.swift` | 161 | — | — | clean |
| `Services/Metrics/PeripheralBatterySupport.swift` | 341 | — | — | clean |
| `Services/MiddleClick/MiddleClickSupport.swift` | 96 | — | — | clean |
| `Services/MouseAcceleration/MouseAccelerationSupport.swift` | 128 | — | — | clean |
| `Services/MouseButtons/MouseButtonShortcutSupport.swift` | 197 | — | UserDefaults* | clean |
| `Services/MouseNavigation/MouseNavigationSupport.swift` | 126 | — | — | clean |
| `Services/QuickTools/MicMuteSupport.swift` | 51 | — | — | clean |
| `Services/QuickTools/QuickTogglesSupport.swift` | 103 | — | — | clean |
| `Services/QuickTools/ScratchpadSupport.swift` | 342 | — | — | clean |
| `Services/QuickTools/ScreenshotSharingSupport.swift` | 106 | — | — | clean |
| `Services/Shelf/ShelfSupport.swift` | 696 | — | — | clean |
| `Services/Snippets/TextSnippetSupport.swift` | 546 | — | — | clean |
| `Services/Update/UpdateInstallerSupport.swift` | 242 | — | — | clean |
| `Services/MouseButtons/MouseSpacesGestureSupport.swift` | 168 | CoreGraphics | — | clean (CoreGraphics types only) |
| `Services/MouseExceptions/MouseAppExceptionSupport.swift` | 218 | CoreGraphics | Bundle | clean (CoreGraphics types only) |
| `Services/Recorder/RecorderTimeline.swift` | 322 | CoreGraphics | — | clean (CoreGraphics types only) |
| `Services/Recorder/RecordingSharingSupport.swift` | 141 | CoreGraphics | — | clean (CoreGraphics types only) |
| `Core/Localization.swift` | 3193 | Combine | @Published, ObservableObject, import Combine, UserDefaults* | needs OpenCombine only |
| `Services/DockClick/DockClickSupport.swift` | 289 | CoreGraphics | CGWindowID | needs small extraction |
| `Services/FocusFollowsMouse/FocusFollowsMouseSupport.swift` | 104 | CoreGraphics | CGWindowID | needs small extraction |
| `Services/MouseClickDebounce/MouseClickDebounceSupport.swift` | 132 | CoreGraphics | CGEventType | needs small extraction |
| `Services/Recorder/RecorderSupport.swift` | 764 | CoreGraphics | CGDirectDisplayID, CGWindowID, FileManager* | needs small extraction |
| `Services/Switcher/SpaceHopSupport.swift` | 87 | CoreGraphics | CGEventFlags | needs small extraction |
| `Services/WindowLayout/WindowGestureSupport.swift` | 642 | CoreGraphics | CGEventFlags, UserDefaults*, #available(macOS) | needs small extraction |
| `Services/WindowLayout/WindowLayoutSupport.swift` | 849 | CoreGraphics | CGWindowID, UserDefaults* | needs small extraction |
| `App/FeatureRuntime.swift` | 320 | AppKit | NSApp, @Published, ObservableObject, Bundle.main!, ProcessInfo*, UserDefaults* | not portable |
| `Core/Defaults.swift` | 1897 | Carbon.HIToolbox, CoreGraphics | Bundle.main!, UserDefaults* | not portable |
| `Core/GlobalShortcut.swift` | 1095 | AppKit, Carbon.HIToolbox, CoreGraphics | NSEvent, CGEvent, CGEventFlags, UserDefaults* | not portable |
| `Services/Audio/MixerRoutingSupport.swift` | 570 | CoreAudio | — | not portable |
| `Services/AutoQuit/AutoQuitSupport.swift` | 160 | ApplicationServices | Bundle | not portable |
| `Services/CommandBar/CommandBarSupport.swift` | 1081 | CryptoKit, Security | Bundle.main!, UserDefaults*, DispatchQueue* | not portable |
| `Services/DockPreview/DockPreviewSupport.swift` | 576 | AppKit, CoreGraphics | NSEvent, CGWindowID | not portable |
| `Services/Finder/FinderPasteImageSupport.swift` | 29 | UniformTypeIdentifiers | — | not portable |
| `Services/Homebrew/HomebrewSupport.swift` | 881 | Darwin | ProcessInfo* | not portable |
| `Services/ManagedDownloads/WhatsAppDownloadSupport.swift` | 227 | UniformTypeIdentifiers | UserDefaults* | not portable |
| `Services/Media/MediaSupport.swift` | 898 | CoreGraphics, Darwin, ImageIO, UniformTypeIdentifiers | CGImage, CGImageSourceCopyPropertiesAtIndex, CGImageSourceCreateThumbnailAtIndex, CGImageSourceCreateWithURL, FileManager* | not portable |
| `Services/Metrics/NetworkProcessSupport.swift` | 213 | Darwin | — | not portable |
| `Services/QuickTools/QuickToolsSupport.swift` | 255 | AppKit | NSBitmapImageRep, NSColor, CGImage | not portable |
| `Services/QuickTools/ScreenshotSupport.swift` | 2068 | Carbon.HIToolbox, CoreGraphics | CGMutablePath, CGPath, Bundle, FileManager*, UserDefaults* | not portable |
| `Services/RadialMenu/RadialMenuSupport.swift` | 1098 | AppKit, CoreGraphics, ImageIO, SwiftUI | NSBitmapImageRep, NSGraphicsContext, NSImage, CGImageSourceCopyPropertiesAtIndex, CGImageSourceCreateWithData, SwiftUI.Color, UserDefaults*, DispatchQueue* | not portable |
| `Services/SuperKey/SuperKeySupport.swift` | 402 | Carbon.HIToolbox | — | not portable |
| `Services/Switcher/SwitcherSupport.swift` | 1602 | AppKit, CoreGraphics | NSFont, CGColorSpaceCreateDeviceRGB, CGContext, CGEventType, CGImage, CGImageAlphaInfo, CGWindowID, ProcessInfo* | not portable |
| `Services/Uninstall/UninstallerSupport.swift` | 645 | Darwin | — | not portable |
| `Services/Update/UpdateServiceSupport.swift` | 172 | CryptoKit | — | not portable |

## 2. Real compilation

Workflow: `.github/workflows/linux-spike-wp00.yml`. Every build step is
`continue-on-error: true`, so the spike never fails the branch.

- Run 1 — <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34653318777>
- Run 2 — <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34653604112>
- Run 3 — <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34653933762>
- **Run 4 (final) — <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34654442724>**
  - swift 6.1 job: <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34654442724/job/103443661503>
  - swift 6.2 nightly job: <https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34654442724/job/103443661427>

Toolchain, from the job log:

```
Swift version 6.1.3 (swift-6.1.3-RELEASE)
Target: x86_64-unknown-linux-gnu
Linux 07cfb9dd6ac7 6.17.0-1022-azure #22-Ubuntu SMP ... x86_64 GNU/Linux
PRETTY_NAME="Ubuntu 24.04.4 LTS"
```

The nightly job runs `swiftlang/swift:nightly-6.2-noble`; its testing library
reports `6.2.3 (48a471ab313e858)`. Both tags exist and both were pulled
successfully.

### Targets

| target | contents | why |
|---|---|---|
| `VorssaintCoreSpike` | 103 files: the 101 the census does not call "not portable", plus `Defaults.swift` and `GlobalShortcut.swift` | the census-clean set alone does not resolve — see below |
| `VorssaintCoreWide` | all 120 census files | the full error census |
| `VorssaintServices` | 208 files: everything under `Sources/Vorssaint` outside `UI/` that does not import SwiftUI/AppKit/Cocoa | the only near-dependency-closed set |
| `VorssaintCoreMinimal` | `URLCleaning.swift`, `CommandBarMath.swift` | a genuinely closed set; the tests run here |
| `hello-spike` | 30 lines of Foundation | the static-link probe |

Apple frameworks are shimmed as ordinary SwiftPM targets **named after the
framework** (`Sources/CoreGraphicsShim` builds as module `CoreGraphics`, and
so on), so the vendored sources keep their unmodified `import`. `sync.sh`
performs exactly one source rewrite — `import Carbon.HIToolbox` →
`import Carbon`, because a SwiftPM target name cannot contain a dot — and
that is the whole extent of the modification.

## 3. Error census per iteration

Four push cycles were used; the WP allows four.

| iteration | run | what changed | `VorssaintCoreSpike` diagnostics |
|---|---|---|---|
| 1 | 34653318777 | first build of the 93 census-clean files | drowned in `cannot find type 'AppLanguage' in scope` — the census-clean list is a *classification*, not a dependency closure: every `*Strings.swift` needs `AppLanguage` from `Localization.swift` (classified "needs OpenCombine only") and several `*Support.swift` need `DefaultsKey` from `Defaults.swift` (classified "not portable"). Also: `swift test` died on `circular dependency between modules 'VorssaintCombine' and 'Combine'`, and `${PIPESTATUS[0]}` was a `Bad substitution` because the container shell is `sh`. |
| 2 | 34653604112 | set closed to 103 files; `canImport(Darwin)` instead of `canImport(Combine)`; `shell: bash` | **4173 raw, 126 distinct** |
| 3 | 34653933762 | added `VorssaintServices` and `VorssaintCoreMinimal`; better classifier bins | 4173 raw, 126 distinct (unchanged — the change was to the other targets) |
| 4 | 34654442724 | raw log to artifact instead of the job log; tests split into their own package | **4173 raw, 126 distinct** (below) |

### Final census, `VorssaintCoreSpike` (103 files), swift 6.1.3

```
swift build exit=1
=== error census for build.log ===
raw diagnostics:    4173
distinct messages:  126

--- by category (raw diagnostics) ---
Combine/OpenCombine             0
CoreGraphics types            149
AppKit leak                     0
Foundation gap                259
Swift 6 concurrency             0
unresolved in-repo           3052
Darwin/libc                     0
circular/module                 0
```

Ten representative distinct errors, verbatim from the job log (the count is
how many times the compiler emitted it; swift repeats each diagnostic once per
emit-module pass and once per compile pass, so the raw totals are inflated
roughly 6× against the distinct ones):

```
    132 error: 'XMLParser' is unavailable: This type has moved to the FoundationXML module. Import that module to use it.
    126 error: cannot find 'RecorderMotion' in scope
    106 error: cannot find 'MediaImageWatermarkKind' in scope
     88 error: cannot find 'QuitProtectionSupport' in scope
     84 error: value of type 'XMLParser' (aka 'AnyObject') has no member 'abortParsing'
     84 error: cannot find 'TemperatureSensorSelector' in scope
     84 error: cannot find 'SwitcherNativeSymbolicHotKey' in scope
     84 error: cannot find 'RadialMenuSupport' in scope
     84 error: cannot find 'MixerRoutingSupport' in scope
     66 error: cannot find type 'HomebrewCaskRecord' in scope
```

Files with the most diagnostics:

```
   1115 /Vendored/GlobalShortcut.swift
   1003 /Vendored/Defaults.swift
    420 /Vendored/SettingsBackupSupport.swift
    259 /Vendored/AppUpdateFeedSupport.swift
    171 /Vendored/RecorderSupport.swift
```

**Reading this honestly: 3052 of the 4173 diagnostics are not Linux problems
at all.** They are `cannot find X in scope` for types declared in Vorssaint
files that the candidate set does not contain. The candidate list in
`WORK_PACKAGES.md` was assembled by feature, not by dependency, and the app
has no module boundaries today, so any subset of it is full of holes. What
remains after subtracting them is small, and it is the real answer:

| genuine Linux blocker | evidence |
|---|---|
| `XMLParser` moved to a separate module | `error: 'XMLParser' is unavailable: This type has moved to the FoundationXML module.` — one file, `AppUpdateFeedSupport.swift`. Fix: `#if canImport(FoundationXML) import FoundationXML #endif`. Trivial. |
| `ProcessInfo.ThermalState` does not exist | `error: 'ThermalState' is not a member type of class 'FoundationEssentials.ProcessInfo'` — `FanControlSupport.swift`. Thermal state is a macOS concept; on Linux it comes from hwmon, so this is a Platform-protocol item anyway (WP-12). |
| CoreFoundation C API is not exposed | `error: cannot find 'CFGetTypeID' in scope`, `error: cannot find 'CFBooleanGetTypeID' in scope` — `SettingsBackupSupport.swift`, used to tell a stored `Bool` from a stored number in a plist. Needs rewriting against `Codable`/`Any` type checks; WP-14 has to touch this file anyway. |
| Carbon text input, not just the key table | `cannot find 'UCKeyTranslate'`, `'kUCKeyActionDisplay'`, `'LMGetKbdType'`, `'OptionBits'`, `'OSStatus'`, `'noErr'`, `'paramErr'` — all in `GlobalShortcut.swift`. This is the "key code → printed character for the current layout" path. The Carbon *key table* shims away in 110 lines (`Sources/CarbonShim/Carbon.swift`); the *translation* does not, and needs xkbcommon. |
| CoreGraphics beyond geometry | 149 diagnostics, all from `CGEvent`: `value of type 'CGEvent' has no member 'flags' / 'keyboardGetUnicodeString' / 'getIntegerValueField'`. The geometry family (`CGFloat/CGPoint/CGSize/CGRect/CGVector/CGAffineTransform`) is in Linux Foundation and caused zero errors. `CGWindowID`, `CGDirectDisplayID`, `CGEventFlags`, `CGEventType` are a typealias and two option sets — shimmed in 60 lines with no further errors. |

And the categories that are **empty**: `Combine/OpenCombine 0`,
`AppKit leak 0`, `Swift 6 concurrency 0`. No AppKit symbol survived the thin
`Sources/AppKitShim` stub; nothing in the core hit strict-concurrency.

### `VorssaintCoreWide` (all 120 files) and `VorssaintServices` (208 files)

```
=== error census for build-wide.log ===
raw diagnostics:    122     distinct messages:  1
    122 error: no such module 'Darwin'
    122 /Vendored/HomebrewSupport.swift

=== error census for build-services.log ===
raw diagnostics:    210     distinct messages:  1
    210 error: no such module 'Vision'
    210 /Vendored/BarcodeDetector.swift
```

These two numbers are **not** comparable to the 4173 above and must not be
read as "the wide set is cleaner". `error: no such module` aborts the frontend
before type-checking, so the build stops at the first unshimmed import and
reports nothing else. `import Darwin` (4 files) and `import Vision` (1 file)
were deliberately left unshimmed: `Darwin` is the libc module, which is
`Glibc` on Linux, and `Vision` has no counterpart at all. Closing those two
holes is the obvious next iteration; the four-push budget ran out first, and
what they would reveal about the *other* 200 files is unmeasured.

### swift 6.2 nightly

Identical on the core set: the same 4173 raw / 126 distinct, the same ten top
errors, the same `no such module 'Vision'` for the service set. Nothing in
this spike is fixed or broken by moving from 6.1.3 to the 6.2 nightly.

## 4. The two files that actually compiled, and passed their tests

`spikes/wp00-swift-core/minimal/` is a package of its own holding
`URLCleaning.swift` and `CommandBarMath.swift` **unmodified**, plus nine tests
that check behaviour, not just compilation — including
`NumberFormatter`-formatted output and `Locale`-driven separators, the places
where swift-corelibs-foundation is most likely to differ from Apple's.

swift 6.1.3:

```
[4/6] Compiling VorssaintCoreMinimal URLCleaning.swift
[3/6] Compiling VorssaintCoreMinimal CommandBarMath.swift
Build complete! (2.76s)
swift build exit=0
...
Test Suite 'All tests' passed at 2026-09-11 22:33:52.868
	 Executed 9 tests, with 0 failures (0 unexpected) in 0.002 (0.002) seconds
swift test exit=0
```

swift 6.2 nightly: `Executed 9 tests, with 0 failures (0 unexpected) in 0.103
(0.103) seconds`, `swift test exit=0`.

`CommandBarMath.evaluate("0.1 + 0.2", locale: en_US)` returns `"0.3"` on
Linux, `evaluate("12 * (3 + 4)")` returns 84 formatted as `"84"`, and
`evaluate("20% of 250")` returns 50 — the same answers the macOS tests expect.
This is the single most load-bearing result in the spike: the logic does not
merely build, it behaves.

## 5. Static linking

`hello-spike` built with `--static-swift-stdlib` in release, then run, then
`ldd`-ed, in the same job:

```
--- binary ---
-rwxr-xr-x 1 root root 68097496 Sep 11 22:33 .../release/hello-spike
--- run ---
json={"locales":["en","de","fr","es","pt-BR","it","nl","ja","ko","zh-Hans","ru","pl","tr"],"name":"vorssaint-wp00","started":-978307200}
roundtrip=13 locales
url=https://example.com/a?keep=1
bytes=1.2 MB
regex=0
ok
--- ldd ---
	linux-vdso.so.1 (0x00007fe875c77000)
	libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x00007fe872517000)
	libstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x00007fe872299000)
	libgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x00007fe875c3e000)
	libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007fe872087000)
	/lib64/ld-linux-x86-64.so.2 (0x00007fe875c79000)
--- dynamic swift/Foundation/icu libraries still referenced ---
(none)
--- size ---
65M	.../release/hello-spike
```

**Result: the Swift runtime, the standard library and Foundation link
statically and completely.** Nothing named `swift*`, `Foundation*`, `icu*`,
`curl` or `xml` remains dynamic; what is left is libc, libm, libstdc++ and
libgcc — the set any C++ program on the system already needs. The program runs
and produces correct JSON, URL and `ByteCountFormatter` output.

The cost is size: **65 MB for 30 lines of Foundation.** That is the whole of
Foundation pulled in without dead-stripping. It matters for WP-04
(AppImage/Flatpak) and should be re-measured there with
`-Xlinker --gc-sections` and against the Static Linux SDK (musl), which was
not tried here.

There is one link-time warning, repeated five times, from Foundation itself,
not from our code:

```
libFoundationEssentials.a(Data+Writing.swift.o): ... warning: the use of `mktemp' is dangerous, better use `mkstemp' or `mkdtemp'
```

## 6. OpenCombine verdict

**OpenCombine is viable and barely exercised — which is the finding.**

`swift package resolve` pinned it cleanly:

```
"identity" : "opencombine",
"location" : "https://github.com/OpenCombine/OpenCombine.git",
"state" : { "revision" : "8576f0d579b27020beccbccc3ea6844f3ddfc2c2", "version" : "0.14.0" }
```

`OpenCombine`, `OpenCombineFoundation` and `OpenCombineDispatch` all built,
and the **Combine/OpenCombine error bin is 0** in every iteration of every
target. But the reason it is 0 is that exactly one of the 120 candidate files
(`Localization.swift`) imports Combine at all; the rest of the
`ObservableObject` surface lives in the service classes and the SwiftUI views,
which this spike does not cover. So: no evidence against OpenCombine, and no
real test of it either. WP-13 should treat that as open.

One concrete trap for WP-13, found the hard way (iteration 1):

```
error: circular dependency between modules 'VorssaintCombine' and 'Combine'
  13 | @_exported import Combine
```

`#if canImport(Combine)` inside a `VorssaintCombine` re-export target finds a
sibling shim target called `Combine` and Swift 6.1 rejects the cycle. In the
real port there is no such shim, so `canImport` is fine — but if anyone adds
one, key the switch on `canImport(Darwin)` instead, as this spike does.

## 7. Files that are clean as-is

93 of the 120 candidate files (42 858 + 849 = 43 707 lines) carry no
Linux-blocking symbol. They are the 89 in the "clean" row plus the four that
touch only the `CGFloat/CGPoint/CGSize/CGRect` family
(`MouseSpacesGestureSupport`, `MouseAppExceptionSupport`, `RecorderTimeline`,
`RecordingSharingSupport`). Three caveats, in descending importance:

1. **Clean is not compilable.** None of them resolves in isolation, because
   they reference types declared in files outside the set. The list is a
   starting point for WP-11, not a shippable module.
2. The ones proven to compile *and* pass tests on Linux are the two in
   `VorssaintCoreMinimal`: `URLCleaning.swift` and `CommandBarMath.swift`.
3. **The census has a known blind spot and the compiler found it.** It scans
   for `NS*`, `CG*` and framework imports, so it misses Apple symbols spelled
   with neither: `AppUpdateFeedSupport.swift` and `SettingsBackupSupport.swift`
   are both in the "clean" list, and both failed on Linux —
   `XMLParser`/`FoundationXML` in the first, `CFGetTypeID`/`CFBooleanGetTypeID`
   in the second. Two files out of 93 is a ~2 % false-clean rate; treat the
   clean list as a strong prior, not a guarantee, and let `swift build` be the
   check.

The full per-file classification is the table in § 1.

## 8. Verdict

**GO WITH CONDITIONS** on keeping the core in Swift. Rewriting it in Rust
(`PLAN.md` § 9) is not warranted by anything measured here.

What the evidence supports:

- Linux Swift 6.1.3 compiles unmodified Vorssaint sources, links Foundation
  and the stdlib statically into a self-contained binary, and produces the
  same answers for locale-sensitive logic. Nine behavioural tests pass on
  6.1.3 and on the 6.2 nightly.
- The Linux-specific blockers in 64 000 lines of candidate code are five
  named items (§ 3), four of which are a few lines each and the fifth of
  which (`UCKeyTranslate`) is a Platform-protocol item the plan already
  assumes.
- Zero Combine errors, zero AppKit leaks that the shim could not absorb, zero
  Swift 6 strict-concurrency errors.

The conditions:

1. **WP-11 must be sequenced by dependency, not by feature.** The candidate
   list in `WORK_PACKAGES.md` produced 3052 "cannot find X in scope"
   diagnostics because it is a feature list. Build the move order from the
   actual declaration graph and move closed sets; otherwise every intermediate
   commit is red.
2. **`Defaults.swift` and `GlobalShortcut.swift` are the hub and must be split
   first.** They produced 1115 and 1003 diagnostics — half the census — and
   every other file depends on them. `Defaults.swift` needs its `Bundle.main`
   domain lookup and its Carbon key table separated; `GlobalShortcut.swift`
   needs the key *table* (portable, 110 lines) split from the key
   *translation* (`UCKeyTranslate`, not portable, becomes xkbcommon).
3. **Budget for the four named Foundation gaps**: `FoundationXML` for
   `XMLParser`, no `ProcessInfo.ThermalState`, no `CFGetTypeID`/
   `CFBooleanGetTypeID`, no `FileManager.trashItem` (unused today, but the
   Cleaner and Uninstaller features will want it — freedesktop trash spec or
   the `org.freedesktop.FileManager1.TrashFiles` portal).
4. **Re-run this spike on Fedora before the Phase 0 gate closes**, or accept
   in writing that Ubuntu 24.04 is the only proven distribution. It could not
   be done from the port containers.
5. **Close the `Darwin` and `Vision` shim holes and re-measure the 208-file
   service set.** Its current census (210 diagnostics, one message) is an
   artefact of the frontend aborting at the first missing module, not a
   statement about that code. Until that is done, nobody knows the error count
   for the bulk of the service layer.
6. **Treat the OpenCombine verdict as provisional.** It resolves and builds,
   but one file in the candidate set imports Combine. WP-13 must re-measure
   against the real `ObservableObject` services.
7. **Re-measure binary size in WP-04.** 65 MB statically linked for a trivial
   program is the number the AppImage and Flatpak work has to start from.

## 9. Reproducing this

```
./spikes/wp00-swift-core/census.py                 # § 1, needs only python3
cd spikes/wp00-swift-core && ./sync.sh             # vendor the sources
swift build --target VorssaintCoreSpike            # § 3   (needs Linux Swift)
swift build --target VorssaintServices             # § 3
swift build -c release --static-swift-stdlib --product hello-spike && ldd ...   # § 5
(cd minimal && swift test)                         # § 4
```

The raw `swift build` logs from run 4 are attached to the run as artifacts
`wp00-swift-6.1-logs` and `wp00-swift-6.2-logs`. They are *not* printed into
the job log: the failing targets emit tens of thousands of diagnostic lines
and the Actions API only ever returns the tail, so printing them in full made
the run unreadable through the API. That is why the job log carries the
classified census from `classify-errors.sh` instead.
