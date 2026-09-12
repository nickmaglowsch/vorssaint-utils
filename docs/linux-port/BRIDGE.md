# `CoreBridge`: the one seam between the core and the shell (WP-18)

Deliverable of WP-18, and the convention every feature squad follows when it
adopts a service for the Linux shell.

`PLAN.md` § 4.2 chose one generic bridge over 232 view-specific bindings: the
Swift core exposes three calls over a C ABI, each service publishes a `Codable`
snapshot and accepts a `Codable` command, and the Qt side has exactly one
`CoreModel` that is instantiated per service and bound in QML. This file is
what that turned into, and what you have to do to put your service behind it.

Read with it: `linux/shell/include/corebridge.h` (the ABI),
`spikes/wp01-toolkit/qt-quick/CoreModel.cpp` (the consumer, already written),
and `PLATFORM.md` (the protocols underneath).

---

## 1. The checklist

Adopting a service is six steps and no `#if os(Linux)` anywhere.

**1. Declare the snapshot.** A `Codable & Equatable` struct of the state the
service's SwiftUI views already observe — its `@Published` properties, and
nothing computed that a view could compute itself.

```swift
public struct ClipboardSnapshot: Codable, Equatable {
    public let entries: [Entry]
    public let isPaused: Bool
    public let capacity: Int
}
```

Rules:
- **Values, not objects.** No class, no closure, no `Data` blob the QML cannot
  read. A thumbnail is a path or a shared-memory handle, never base64 in a
  snapshot that is re-encoded on every change.
- **`nil` means unknown, and says so in a doc comment.** The C layer's
  "unknown is never zero" rule (`linux/platform/README.md`) survives into the
  snapshot: a pid the backend could not read is `nil`, never `0`.
- **Cheap to build.** `bridgeSnapshot()` runs on every publish. Read the
  published properties; do not sample hardware, do not hit the disk.

**2. Declare the command enum.** One case per thing a view calls, with the
single-key wire format of § 2.

**3. Conform.** `BridgeService` wants the id, the snapshot and `apply`:

```swift
public final class ClipboardBridgeService: BridgeService {
    public static let bridgeID = "clipboard"
    public func bridgeSnapshot() -> ClipboardSnapshot { … }
    public func apply(_ command: ClipboardCommand) throws { … ; publishToBridge() }
}
```

`apply` **throws to refuse**. A refusal reaches the shell as `-2` and is a
thing a person did that cannot be done; make the snapshot say why, because the
status code alone cannot.

**4. Publish on every change.** End every mutation with `publishToBridge()`.
Over-publishing is free — the diff in § 4 drops anything that did not change —
and a missed publish is a view that silently stops updating, which is the
expensive failure of the two.

**5. Register.** Add it to `CoreBridge.registerStandardServices()` in
`Sources/VorssaintCore/Bridge/BridgeStandardServices.swift`, next to the three
that are there.

**6. Test it.** Copy the shape of `Tests/VorssaintCoreTests/BridgeTests.swift`:
a snapshot round trip, the exact wire bytes of every command, one refusal, one
malformed input. A service with no round-trip test is a service whose QML will
be written against a guess.

Then, on the shell side, the whole binding is:

```qml
CoreModel { id: clipboard; service: "clipboard" }
Text { text: clipboard.state.entries.length + " entries" }
Button { onClicked: clipboard.invoke('{"clear":true}') }
```

No C++ is written. That is the entire point.

---

## 2. The command wire format

**A command is one JSON object with exactly one key.** The key is the case
name; the value is the payload.

| Swift | JSON |
|---|---|
| `.install("switcher")` — one associated value | `{"install":"switcher"}` |
| `.setLanguage("pt-BR")` | `{"setLanguage":"pt-BR"}` |
| `.moveResize(id: 7, rect: …)` — several | `{"moveResize":{"id":7,"rect":{…}}}` |
| `.uninstallAll` — none | `{"uninstallAll":true}` |

The `true` for a payload-less case is deliberate: it keeps the object
non-empty, so "one key" is a rule with no exception and a decoder can reject
`{}` outright.

This is hand-written rather than synthesised. Swift's own enum `Codable`
spells `.install("switcher")` as `{"install":{"_0":"switcher"}}`; `_0` is a
compiler detail that would then appear in every QML file in the shell and in
every row of this table. `BridgeCommandKey` and `BridgeCommandCoding` in
`Sources/VorssaintCore/Bridge/BridgeCoding.swift` reduce each enum to about
fifteen lines — copy one of the three that exist. The WP-01 stub already spoke
this shape (`{"recordShortcut":"Ctrl+Alt+K"}`), which is the other reason it is
the convention.

