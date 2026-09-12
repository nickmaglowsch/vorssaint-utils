# WP-16: the test harness on both platforms

Deliverable of WP-16 (`WORK_PACKAGES.md`). What runs where, how the Linux
suite is produced, and the evidence for every number below.

**Result.** `Package.swift` gains a `VorssaintCoreTests` XCTest target.
`Tools/linux-port/port-tests.py` selects, out of the 5 368 `expect…(…)` call
sites in `Tests/`, the **1 309** whose statements only touch declarations
under `Sources/VorssaintCore` (plus the standard library and the Foundation
subset swift-corelibs-foundation provides) and emits them as **73** generated
`XCTestCase` files. The other 4 059 call sites stay on macOS only, because
they name AppKit/IOKit/CoreGraphics API or a symbol that is still in
`Sources/Vorssaint`. `build.sh --test` is untouched and still reports
`TESTS OK (31565 checks)`.

_(placeholder: CI run URLs and quoted output are filled in below once the
gate has run — see § 5.)_

## 1. What the macOS harness is, and why it could not simply be reused

`build.sh --test` compiles ~200 named `Sources/**` files together with the
eight `Tests/*.swift` files into one `swiftc` invocation and runs the
resulting binary. `Tests/MetricsTests.swift` is 27 024 lines, of which 26 680
are one `static func main()`: a flat sequence of local bindings and
`expect(…)` / `expectEqual(…)` / `expectClose(…)` / `expectFormat(…)` calls,
cut into 125 `// MARK:` sections. There is no XCTest anywhere — the file says
why: "the Command Line Tools cannot run `swift test`".

That harness cannot run on Linux: the same compile also pulls in
`Sources/Vorssaint/**` AppKit and IOKit code. But most *checks* are pure. So
the port is a selection problem, not a rewrite: find the statements that
survive on their own, keep their order and their messages, and leave the rest
exactly where they are.

## 2. The generator

```
python3 Tools/linux-port/port-tests.py            # regenerate the suite
python3 Tools/linux-port/port-tests.py --report   # per-section counts (§ 4)
python3 Tools/linux-port/port-tests.py --why      # what blocks the rest
python3 Tools/linux-port/port-tests.py --verify   # re-check the emitted files
```

Output: `Tests/VorssaintCoreTests/Generated*.swift`, committed. The CI step
"The generated tests are what the generator produces" runs the tool and
`git diff --exit-code`, so a hand edit to a generated file fails the gate.

How it selects:

1. **Lexing.** A Swift-aware scrubber blanks comments and string literals
   while keeping every offset, and — unlike `declgraph.strip_noise` — it
   understands `\(…)` interpolation. That matters: in
   `"… (\(marks.prefix(6).joined(separator: ", ")))"` the older scrubber ends
   the literal at the quote *inside* the interpolation and eats the real
   closing parenthesis, which merged the last 13 000 lines of MetricsTests
   into a single "statement". It also reports which line endings fall inside
   a `"""` literal, so a JSON fixture whose lines start at the statement's own
   indentation is not mistaken for a run of new statements.
2. **Splitting.** The body is cut into top-level statements by bracket depth.
   A line that is neither a statement start nor part of the statement above
   it aborts the run (`port-tests: orphan line …`) rather than being dropped:
   silently losing a line would generate code that is missing a piece.
3. **Selection.** Every name a statement mentions must resolve to one of:
   a local introduced by an earlier *kept* statement in the same section; a
   declaration under `Sources/VorssaintCore` (the index comes from
   `Tools/linux-port/declgraph.py`, the WP-11 tool); or the allow-listed
   stdlib/Foundation surface in the tool. Anything else — a declaration that
   stayed in `Sources/Vorssaint`, `CGPoint`, `pid_t`, a helper defined in
   another `Tests/` file — drops the statement.
4. **Poisoning.** When a statement is dropped, every local name it touched is
   dropped from scope too, so a later check cannot silently read a variable
   whose value came from a statement that is no longer there.
5. **Sections.** Each `// MARK:` becomes one `XCTestCase` with one test
   method; locals do not cross that boundary. Statements that set shared
   static state (`MetricFormat.locale = Locale(identifier: "en_US_POSIX")`,
   pinned once at the top of the file and read thousands of lines further
   down) are replayed at the head of every later section that needs them,
   because XCTest gives no order between classes.
6. **Emission.** The original statement text, comments and assertion messages
   are copied verbatim; `expect` maps to `XCTAssertTrue(condition, message)`,
   `expectEqual`/`expectFormat` to `XCTAssertEqual`, `expectClose` to
   `XCTAssertEqual(…, accuracy:)`. `formatSpecifiers` and `placeholderShape`
   are copied once into `GeneratedSupport.swift`.

