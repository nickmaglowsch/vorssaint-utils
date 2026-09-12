# WP-16: the test harness on both platforms

Deliverable of WP-16 (`WORK_PACKAGES.md`). What runs where, how the Linux
suite is produced, and the evidence for every number below.

**Result.** `Package.swift` gains a `VorssaintCoreTests` XCTest target.
`Tools/linux-port/port-tests.py` selects, out of the 5 368 `expect…(…)` call
sites in `Tests/`, the **1 340** whose statements only touch declarations
under `Sources/VorssaintCore` (plus the standard library and the Foundation
subset swift-corelibs-foundation provides) and emits them as **73** generated
`XCTestCase` files. The other 4 028 call sites stay on macOS only, because
they name AppKit/IOKit/CoreGraphics API or a symbol that is still in
`Sources/Vorssaint`. `build.sh --test` is untouched and still reports
`TESTS OK (31565 checks)`.

On Linux the suite is **73 XCTest cases / ~2 600 executed checks** and it is
green on the port branch — `Executed 90 tests, with 0 failures` counting the
17 `PlatformProtocolTests` WP-12 put in the same target (§ 6.2). On macOS
`build.sh --test` still ends in `TESTS OK (31565 checks)` (§ 6.1). Three
checks were found not to port; each is accounted for in § 6.3.

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
"The generated tests are what the generator produces" re-runs the tool:
`--verify` is a hard gate, and drift between the committed files and a fresh
run is reported as a warning with the regeneration command. Drift is not a
failure on purpose — every later core move enlarges the selection, and that
would otherwise turn the shared branch red for whoever pushes next. **Run the
generator and commit its output whenever you move a file into
`Sources/VorssaintCore`**; the counts below are a snapshot of the core at this
commit (112 files).

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

   The soundness of step 3 is not total, and the limit is worth stating:
   a name reached through a dot is assumed to belong to a receiver that was
   already checked, so an *instance member* on a core type is not verified
   against the declaration index. Members added to a core type by an
   extension in a non-core file (`ScratchpadSupport.markdownPreview` in
   `ScratchpadSupport+Mac.swift`, say) are caught, because the tool indexes
   those separately and matches them as `Type.member` — bare-name matching
   cost 126 unrelated statements and was abandoned. What is not caught is a
   member a core file itself declares behind `#if os(macOS)`. The Linux
   compiler on CI is the backstop for that case, and it is why the generated
   suite is a CI gate rather than a local claim.
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
(the macOS binary counts 31 565 executed checks over 5 368 call sites).

