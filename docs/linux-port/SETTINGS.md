# Settings on both platforms (WP-14)

Deliverable of WP-14. What the `SettingsStore` protocol promises, what the
Linux file looks like on disk, and — the part that matters for whoever picks
up the next feature — exactly which call sites still talk to `UserDefaults`
directly.

Everything here is in `Sources/VorssaintCore/Core/`:
`SettingsStore.swift`, `UserDefaultsSettingsStore.swift`,
`JSONSettingsStore.swift`, `SettingsStoreAccess.swift`,
`SettingsBackupFormat.swift` and the generated `DefaultsKeyInventory.swift`.

## 1. Why a protocol at all

`DefaultsKey` (`Sources/VorssaintCore/Core/DefaultsKeys.swift`, moved by
WP-11) stays exactly as it is: 587 `static let`s plus
`featureAvailable(_:)`. What had to change is the object the keys are handed
to. `UserDefaults` exists on Linux — swift-corelibs-foundation vends it — but
it is not the same thing:

| | macOS | swift-corelibs |
|---|---|---|
| Who writes | `cfprefsd`, out of process | this process |
| Coalescing | the daemon's | none |
| Crash safety | the daemon's | a plain write |
| Cross-process change notification | yes | no |
| `CFGetTypeID` to tell a `Bool` from a `1` | yes | **not vended** |

The last row is not a detail. `SettingsBackupSupport.valueLooksRight` decides
whether an imported value is the shape the setting expects, and on macOS it
does so with `CFGetTypeID(value) == CFBooleanGetTypeID()`; `CORE_MOVES.md`
records that as the named gap in that file. `value as? Bool` is not a
substitute: on Darwin it answers `true` for the number 1.

So the port owns the Linux file, and the protocol is what lets one core call
either.

## 2. The contract

```swift
public protocol SettingsStore: AnyObject {
    func object(forKey:) -> Any?
    func bool(forKey:) -> Bool
    func integer(forKey:) -> Int
    func double(forKey:) -> Double
    func string(forKey:) -> String?
    func data(forKey:) -> Data?
    func array(forKey:) -> [Any]?
    func stringArray(forKey:) -> [String]?
    func dictionary(forKey:) -> [String: Any]?

    func set(_ value: Any?, forKey:)
    func set(_ value: Bool, forKey:)
    func set(_ value: Int, forKey:)
    func set(_ value: Double, forKey:)

    func register(defaults: [String: Any])
    func removeObject(forKey:)
    func flush()
    func observeChanges(_ handler: @escaping (Set<String>) -> Void) -> SettingsObservation
}
```

It is `UserDefaults`' surface, method for method and overload for overload,
and that is the whole point: the 615 call sites in `Sources/Vorssaint` migrate
by changing *which object* they talk to and nothing else. A sweep that also
changed their shape would be a change the macOS gate cannot usefully prove
anything about.

Three additions, each because the Linux side needs it and macOS can answer it
for free:

- **`flush()`** makes pending writes durable. `UserDefaultsSettingsStore`
  does nothing (the system owns the schedule); `JSONSettingsStore` writes the
  file. A quit path, a settings export and every test call it.
- **`observeChanges`** returns a token that cancels on `deinit`. On macOS it
  reports each write by key and bridges `UserDefaults.didChangeNotification`
  as a keyless "something moved", which is what a write by another process
  actually tells you.
- **`snapshot(of:)`**, a protocol extension, is the reader
  `SettingsBackupSupport.payload(appVersion:valueFor:)` wants: values
  *including* the registered defaults, so an exported file reproduces a setup
  the person never touched a control for.

Nothing else from `UserDefaults` is in the protocol. Two call sites use
`persistentDomain(forName:)` and they both read *another application's*
domain — `com.apple.dock` in `DockPreviewService.swift:1247`, and the
preference-cleanup path in `Defaults.swift:1092`. That is not "our settings"
and it has no Linux meaning; it stays macOS-only.

### `SettingsValue`

Six shapes, which is what the 644 keys actually hold: `bool`, `integer`,
`double`, `string`, `data`, `array`, `dictionary`. There is no `date` case,
checked rather than assumed: every timestamp in the key table is written as a
`timeIntervalSince1970` `Double`.