The `#if canImport(Darwin) import Combine #else import OpenCombine #endif`
guard (`COMBINE.md` § 1) is emitted for any case that mentions a Combine type,
and the test target carries the conditional `OpenCombine` dependency for it.
No selected check currently needs it — the Combine users in the core are
services, and every statement that constructs one also names a Mac-side type:

```
$ grep -l OpenCombine Tests/VorssaintCoreTests/*.swift ; echo "exit $?"
exit 1
```

### 2.1 The generator is not a compiler

No Swift toolchain exists in the port agents' container (`swift.org` is
blocked by the egress proxy), so the tool ships its own whole-file check and
CI runs it next to the build:

```
$ python3 Tools/linux-port/port-tests.py --verify
74 file(s) checked, 0 problem(s)
```

It re-reads each emitted file, requires balanced brackets, and requires every
name in it to resolve to a local declared in the same method, a
`Sources/VorssaintCore` declaration, or the allow-list. The compiler on the
Linux CI leg is the real gate; this is what makes the first attempt at it
plausible rather than a guess.

## 3. What runs where

| leg | command | what it runs |
|---|---|---|
| macOS (`macos` job) | `./build.sh --test` | unchanged: all eight `Tests/*.swift` compiled with `swiftc`, `TESTS OK (31565 checks)` |
| Linux (`linux-core` job) | `swift test --filter VorssaintCoreTests` | the 73 generated cases |

`build.sh --test` does not read `Tests/VorssaintCoreTests/` — it names its
inputs one by one — and `Package.swift`'s test target reads nothing else, so
the two suites cannot interfere. On Linux the manifest omits the `VorssaintMac`
and `Vorssaint` targets (`#if os(macOS)` in `Package.swift`): `swift test`
builds *every* target in the package, and those two are AppKit/IOKit code.
On macOS the manifest is byte-for-byte equivalent to what it declared before.

## 4. Counts per section

`python3 Tools/linux-port/port-tests.py --report`; "linux" is the number of
`expect…` call sites emitted into `Tests/VorssaintCoreTests`, "macOS" the
number left behind, and the last column names the kind of blocker
(`mac` = a declaration still in `Sources/Vorssaint`, `platform` = Apple API,
`unknown-name` = a name nothing in scope declares, e.g. a helper from another
`Tests/` file or a value produced by an already-dropped statement).

Call sites, not executed checks: the loops in these sections multiply them
(the macOS binary counts 31 565 executed checks over 5 368 call sites).