Snapshots, by contrast, are plain synthesised `Codable`: they are structs, so
there is nothing to spell.

### Status codes

`vs_command` returns what `BridgeError.cStatus` says:

| C | `BridgeError` | Means |
|---|---|---|
| `0` | — | applied |
| `-1` | `.unknownService` | nothing is registered under that id |
| `-2` | `.rejected`, `.snapshotEncodingFailed` | the service refused; a person did something that cannot be done |
| `-3` | `.malformedCommand` | the JSON did not decode — a shell bug, not a user's |

`-2` and `-3` are different on purpose: `-2` belongs in the interface, `-3`
belongs in a log and in someone's inbox.

---

## 3. The header, and every way it differs from the WP-01 stub

`linux/shell/include/corebridge.h` is the contract.
`spikes/wp01-toolkit/corebridge-stub/corebridge.h` is the C stub the WP-01
bake-off measured Qt and GTK4 against. The `bridge-abi` CI job holds their
declarations identical:

```
$ grep -E '^(#|typedef|int |char |void |extern|\})' \
      spikes/wp01-toolkit/corebridge-stub/corebridge.h > /tmp/stub.decls
$ grep -E '^(#|typedef|int |char |void |extern|\})' \
      linux/shell/include/corebridge.h > /tmp/real.decls
$ diff -u /tmp/stub.decls /tmp/real.decls && echo IDENTICAL
IDENTICAL
```

What that leaves is every difference between the two files, and there are
exactly three. None of them is ABI.

1. **The prose above the include guard is rewritten.** The stub's block showed
   the Swift declarations it was standing in for; the real one shows the
   declarations that actually exist, plus the contract in § 5. A reader of the
   header should not have to open a second file to learn that the callback can
   arrive on another thread.
2. **The Swift parameters are `Optional` pointers.** The stub's comment showed
   `UnsafePointer<CChar>`; `CoreBridgeC.swift` writes `UnsafePointer<CChar>?`.
   An Optional pointer imports as `char *` either way — there is no ABI in the
   difference — and it is what lets the Swift side answer a NULL service name
   with `-1` instead of trapping. The C client calls
   `vs_subscribe("metrics", NULL, NULL)` and gets `-1` for exactly this reason.
3. **`vs_command` gained `-3`.** The stub documented `0`, `-1`, `-2`. The real
   header adds `-3` for JSON that would not decode, for the reason in § 2. It
   is the one line of the declaration region that differs, and it is inside a
   comment: `CoreModel::invoke` returns the `int` to QML unchanged, and QML
   already treats every non-zero as a failure, so nothing on the Qt side
   changes.

`typedef void (*vs_snapshot_callback)(const char *json, void *ctx)` keeps the
stub's name — the brief called it `vs_snapshot_cb`, but the header the shell
was actually built against says `vs_snapshot_callback` and the header is the
contract.

---

## 4. Diffing: a full snapshot with an unchanged fast path

**The decision: the bridge delivers whole snapshots, and delivers nothing when
the encoded bytes match what the subscribers already hold.** It does not send
JSON merge patches.

Three reasons, in the order that decided it.

**The consumer cannot apply a patch, and should not learn how.**
`CoreModel::applySnapshot` parses the JSON and *replaces* `m_state`:

```cpp
const QVariantMap next = doc.object().toVariantMap();
if (next == m_state) return;
m_state = next;
emit stateChanged();
```

A merge patch would need a base document kept per model, a recursive merge over
`QVariantMap`, and a rule for `null`-means-delete — C++ that every squad would
then be able to get subtly wrong. The bridge exists so that nobody writes that.

**A merge patch cannot express a change inside an array, so it saves nothing on
the one payload where saving matters.** RFC 7386 replaces arrays wholesale. The
metrics history is a 60-sample ring buffer that shifts by one every tick: every
tick changes the array, so every patch carries all sixty samples. Measured, by
`testDiffingBudgetOnASixtySampleHistory`, on CI rather than by hand — the CI
step prints the `[bridge-diff]` lines on every run:

```
[bridge-diff] metrics, 60 changing ticks: full=34876 B, merge-patch=32656 B, patch saves 2220 B (6.4 %)
[bridge-diff] metrics, 60 idle publishes: delivered=0 B in 0 callbacks, suppressed=60
[bridge-diff] l10n catalog snapshot: 54388 B, 974 strings
```

(Run 34714270680, job "Linux core (Swift 6.1)", step "Unit tests".)