`SettingsValue.from(_:)` classifies a value arriving as `Any`. It tests
`NSNumber` through **`objCType`**, not `CFGetTypeID` (not on Linux) and not
`as? Bool` (wrong on Darwin): a boxed boolean is `"c"` on both platforms, a
boxed `Double` is `"d"`, and a boxed integer is neither. Plain Swift
`Bool`/`Int`/`Double` are tried after that, for a platform where the
`NSNumber` bridge did not engage.

A value of any other shape is **refused**, not coerced: writing something the
store cannot read back would lose the setting on the next launch.

## 3. `UserDefaultsSettingsStore` (macOS)

Every method forwards to `UserDefaults`. No caching, no coalescing, no
validation. The behaviour of the macOS product is the system's behaviour, and
the proof that WP-14 changed none of it is the macOS leg still reporting

```
TESTS OK (31565 checks)
```

— the same number as before the package.

## 4. `JSONSettingsStore` (Linux)

### Where

`$XDG_CONFIG_HOME/vorssaint/settings.json`, falling back to
`~/.config/vorssaint/settings.json`. Per the basedir specification an *empty*
or *relative* `XDG_CONFIG_HOME` is treated as unset, not as a path; all four
cases are pinned in `SettingsStoreTests`.

### The file

```json
{
  "formatVersion" : 1,
  "settings" : {
    "keepAwakeAutoStart" : { "type" : "bool",    "value" : true },
    "smoothScrollStep" :   { "type" : "integer", "value" : 40 },
    "monitorRefreshSeconds" : { "type" : "double", "value" : 1.5 },
    "appLanguage" :        { "type" : "string",  "value" : "en-US" },
    "textSnippets" :       { "type" : "data",    "value" : "W3siaWQ…" },
    "panelSectionOrder" :  { "type" : "array",
                             "value" : [ { "type" : "string", "value" : "system" } ] }
  }
}
```

Written pretty-printed with sorted keys, so a diff of two settings files is
readable and a rewrite that changed nothing produces identical bytes.

**Why every value is tagged.** Untagged JSON cannot carry these settings.
`true` and `1` are the same number to one JSON reader or the other, `40`
versus `40.0` is exactly the distinction `valueLooksRight` drops a key over,
and `Data` has no JSON spelling at all. Decoding is driven by the tag rather
than by guessing from the literal, which is also why it cannot drift between
the two toolchains' `JSONDecoder`s. `data` is base64, `JSONEncoder`'s default.

`formatVersion` is there from the first release because the one thing a
settings file cannot do later is grow a version field. A file claiming a
version this build does not read is treated as damaged (below), not guessed
at.

### Atomic writes

A flush writes a temp file **in the same directory** (`rename(2)` is only
atomic within one filesystem), `fsync`s it, `rename`s it over
`settings.json`, and then `fsync`s the *directory* — without that last step a
power cut can lose the rename even though the file's bytes survived. A crash
at any point leaves either the old file or the new one, never half of either.
`SettingsStoreTests.testAtomicWriteLeavesNoTemporaryFilesBehind` asserts the
directory holds exactly `settings.json` afterwards.

### Debounced flush

`flushInterval` (0.5 s by default, `0` writes through) coalesces writes. The
app writes settings from sliders and pointer taps; an `fsync` per keystroke on
a laptop disk is not acceptable. `flush()` forces one, and `deinit` flushes.

### Corruption recovery

A file that does not parse — truncated, hand-edited into invalid JSON, or
carrying a `formatVersion` this build does not read — is **moved aside** to
`settings.json.corrupt-<ISO timestamp>`, reported through `recoveryHandler`
(and kept in `lastRecovery` for a caller that arrived too late to set one),
and the store carries on from the registered defaults.

It is never deleted and never silently ignored. Those bytes may be the only
copy of settings going back years, and quietly losing someone's configuration
is the one outcome worse than loudly losing it. The shell turns the report
into a notification naming both paths.

## 5. The settings backup crosses platforms unchanged

The backup format does not change for the port. It is still an XML property
list with three entries, and the three key names and the version now live in
`SettingsBackupFormat` in the core so both builds spell them the same way;
`SettingsBackupSupport` points its four constants at it and is otherwise
untouched.