```
$ python3 Tools/linux-port/port-tests.py --report
section                                                linux   macOS  why dropped
Prelude                                                    0      15  mac,platform,unknown-name
Prelude                                                    0       4  unknown-name
Byte / rate formatting                                    25       0  
Disk helpers                                              38       0  
Clipboard history search                                   0       4  mac
Clipboard auto clear preferences                           0      10  mac
Clipboard auto clear timing                               33       2  mac
Settings search navigation                                13      93  mac,platform,unknown-name
Peripheral battery helpers                                18       1  platform
Keyboard debounce                                          0      29  mac,unknown-name
Mouse click debounce                                      55      28  mac,platform,unknown-name
Smooth scrolling                                          54      21  mac,platform,unknown-name
Watts & percent                                           30       0  
Temperature sensor selection                               0      44  mac,unknown-name
Hot CPU alert                                             12       0  
Uptime formatting                                          6       0  
Memory used                                                2       7  mac,unknown-name
App memory                                                13       0  
Registered defaults                                       26     116  mac,platform,unknown-name
Hidden app windows (issue #656)                            0       2  mac,platform
Stale surfaces without an Accessibility witness (iss       0      58  mac,platform,unknown-name
Which display the switcher opens on                        0       7  mac,platform
The switcher list held to one display (issue #1391)        1      12  mac,platform,unknown-name
Switcher entries for apps with no window (issue #351       0      17  mac,unknown-name
The visible cap spends its slots across apps (issue        9     209  mac,platform,unknown-name
The panel surface reaches the popover arrow (issue #       2     146  mac,platform,unknown-name
Editing, navigation and upper function keys as short       0      12  mac,unknown-name
Shortcuts the app silences while a field is listenin      10      25  mac,unknown-name
WhatsApp downloads                                        73     138  mac,platform,unknown-name
Window layout shortcut resolution (issue #169)             0       6  mac
Window layout restore history (issue #414)                 0       6  mac,platform,unknown-name
Window layout geometry                                     0     111  mac,platform
Window move and resize gestures                            0      21  mac,platform,unknown-name
Click versus drag custody (issue #321)                     0      33  mac,platform,unknown-name
Directional pointer layout                                 0      43  mac,platform,unknown-name
Media size targets                                         8     199  mac,platform,unknown-name
Boost limiter (issue #326)                                 0      21  mac,platform,unknown-name
Mixer render (issue #397)                                  0       5  mac,platform,unknown-name
Unwritten output frames (issue #326)                       0      23  mac,platform,unknown-name
Shelf persistence                                          0      21  mac
Shelf reveal                                               0      36  mac,platform
Shelf dock drag support                                    5      49  mac,platform,unknown-name
Shelf tile tooltip                                         1      15  mac,unknown-name
Middle click tap (issue #161)                             11       2  mac
Cut and paste move progress (issue #168)                   6      15  platform,unknown-name
Paste copied image as file (issue #429)                    0       5  mac
Update installer helpers                                   2      62  mac,platform,unknown-name
Settings search structural deduplication                   0       9  mac,unknown-name
Settings search routing                                    0      12  mac,unknown-name
Grouped Settings search suggestions                        0      77  mac,platform,unknown-name
- UpdateServiceSupport & SemVer channel reconciliati       4      22  mac,unknown-name
Launch at login reconciliation                             8       0  
Dock Preview helpers                                      35     430  mac,platform,unknown-name
Release notes parsing                                      0      18  mac,unknown-name
URL cleaning                                              38       1  mac
Homebrew command building and parsing                      4      74  mac,unknown-name
Localization format contracts                             24     103  mac,platform,unknown-name
Network speed math                                        28      34  mac,platform,unknown-name
Interface filtering                                        8       0  
History ring buffer                                        5       0  
Cleaning-mode unlock gesture                              26       0  
Music launch blocker                                      14       0  
Features hub catalog                                      18      14  mac
Hardware-gated installs                                   25       7  mac,platform,unknown-name
Fan Control safety policy                                 12     109  mac,unknown-name
Super key held-key watchdog                                0       4  mac
Features hub strings                                      36       0  
Kill Process safety                                        9      44  mac,unknown-name
Hub presets and energy badges                              0      14  mac
Settings page visibility                                   0      36  mac,unknown-name
Display brightness (DDC/CI helpers)                       91       2  mac,platform
Text snippets engine (issue #201)                         89       0  
Snippet library (issue #340)                              13       2  mac,unknown-name
Radial menu (issue #220)                                   1      36  mac,platform,unknown-name
Radial menu profiles                                       2      28  mac,platform,unknown-name
Dock click with AX-blind apps (issue #200)                 0       8  mac,platform
Dock click restore order (issue #357)                      0      10  mac
Quick toggles                                             16       4  mac,platform
Screenshot tool                                           27     180  mac,platform,unknown-name
Assistive keyboard click recognition                       0       1  unknown-name
Remappable screenshot tool shortcuts                      41     127  mac,platform,unknown-name
Mouse button shortcuts (issue #282)                        0      21  mac,unknown-name
Spaces and Mission Control drag (issue #1012)             16      24  mac,platform,unknown-name
Super key (issue #330)                                     4      55  mac,unknown-name
Mouse app exceptions (issue #358)                         21      49  mac,platform,unknown-name
Settings backup                                            0      67  mac,platform,unknown-name
Precise volume roller                                     11       0  
App updates                                                0      99  mac,unknown-name
Brightness key base (issue #370)                           5       0  
Command bar calculator                                    41       0  
Command bar, what the person controls                      0       7  mac,unknown-name
Compact mode, what an empty field shows                    0      13  mac
What the bar noticed about this session                    0      12  mac,unknown-name
Finding a file from the bar                               15       5  mac,unknown-name
The Mac's own Settings panes                              10      30  mac,platform,unknown-name
Command bar unit conversion                                0      25  mac,unknown-name
Command bar emoji                                         14       3  mac
Command bar highlighting                                   0       6  mac
Command bar wiring                                         0      10  mac,unknown-name
Screen recorder wiring                                     2      24  mac,platform,unknown-name
Screen recorder geometry and policy                        4      82  mac,platform,unknown-name
Screen recorder motion                                    13       0  
Screen recorder zoom                                      20      15  mac,platform,unknown-name
Screen recorder timeline                                   2      22  mac,platform,unknown-name
Screen recorder text                                       0       9  mac,unknown-name
Screen recorder pictures                                   0      32  mac,platform,unknown-name
Screen recorder blur                                       0      16  mac,platform,unknown-name
Screen recorder pointer track                              0       9  mac,platform,unknown-name
Screen recorder canvas                                    17      31  mac,platform,unknown-name
Command bar search and ranking                             6      88  mac,unknown-name
The other names macOS knows an app by                      4      27  mac,unknown-name
Open what was typed as a URL                               0      59  mac,platform,unknown-name
Capture tool shortcuts                                     0       5  mac
A failed removal explains itself where it failed           3       7  mac
Private file store                                         0       6  mac,platform,unknown-name
Result                                                     5       0  
Every temp dir build.sh stages in is swept when the        0       6  unknown-name
An identity-less build that installs creates its sta       2       2  unknown-name
The stable identity is judged by whether codesign ca       0       2  unknown-name
Modifying mouse taps are handed back across a sessio      22       0  
Uninstallation paths stay aligned across SelfUninsta       8       0  
Detached command reruns (counted last, so a late rer       0       1  unknown-name
Command-Q / Command-W protection                          29      23  mac,platform,unknown-name
A sleeping clock                                           6       0  
A dropped identifier                                       2       0  unknown-name
Prelude                                                    0      19  mac
Prelude                                                    0      25  platform
Prelude                                                    0      12  mac,platform,unknown-name
Prelude                                                    0       1  unknown-name
Prelude                                                    0      22  mac,platform
Prelude                                                    0       9  mac
TOTAL                                                   1309    4059
```