**6.4 %.** That is the `capacity` and `source` keys — and, on the ticks where
the newest sample happens to repeat the previous one, `cpu` — that a patch can
leave out. The history is 94 % of the payload and the patch resends every byte
of it, every tick, because RFC 7386 has no way to say "one element changed".
For 6.4 % the shell would carry a merge implementation, a per-model base
document, and a null-means-delete rule.

**The fast path, by contrast, is total when nothing moves.** Sixty publishes
with no change deliver zero bytes in zero callbacks. That is the regime almost
everything in this app lives in: `l10n` is a 53 KB snapshot that changes when
somebody picks a language, `featureRuntime` changes when somebody installs
something, and both are published far more often than they change, because
§ 1's rule is "publish on every mutation and let the bridge decide". A merge
patch saves nothing at all here — it is `{}`, which still has to be delivered
and parsed, unless the sender diffs first, at which point it is doing this.

Consequences to know about:

- **Key order is load-bearing.** The diff is `Data != Data`, so the encoder
  sets `.sortedKeys`. An encoder free to reorder a dictionary would report
  every snapshot as changed. `testSnapshotKeysComeOutSorted` holds it.
- **The diff is per service, not per subscriber.** Every subscriber of a
  service receives identical bytes, and a new one is fed its own immediate
  delivery by `subscribe`, so they stay in lockstep
  (`testTwoSubscribersStayInLockstep`).
- **`CoreBridge.invalidate(_:)` is the escape hatch** for the case where the
  snapshot is unchanged but the world is not — the shell calls it after a
  language change so every service re-delivers into the new catalogs.
- **The Qt side keeps its own `next == m_state` check.** Belt to the bridge's
  braces, and it costs one comparison of a map that was going to be built
  anyway.

---

## 5. Threading

The contract is in the header, in the form `linux/platform/README.md` uses for
the platform vtable. In short:

| | |
|---|---|
| `vs_subscribe`, `vs_command`, `vs_snapshot` | called **only** from the thread that runs the core — in the shell, the GUI thread, which is also the thread the Swift services already require ("main thread only"). `CoreModel::setService` and `CoreModel::invoke` are reached from QML, so this already holds. |
| the snapshot callback | may arrive on **any** thread: whichever one called `publish`. |
| the callback's `json` pointer | owned by the bridge, valid for the duration of the call only. |
| the callback itself | must not block. It runs with the bridge's lock held. |

The Qt side was written against this before the Swift side existed, and matches
it exactly:

```cpp
void CoreModel::snapshotTrampoline(const char *json, void *ctx) {
    // The header says the pointer dies with the call, so copy before the hop.
    auto *self = static_cast<CoreModel *>(ctx);
    QMetaObject::invokeMethod(self, "applySnapshot", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(json)));
}
```

`QString::fromUtf8` is the copy; `Qt::QueuedConnection` is the hop. A queued
connection to an object owned by the GUI thread runs on the GUI thread whether
the caller was on it or not, so the same code is correct for a main-thread
publish and for a sampler's.

### Why the callback may arrive anywhere, when today it never does

Every publisher in the core is on the main thread right now, and a contract
that said so would be simpler. It would also have to be broken by WP-A1, whose
sensors sample on their own cadence, and by WP-B1's capture stream, which
dispatches on the PipeWire loop. Breaking a threading contract after the shell
has been written against it is how a port acquires a class of bug it cannot
test for, so the weaker promise is made now, when it costs one `QueuedConnection`
that the Qt side had already written.

`testACallbackMayArriveFromAnotherThread` publishes from
`DispatchQueue.global()` and asserts the callback did not arrive on the main
thread, so the permission is exercised rather than merely documented.

### Why callbacks run under the lock

Two publishes from different threads must not interleave their deliveries —
a subscriber that received snapshot *n* after *n+1* would render stale state
and never be corrected, because the next publish is diffed against *n+1*. The
lock is `NSRecursiveLock` so that the ordinary pattern — `apply` mutates, then
calls `publishToBridge`, whose callback re-enters the bridge on the same
thread — is safe.

### Swift 6 concurrency

The package is on `swift-tools-version: 5.9`, so Swift 5 language mode, and
`CoreBridge` is `@unchecked Sendable` with an explicit `NSRecursiveLock`
rather than an actor. That is not a stopgap. `@_cdecl` functions cannot be
`async` and cannot inherit an actor's isolation, so the C boundary *must* be a
synchronous lock-guarded one no matter what the rest of the package does. When
the package moves to the Swift 6 mode, the services stay `@MainActor`, the
lock stays the stated reason for `@unchecked`, and the boundary does not move —
only the annotations do.

The core's "main thread only" convention is crossed in exactly one place and in
one direction: a service publishes from wherever it is, and the bridge hands the
bytes out from there. Nothing crosses inward: `vs_command` runs `apply` on the
caller's thread, and the caller is contractually the core's own thread.