`PropertyListSerialization` is on both toolchains, so
`SettingsStoreTests.testABackupWrittenOnOnePlatformImportsOnTheOther` runs on
**both** legs of the gate rather than only the Linux one. It writes a backup
out of a `UserDefaultsSettingsStore`, serialises it exactly as
`SettingsBackup.runExportPanel` does, reads it into a `JSONSettingsStore`,
compares every shared key value for value — and then does the return crossing.

The rest of `SettingsBackupSupport` (which keys travel, which never do,
`valueLooksRight`) stays in the Mac layer: it validates against thirty service
`Support` types that have not reached the core, and `valueLooksRight` is the
`CFGetTypeID` gap above. When those move, `SettingsValue` is what replaces
that function's three private helpers.

## 6. The key inventory

`DefaultsKeyInventory.swift` is generated by
`Tools/linux-port/defaults-inventory.py` from `DefaultsKeys.swift`, because
Swift cannot enumerate an enum's static members and WP-14's acceptance
criterion is a round trip over *every* key.

```
$ python3 Tools/linux-port/defaults-inventory.py --verify
defaults inventory OK (587 fixed keys)
```

587 fixed keys, plus one `featureAvailable.<id>` per feature from
`FeatureSupportCatalog.allFeatureIDs` (57), is **644**. `PLAN.md` § 3 says
"616 keys" and `WORK_PACKAGES.md` repeats it; that figure is from the survey
at 3.3.5 and the table has grown since. 644 is the counted number, and the
round-trip test walks all of them with a different value shape per key
position, so the sweep cannot pass by storing everything as a string.

The `--verify` run is a hard gate in `linux-port-ci`, step "The generated
tables match their sources": a key added to `DefaultsKeys.swift` without
regenerating fails the gate rather than quietly falling out of the test.

## 7. Migration state

`SettingsStoreAccess.shared` is the single accessor. On macOS it is
`UserDefaults.standard` wrapped; on Linux a `JSONSettingsStore` at the XDG
path. `SettingsStoreAccess.use(_:)` points it elsewhere — the Linux shell for
a file named on the command line, a test for a temporary directory.

**Migrated (WP-14):**

| File | What |
|---|---|
| `Sources/VorssaintCore/Core/Localization.swift` | `L10n`'s language read and write, the only `UserDefaults` call sites that were already in the core and covered by tests |

**Not migrated, deliberately — the follow-up list:**

| Where | Count | Who moves it |
|---|---|---|
| `Sources/Vorssaint/**` | 615 references across 113 files | per feature, as each one is ported. The heaviest are `KeepAwakeManager.swift` (29), `CommandBarService.swift` (28), `FanControlService.swift` (23), `ShelfService.swift` (22), `BrightnessService.swift` (22), `AppDelegate.swift` (22) |
| `Sources/Vorssaint/Core/Defaults.swift` | 20 | the `registeredDefaults` table is `register(defaults:)`'s argument and moves with the first Linux launch path; `preferenceCleanup` at line 1092 reads another app's domain and stays macOS-only |
| `Sources/VorssaintCore/Services/CommandBar/CommandBarSupport.swift` | 1 | `forgetAll(in defaults: UserDefaults = .standard)` — a parameter type, so changing it touches its macOS callers. It moves with the command bar (WP-B10) |
| `Sources/VorssaintCore/Bridge/FeatureRuntimeBridge.swift` | 2 | `DefaultsFeatureAvailabilityStore` (WP-18), which takes a `UserDefaults` the same way. It moves with the Linux hub |
| `Sources/Vorssaint/Services/DockPreview/DockPreviewService.swift` | 1 | never: `persistentDomain(forName: "com.apple.dock")` is a macOS-only read of another application's preferences, and `dockPreview` is a Drop feature |

The rule for whoever ports a feature: change `UserDefaults.standard` to
`SettingsStoreAccess.shared` in that feature's files, in the same PR, and
nothing else. The call sites do not change shape, so the diff is one
identifier per line and the macOS gate is a real check on it.

One thing to know before adding a file to the core: `build.sh` compiles
`Sources/Vorssaint`, `Sources/VorssaintCore` and `Sources/VorssaintMac` into
one `swiftc` invocation from an **explicit file list**, and `build.sh --test`
from a second one. A new core file that a harness file reaches has to be added
to that list, and no two files anywhere in those three directories may share a
basename (`PLATFORM.md` § 2). Both rules cost this package a red gate before
they were learned.