## 5. The two vacuous walks (CORE_MOVES.md § 5)

WP-11 named two source-scanning checks in `Tests/MetricsTests.swift` that
still pass but no longer read anything, because the string catalogs they walk
moved under `Sources/VorssaintCore/Core`. This is the one edit WP-16 makes to
that file: both walks now cover the old and the new roots, so they scan the
files again. No `expect` call is added or removed — the macOS count stays at
31 565.

The apostrophe sweep (line 13547) walked
`Sources/Vorssaint/Core` + `…/Core/Localizations`:

```
$ ls Sources/Vorssaint/Core | grep -cE '(Strings\.swift$|^Strings\+|^Localization\.swift$)'
2
$ ls Sources/Vorssaint/Core/Localizations
ls: cannot access 'Sources/Vorssaint/Core/Localizations': No such file or directory
$ ls Sources/VorssaintCore/Core | grep -cE '(Strings\.swift$|^Strings\+|^Localization\.swift$)'
39
$ ls Sources/VorssaintCore/Core/Localizations | grep -cE '(Strings\.swift$|^Strings\+|^Localization\.swift$)'
11
```

so **2 files before, 52 after** (39 + 11 + the two that stayed:
`MouseExceptionStrings.swift` and `SuperKeyStrings.swift`). The French
non-breaking-space sweep (line 13582) listed
`Sources/Vorssaint/Core/Localizations/Strings+French.swift` (now absent) plus
`Sources/Vorssaint/Core/*Strings.swift`:

```
$ ls Sources/Vorssaint/Core/Localizations/Strings+French.swift
ls: cannot access '…': No such file or directory
$ find Sources/Vorssaint/Core -maxdepth 1 -name '*Strings.swift' | wc -l
2
$ ls Sources/VorssaintCore/Core/Localizations/Strings+French.swift
Sources/VorssaintCore/Core/Localizations/Strings+French.swift
$ find Sources/VorssaintCore/Core -maxdepth 1 -name '*Strings.swift' | wc -l
38
```

so **2 files with content before, 40 after** (38 + the French catalog + the
two that stayed; two of the 42 listed paths yield no lines — the missing
`Sources/Vorssaint/Core/Localizations/Strings+French.swift` and the one
`*Strings.swift` with no `static let fr =` block).

Both widened walks were replayed in Python before the edit landed
(`/tmp/wp16/walks.py`, the same rules line for line) to be sure they are not
about to turn red on macOS:

```
$ python3 /tmp/wp16/walks.py          # old paths + new paths
walk1 files: 52 violations: 0 []
french sources listed: 42 with content: 40 violations: 0 []
$ python3 /tmp/wp16/walks_old.py      # the paths as they were
walk1 files: 2 violations: 0 []
french sources listed: 3 with content: 2 violations: 0 []
```

## 6. CI

<!-- CI -->

## 7. WP-17 and `Tools/ui-smoke.sh`

WP-17 ("macOS CI job proves `main` unchanged") is satisfied by the `macos`
job WP-10 added to `.github/workflows/linux-port-ci.yml`: it builds with
`./build.sh`, runs `./build/Vorssaint --selftest` and runs `./build.sh --test`
on every push to the port branch, on the Xcode the compatibility job pins.
The WP-16 change keeps that job exactly as it was.

`Tools/ui-smoke.sh` is **not** added to it, and should not be. Read the
script: it drives the *installed* Developer build
(`/Applications/Vorssaint (Developer).app`, exits 1 if absent) through
`System Events` Accessibility scripting, takes `screencapture` screenshots,
and its own header states it "Requires Accessibility + Screen Recording
permission for the terminal running it". Both are TCC permissions that a
GitHub-hosted macOS runner cannot grant non-interactively, and the popover
assertions it makes need a real logged-in GUI session. On a runner it would
fail at the first `ax` call with an empty result, i.e. it would be a false
red, not a gate. It stays a local pre-release script; the automated proof
that `main` is unchanged is `--selftest` plus the 31 565 checks.