---

## 6. The three services adopted, and what is still a placeholder

| id | Swift | Snapshot | Commands |
|---|---|---|---|
| `featureRuntime` | `FeatureRuntimeBridgeService` | `features[]` (id, installed, installable), counts, `needsRestartToUnload`, `revision` | `install`, `uninstall`, `installAll`, `uninstallAll` |
| `l10n` | `L10nBridgeService` | `language`, `languages[]`, `usesFewCountForm`, `strings` (974 keys) | `setLanguage` |
| `metrics` | `MetricsBridgeService` | `cpu`, `history[≤60]`, `capacity`, `source` | `sample`, `reset` |

**`featureRuntime` is the persisted half of `FeatureRuntime`, not a wrapper of
it.** `Sources/Vorssaint/App/FeatureRuntime.swift` imports AppKit, terminates
`NSApp` and names thirty service singletons in its bindings table; it cannot
move into the core, and a bridge that called it would only build on macOS. What
it and the Linux hub genuinely share is the persisted answer, so
`FeatureAvailabilityStore` is that answer and
`DefaultsFeatureAvailabilityStore` reads and writes the same
`featureAvailable.<id>` keys through `DefaultsKey.featureAvailable`. The
install gate is `FeatureRuntime.mayFlip`'s, asymmetry included: an install the
hardware refuses is refused out loud, an uninstall is never refused and an
existing install is never revoked. WP-15 moves `FeatureCatalog` into the core
and `LinuxFeatureProbeSet` — a twelve-id placeholder naming only features whose
Linux backend has landed or is in flight — becomes
`AppFeature.allCases.map(\.rawValue)`. Until then a CI grep proves every one of
those twelve is a real `AppFeature` case, which is the only check that can
cross the module boundary the list sits on the wrong side of.

**`l10n` carries the whole catalog because the alternative is a second set of
translations.** The playbook's rule is that every user-facing string goes
through `Strings` in all thirteen languages and that the compiler is the check.
QML has no compiler, so what keeps the rule is that the QML reads out of the
same struct: `l10n.state.strings.menuQuit` is `L10n.shared.s.menuQuit`. The
flattening is `Mirror`, not a generated table, because `Strings` gains fields in
nearly every feature PR and a generated table is one more thing to forget;
`catalogCoverage` reports the field count and the string count separately and a
test holds them equal, so a field that is not a `String` cannot vanish silently.

**`metrics` is a ring buffer and nothing else.** No `/proc` read, no sampler:
`SystemSensors` (`PLATFORM.md` § 2) is where that belongs, and a second reader
of the same hardware is what `PLATFORM.md` § 4 tells everyone not to write. The
keys are `cpu` and `history` because `spikes/wp01-toolkit/qt-quick/qml/Panel.qml`
already binds those against the C stub, which makes the swap from stub to real
core a zero-line change to the QML — the cheapest possible proof that the ABI
is the same one. `source` is `"placeholder"` until WP-A1, and the panel is meant
to badge anything that is not `"sensors"` so a probe series cannot be mistaken
for a measurement. WP-A1 keeps the type, deletes
`MetricsCommand.sample`, and calls `record(_:)` from the sensor's cadence.

---

## 7. Wiring the Qt side

`spikes/wp01-toolkit/qt-quick/CMakeLists.txt` gained an option:

```sh
cmake -B build -DVORSSAINT_REAL_BRIDGE=ON \
      -DVORSSAINT_BRIDGE_LIB=/tmp/libvorssaintbridge.a
```

which swaps the C stub for the Swift archive. Not one line of `CoreModel.cpp`,
`main.cpp` or the QML changes between the two.

**Why the option is in the spike and not in a new `linux/shell/` app.**
`linux/shell` is WP-20's deliverable — the Qt Quick executable, the main loop,
single-instance, `--selftest`. A skeleton of it now would be something WP-20
has to delete. What WP-18 does put under `linux/shell` is the two files WP-20
will keep unchanged: `include/corebridge.h`, which is the contract, and
`tests/abi_client.c`, which is the conformance test for it.

---

## 8. The CI evidence

The `bridge-abi` job in `.github/workflows/linux-port-ci.yml`. There is no
Swift toolchain on the machines the port's agents run on, so CI is the
compiler and this leg is the only thing that proves something about the
*linked* product rather than about the Swift front end.

1. **The header still declares the WP-01 stub's ABI** — the `diff` in § 3.
2. **Every `LinuxFeatureProbeSet` id is a real `AppFeature`** — the grep of
   § 6.