```
$ python3 Tools/linux-port/port-tests.py --report
section                                                linux   macOS  why dropped
Prelude                                                    0      15  incomplete,mac,platform,unknown-name
Prelude                                                    0       4  mac,unknown-name
Byte / rate formatting                                    25       0  
Disk helpers                                              38       0  
Clipboard history search                                   0       4  mac
Clipboard auto clear preferences                           0      10  mac
Clipboard auto clear timing                               33       2  mac
Settings search navigation                                12      94  incomplete,mac,platform,unknown-name
Peripheral battery helpers                                18       1  platform
Keyboard debounce                                          0      29  mac,unknown-name
Mouse click debounce                                      48      35  incomplete,mac,platform,unknown-name
Smooth scrolling                                          50      25  incomplete,mac,platform,unknown-name
Watts & percent                                           30       0  
Temperature sensor selection                               0      44  incomplete,mac,unknown-name
Hot CPU alert                                             12       0  
Uptime formatting                                          6       0  
Memory used                                                2       7  mac,unknown-name
App memory                                                13       0  
Registered defaults                                       20     122  incomplete,mac,platform,unknown-name
Hidden app windows (issue #656)                            0       2  mac,platform
Stale surfaces without an Accessibility witness (iss       0      58  mac,platform,unknown-name
Which display the switcher opens on                        0       7  mac,platform
The switcher list held to one display (issue #1391)        0      13  incomplete,mac,platform,unknown-name
Switcher entries for apps with no window (issue #351       0      17  mac,unknown-name
The visible cap spends its slots across apps (issue        1     217  incomplete,mac,platform,unknown-name
The panel surface reaches the popover arrow (issue #       1     147  incomplete,mac,platform,unknown-name
Editing, navigation and upper function keys as short       0      12  mac,unknown-name
Shortcuts the app silences while a field is listenin      10      25  mac,unknown-name
WhatsApp downloads                                        72     139  incomplete,mac,platform,unknown-name
Window layout shortcut resolution (issue #169)             0       6  mac
Window layout restore history (issue #414)                 0       6  mac,platform,unknown-name
Window layout geometry                                     0     111  mac,platform
Window move and resize gestures                            0      21  mac,platform,unknown-name
Click versus drag custody (issue #321)                     0      33  mac,platform,unknown-name
Directional pointer layout                                 0      43  incomplete,mac,platform,unknown-name
Media size targets                                         4     203  incomplete,mac,platform,unknown-name
Boost limiter (issue #326)                                 0      21  mac,platform,unknown-name
Mixer render (issue #397)                                  0       5  mac,platform,unknown-name
Unwritten output frames (issue #326)                       0      23  mac,platform,unknown-name
Shelf persistence                                          0      21  mac
Shelf reveal                                               0      36  mac,platform
Shelf dock drag support                                    2      52  incomplete,mac,platform,unknown-name
Shelf tile tooltip                                         1      15  mac,unknown-name
Middle click tap (issue #161)                             11       2  mac
Cut and paste move progress (issue #168)                   6      15  platform,unknown-name
Paste copied image as file (issue #429)                    0       5  mac
Update installer helpers                                   0      64  incomplete,mac,platform,unknown-name
Settings search structural deduplication                   0       9  incomplete,mac,unknown-name
Settings search routing                                    0      12  incomplete,mac,unknown-name
Grouped Settings search suggestions                        0      77  incomplete,mac,platform,unknown-name
- UpdateServiceSupport & SemVer channel reconciliati       4      22  incomplete,mac,unknown-name
Launch at login reconciliation                             6       2  incomplete,unknown-name
Dock Preview helpers                                      21     444  incomplete,mac,platform,unknown-name
Release notes parsing                                      0      18  incomplete,mac,unknown-name
URL cleaning                                              35       4  incomplete,mac,unknown-name
Homebrew command building and parsing                      0      78  incomplete,mac,unknown-name
Localization format contracts                             14     113  excluded,incomplete,mac,platform,unknown-name
Network speed math                                        28      34  incomplete,mac,platform,unknown-name
Interface filtering                                        8       0  
History ring buffer                                        5       0  
Cleaning-mode unlock gesture                              22       4  incomplete,unknown-name
Music launch blocker                                      14       0  
Features hub catalog                                      12      20  incomplete,mac,unknown-name
Hardware-gated installs                                   20      12  excluded,incomplete,mac,platform,unknown-name
Fan Control safety policy                                 12     109  mac,unknown-name
Super key held-key watchdog                                0       4  mac
Features hub strings                                      36       0  
Kill Process safety                                        9      44  mac-member,unknown-name
Hub presets and energy badges                              0      14  mac
Settings page visibility                                   0      36  incomplete,mac,unknown-name
Display brightness (DDC/CI helpers)                       83      10  incomplete,mac,platform,unknown-name
Text snippets engine (issue #201)                         55      34  incomplete,unknown-name
Snippet library (issue #340)                              13       2  mac,unknown-name
Radial menu (issue #220)                                   0      37  incomplete,mac,platform,unknown-name
Radial menu profiles                                       0      30  incomplete,mac,platform,unknown-name
Dock click with AX-blind apps (issue #200)                 0       8  mac,platform
Dock click restore order (issue #357)                      0      10  mac
Quick toggles                                             16       4  mac,platform
Screenshot tool                                           10     197  incomplete,mac,platform,unknown-name
Assistive keyboard click recognition                       0       1  unknown-name
Remappable screenshot tool shortcuts                      29     139  incomplete,mac,mac-member,platform,unknown-name
Mouse button shortcuts (issue #282)                        0      21  mac,unknown-name
Spaces and Mission Control drag (issue #1012)              5      35  incomplete,mac,platform,unknown-name
Super key (issue #330)                                     0      59  incomplete,mac,unknown-name
Mouse app exceptions (issue #358)                         17      53  mac,mac-member,platform,unknown-name
Settings backup                                            0      67  incomplete,mac,platform,unknown-name
Precise volume roller                                     11       0  
App updates                                                0      99  incomplete,mac,unknown-name
Brightness key base (issue #370)                           5       0  
Command bar calculator                                    41       0  
Command bar, what the person controls                      6       1  incomplete,unknown-name
Compact mode, what an empty field shows                   10       3  mac
What the bar noticed about this session                   12       0  
Finding a file from the bar                               16       4  mac,unknown-name
The Mac's own Settings panes                              37       3  platform
Command bar unit conversion                               25       0  
Command bar emoji                                         17       0  
Command bar highlighting                                   6       0  
Command bar wiring                                         0      10  mac,unknown-name
Screen recorder wiring                                     5      21  incomplete,mac,platform,unknown-name
Screen recorder geometry and policy                       44      42  incomplete,mac,platform,unknown-name
Screen recorder motion                                    13       0  
Screen recorder zoom                                      24      11  platform,unknown-name
Screen recorder timeline                                  18       6  platform,unknown-name
Screen recorder text                                       8       1  mac
Screen recorder pictures                                  11      21  mac,platform,unknown-name
Screen recorder blur                                       4      12  mac,platform,unknown-name
Screen recorder pointer track                              0       9  mac,platform,unknown-name
Screen recorder canvas                                    17      31  incomplete,mac,mac-member,platform,unknown-name
Command bar search and ranking                            47      47  incomplete,mac,unknown-name
The other names macOS knows an app by                      4      27  incomplete,mac,unknown-name
Open what was typed as a URL                              26      33  incomplete,mac,platform,unknown-name
Capture tool shortcuts                                     0       5  mac
A failed removal explains itself where it failed           2       8  incomplete,mac,unknown-name
Private file store                                         0       6  mac,platform,unknown-name
Result                                                     0       5  incomplete,unknown-name
Every temp dir build.sh stages in is swept when the        0       6  incomplete,unknown-name
An identity-less build that installs creates its sta       0       4  incomplete,unknown-name
The stable identity is judged by whether codesign ca       0       2  unknown-name
Modifying mouse taps are handed back across a sessio       8      14  incomplete,unknown-name
Uninstallation paths stay aligned across SelfUninsta       0       8  incomplete,unknown-name
Detached command reruns (counted last, so a late rer       0       1  unknown-name
Command-Q / Command-W protection                          31      21  incomplete,mac,platform,unknown-name
A sleeping clock                                           1       5  incomplete,unknown-name
A dropped identifier                                       2       0  unknown-name
Prelude                                                    0      19  mac
Prelude                                                    0      25  platform
Prelude                                                    0      12  incomplete,mac,platform,unknown-name
Prelude                                                    0       1  unknown-name
Prelude                                                    0      22  mac,platform
Prelude                                                    0       9  platform
TOTAL                                                   1340    4028
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

Both jobs of `.github/workflows/linux-port-ci.yml` are hard gates.

### 6.1 macOS: the product is unchanged

[Run 34665096667, job 103475277758](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34665096667/job/103475277758)
— `./build.sh`, `./build/Vorssaint --selftest` and `./build.sh --test`, all
success, on the commit that carries the widened walks:

```
2026-09-12T01:43:02Z ▸ Building & running unit tests against MacOSX.sdk…
2026-09-12T01:47:14Z TESTS OK (31565 checks)
2026-09-12T01:47:14Z PREFERENCE CLEANUP TESTS OK
```

The count is the number it was before WP-16: the harness, its inputs and its
assertions are untouched, and the two widened walks read 52 and 40 files
instead of 2 and 2 without finding a violation.

### 6.2 Linux: `swift test`

The Linux half was proven on `claude/wp16-verify`, a branch carrying exactly
these changes on top of `ceefb61` — the last commit whose `linux-core` job was
green — because the shared port branch was red for hours on an unrelated
`MeasurementFormatter` error that WP-12's core moves introduced in
`CommandBarUnits.swift` (run 34665056778, on WP-12's own commit, before
WP-16's first push). Four runs, each fixing what the previous one found:

| run | verdict | what it found |
|---|---|---|
| [34692708425](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34692708425) | red, compile | `expected '{' to start the body of for-each loop` (a `where` clause on its own line) and `cannot find 'regularBareApp' in scope` (`if let x` shorthand read as a declaration) |
| [34693146465](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34693146465) | red, 3 of 61 | `type 'ScratchpadSupport' has no member 'markdownPreview'`, then the suite compiled, ran, and failed three assertions |
| [34693339999](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34693339999) | red, 2 of 61 | `Executed 61 tests, with 2 failures (0 unexpected) in 8.105 (8.105) seconds` — the two exclusions were keyed by line number, and WP-12 had shifted the file by three lines |
| [34693499195, job 103552863887](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34693499195/job/103552863887) | **green** | every step success: three `swift build`s, `swift run VorssaintLinux`, `swift test --filter VorssaintCoreTests`, and the generator re-run |

And then on the port branch itself, once WP-12's `MeasurementFormatting` seam
had fixed the core build:
[run 34693694037, job 103553381117](https://github.com/nickmaglowsch/vorssaint-utils/actions/runs/34693694037/job/103553381117),
**every step success**, ending:

```
2026-09-12T12:28:56Z === totals =======================================
2026-09-12T12:28:56Z 	 Executed 1 test, with 0 failures (0 unexpected) in 0.001 (0.001) seconds
2026-09-12T12:28:56Z 	 Executed 1 test, with 0 failures (0 unexpected) in 0.001 (0.001) seconds
2026-09-12T12:28:56Z 	 Executed 1 test, with 0 failures (0 unexpected) in 0.009 (0.009) seconds
2026-09-12T12:28:56Z 	 Executed 17 tests, with 0 failures (0 unexpected) in 0.104 (0.104) seconds
2026-09-12T12:28:56Z 	 Executed 90 tests, with 0 failures (0 unexpected) in 8.136 (8.136) seconds
```

90 = the 73 generated cases plus the 17 `PlatformProtocolTests` WP-12 added to
the same target. The step order in the workflow puts the generator check
*before* the tests so that these five lines are the last thing in the job log:
the API serves only the tail of a 1 600-line log.

The green job's gate is the quoted line itself: the step ends with

```
grep -qE "Executed [0-9]+ tests?, with 0 failures" /tmp/swift-test.out
! grep -qE "with [1-9][0-9]* failures?" /tmp/swift-test.out
```

and the step exited 0, so `Executed 61 tests, with 0 failures` was printed and
no suite reported a failure. The run before it, over the same 61 cases,
printed the same line with `2 failures` — quoted in the table. The format is
corelibs-xctest's, indented with a tab, which is why the workflow does not
anchor the pattern at the start of the line (the first version did, matched
nothing, and failed a green run).

The suite also prints its own dynamic check count per case, which is where the
executed-check total comes from — the § 4 numbers are call sites, not
executions:

```
[generated-checks] ClipboardAutoClearTiming 225
[generated-checks] FeaturesHubStrings 456
[generated-checks] HardwareGatedInstalls 154
[generated-checks] MouseAppExceptionsIssue358 149
[generated-checks] WhatsAppDownloads 112
…
```

### 6.3 The three checks that do not port

The first run that got as far as executing them found exactly three failures,
all real differences rather than porting mistakes:

| check | why it fails on Linux | what was done |
|---|---|---|
| `App Switcher skips the focused-window read when the target is already minimized` | the counter it reads is bumped inside a helper closure that a *dropped* statement called, so it never reached 1 | fixed in the tool: poisoning now takes one step through the declaration of a local that a dropped statement touched |
| `every system tool the app runs is where it expects` | asserts `/bin/launchctl`, `/usr/bin/hdiutil`, `/usr/sbin/spctl` … exist on the machine running the tests | `EXCLUDED` in the generator, with the reason and the run |
| `hdiutil plist maps the canonical mount path back to its disk image` | `/tmp/Installer Mount` resolves to `/private/tmp/…` on macOS and to itself on Linux | `EXCLUDED`; it takes the two other checks in the same `if let` statement with it |

All three still run on macOS, unchanged.

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