3. **`swift build --target VorssaintLinux`** — the `@_cdecl` file compiles as
   part of the real target.
4. **`ar` it into `libvorssaintbridge.a`** from the objects SwiftPM produced
   for `VorssaintCore`, `VorssaintLinux` (minus `main.swift.o`, whose `main`
   the C client brings its own of) and the OpenCombine products the core needs
   on Linux — SwiftPM builds executables, not archives. `nm --defined-only -g`
   must then find all four symbols as `T`, undecorated, in the archive itself;
   the step fails by name if any is missing.
5. **Compile `linux/shell/tests/abi_client.c`** with
   `$CC -std=c11 -Wall -Wextra -Werror -I linux/shell/include`, and nothing
   else on the include path. A signature the header gets wrong is a compile
   error here; a symbol it names wrongly is a link error in the next step.
   It is C and not C++ deliberately: `extern "C"` would paper over a name
   mismatch that plain C catches.
6. **Link it with `swiftc`** and **run it**. `swiftc` is the linker driver
   because the Swift runtime's autolink entries live in those objects;
   `swift-autolink-extract` turns them into the `-l` flags, and `-lstdc++`
   is added by hand because OpenCombine's lock is a C++ translation unit.
   The client calls all four functions against all three services, checks
   every status code, and does a thousand `vs_snapshot`/`vs_free` round
   trips.

Run 34715155527, job "Bridge C ABI (static library + C client)", verbatim:

```
=== running the C client ===
CoreBridge: vs_snapshot("nosuchservice") = NULL: unknownService("nosuchservice")
CoreBridge: vs_command("metrics") = -3: malformedCommand(service: "metrics", reason: "dataCorrupted(…\"The given data was not valid JSON.\"…)")
CoreBridge: vs_command("l10n") = -2: rejected(service: "l10n", reason: "no language \"kl\"")
metrics snapshot: {"capacity":60,"cpu":0,"history":[],"source":"placeholder"}
[ ok ] metrics snapshot has cpu
[ ok ] metrics snapshot has history
[ ok ] metrics capacity is 60
[ ok ] metrics snapshot keys are sorted
l10n snapshot: 54388 bytes
[ ok ] l10n snapshot has language
[ ok ] l10n snapshot carries the catalog
[ ok ] the catalog is the whole catalog
featureRuntime snapshot: {"features":[{"id":"switcher","installable":true,"installed":false}, … ],"installableCount":12,"installedCount":0,"needsRestartToUnload":false,"revision":0}
[ ok ] featureRuntime snapshot lists features
[ ok ] vs_snapshot of an unknown service is NULL
[ ok ] 1000 vs_snapshot/vs_free round trips
[ ok ] vs_subscribe returns a token >= 1
[ ok ] vs_subscribe fires once immediately
[ ok ] vs_subscribe of an unknown service is -1
[ ok ] a NULL callback is refused, not dereferenced
[ ok ] vs_command accepted
[ ok ] the accepted command pushed a snapshot
after sample: {"capacity":60,"cpu":42.5,"history":[42.5],"source":"placeholder"}
[ ok ] the new sample is in the snapshot
[ ok ] the same sample again is accepted
[ ok ] …and appending it is a real change, so it is delivered
[ ok ] unknown service is -1
[ ok ] malformed JSON is -3
[ ok ] an unknown command is -3
[ ok ] a command the service refuses is -2
[ ok ] l10n accepts a language
[ ok ] the language really changed
[ ok ] featureRuntime accepts an install
[ ok ] …and refuses one it does not have
[ ok ] metrics accepts a reset
[ ok ] the reset changed the snapshot, so it was delivered
[ ok ] a second reset is accepted
[ ok ] …and delivers nothing, because the snapshot did not change
OK: 0 check(s) failed
```

Two things worth noticing in that output. The `CoreBridge:` lines are the
`diagnostic` hook: every status the C boundary flattens into an `int` is also
written out with the reason, which is what makes `-2` and `-3` actionable for
whoever reads the shell's log. And the last two checks are the diff, observed
from C: a second `{"reset":true}` is accepted and produces no callback,
because the snapshot it would have produced is the one the subscriber holds.

The Swift-side memory test lives in `VorssaintCoreTests`
(`testEveryVsSnapshotIsMatchedByAVsFree`, 200 outstanding allocations counted
and released) rather than in the C client, because the counter it reads is
inside `BridgeCSurface` and the C client cannot see it. That is also why
`BridgeCSurface` is in `VorssaintCore` and `CoreBridgeC.swift` is four one-line
`@_cdecl` wrappers: an `@_cdecl` function in an executable target cannot be
imported by a test target, so anything left beside it has no test but a CI job.
