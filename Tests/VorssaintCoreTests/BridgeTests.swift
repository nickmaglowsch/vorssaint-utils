// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Hand-written, not generated: `Tools/linux-port/port-tests.py` only writes
// files named `Generated*.swift`, so this one survives a regeneration.
//
// WP-18. What it covers: snapshot encode/decode round trips for all three
// adopted services, command dispatch to a fake service, the diffing
// behaviour, the unknown-service and malformed-JSON error paths, the
// threading contract, and that every `vs_snapshot` is matched by a `vs_free`.
// The last of those is why `BridgeCSurface` lives in `VorssaintCore` rather
// than beside the `@_cdecl` lines in `VorssaintLinux`: an executable target
// cannot be imported by a test target.

import XCTest
import Foundation
@testable import VorssaintCore

// MARK: - A fake service

/// A service with the smallest snapshot and command that are still honest:
/// something to change, something to refuse.
final class FakeBridgeService: BridgeService {
    static let bridgeID = "fake"

    struct Snapshot: Codable, Equatable {
        let value: Int
        let label: String
        let tags: [String]
    }

    enum Command: Equatable {
        case setValue(Int)
        case setLabel(String)
        case explode
    }

    private let bridge: CoreBridge
    private(set) var applied: [Command] = []
    var value = 0
    var label = "start"
    var tags: [String] = ["a", "b"]

    init(bridge: CoreBridge) { self.bridge = bridge }

    func bridgeSnapshot() -> Snapshot {
        Snapshot(value: value, label: label, tags: tags)
    }

    func apply(_ command: Command) throws {
        applied.append(command)
        switch command {
        case .setValue(let next): value = next
        case .setLabel(let next): label = next
        case .explode:
            throw BridgeError.rejected(service: Self.bridgeID, reason: "asked to explode")
        }
        publishToBridge(bridge)
    }
}

extension FakeBridgeService.Command: Codable {
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: BridgeCommandKey.self)
        let key = try BridgeCommandCoding.singleKey(container, in: decoder)
        switch key.stringValue {
        case "setValue": self = .setValue(try container.decode(Int.self, forKey: key))
        case "setLabel": self = .setLabel(try container.decode(String.self, forKey: key))
        case "explode": _ = try container.decode(Bool.self, forKey: key); self = .explode
        default: throw BridgeCommandCoding.unknownCase(key, decoder)
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: BridgeCommandKey.self)
        switch self {
        case .setValue(let v): try container.encode(v, forKey: BridgeCommandKey("setValue"))
        case .setLabel(let v): try container.encode(v, forKey: BridgeCommandKey("setLabel"))
        case .explode: try container.encode(true, forKey: BridgeCommandKey("explode"))
        }
    }
}

/// A snapshot that cannot encode, for the one error path a well-typed service
/// cannot reach by accident.
final class UnencodableBridgeService: BridgeService {
    static let bridgeID = "unencodable"

    struct Snapshot: Codable, Equatable {
        let value: Double
        // `Double.nan` is not representable in JSON and `JSONEncoder`'s
        // default `nonConformingFloatEncodingStrategy` is `.throw`.
    }

    enum Command: Codable, Equatable { case nothing }

    func bridgeSnapshot() -> Snapshot { Snapshot(value: .nan) }
    func apply(_ command: Command) throws {}
}

// MARK: - Tests

final class BridgeTests: XCTestCase {

    private var bridge: CoreBridge!
    private var service: FakeBridgeService!

    override func setUp() {
        super.setUp()
        bridge = CoreBridge()
        // Errors are expected in several cases below; keep them out of the log.
        bridge.diagnostic = { _ in }
        service = FakeBridgeService(bridge: bridge)
        bridge.register(service)
    }

    // MARK: Snapshot round trips

    func testEveryAdoptedSnapshotSurvivesARoundTrip() throws {
        let metrics = MetricsSnapshot(cpu: 41.5, history: [1, 2, 3.25],
                                      capacity: 60, source: "placeholder")
        let features = FeatureRuntimeSnapshot(
            features: [.init(id: "switcher", installed: true, installable: true),
                       .init(id: "mixer", installed: false, installable: false)],
            installedCount: 1, installableCount: 2,
            needsRestartToUnload: true, revision: 7)
        let l10n = L10nSnapshot(
            language: "pt-BR",
            languages: [.init(id: "en-US", displayName: "English (US)"),
                        .init(id: "pt-BR", displayName: "Português (Brasil)")],
            usesFewCountForm: false,
            strings: ["menuQuit": "Sair", "menuSettings": "Ajustes"])

        try assertRoundTrips(metrics)
        try assertRoundTrips(features)
        try assertRoundTrips(l10n)
        try assertRoundTrips(service.bridgeSnapshot())
    }

    private func assertRoundTrips<T: Codable & Equatable>(
        _ value: T, file: StaticString = #filePath, line: UInt = #line
    ) throws {
        let data = try BridgeJSON.encode(value)
        let back = try BridgeJSON.decode(T.self, from: data)
        XCTAssertEqual(value, back, "\(T.self) did not survive the round trip",
                       file: file, line: line)
        // And the encoding is reproducible, which is what the diff rests on.
        XCTAssertEqual(data, try BridgeJSON.encode(back),
                       "\(T.self) encodes differently the second time",
                       file: file, line: line)
    }

    func testSnapshotKeysComeOutSorted() throws {
        // Positions rather than the whole string: how Foundation spells a
        // `Double` is not this test's business, and differs between
        // corelibs-foundation and Darwin often enough to matter.
        let json = String(decoding: try BridgeJSON.encode(
            MetricsSnapshot(cpu: 1.5, history: [2.5], capacity: 60, source: "placeholder")),
                          as: UTF8.self)
        let positions = ["\"capacity\"", "\"cpu\"", "\"history\"", "\"source\""]
            .compactMap { json.range(of: $0)?.lowerBound }
        XCTAssertEqual(positions.count, 4, "every key is present: \(json)")
        XCTAssertEqual(positions, positions.sorted(),
                       "sorted keys are what makes a byte diff meaningful: \(json)")
    }

    func testTheMetricsSnapshotKeepsTheKeysTheWP01SparklineBinds() throws {
        // spikes/wp01-toolkit/qt-quick/qml/Panel.qml reads metrics.state.cpu
        // and metrics.state.history against the C stub. Renaming either is a
        // shell change, so it is asserted here rather than left to a reviewer.
        let object = try JSONSerialization.jsonObject(
            with: try BridgeJSON.encode(MetricsSnapshot(cpu: 3, history: [1, 2],
                                                        capacity: 60,
                                                        source: "placeholder")))
        let map = try XCTUnwrap(object as? [String: Any])
        XCTAssertNotNil(map["cpu"] as? Double ?? (map["cpu"] as? NSNumber)?.doubleValue)
        XCTAssertEqual((map["history"] as? [Any])?.count, 2)
    }

    // MARK: Command round trips and the wire format

    func testCommandsUseTheSingleKeyWireFormat() throws {
        func wire<T: Codable>(_ command: T) throws -> String {
            String(decoding: try BridgeJSON.encode(command), as: UTF8.self)
        }
        XCTAssertEqual(try wire(FeatureRuntimeCommand.install("switcher")),
                       #"{"install":"switcher"}"#)
        XCTAssertEqual(try wire(FeatureRuntimeCommand.uninstallAll),
                       #"{"uninstallAll":true}"#)
        XCTAssertEqual(try wire(L10nCommand.setLanguage("pt-BR")),
                       #"{"setLanguage":"pt-BR"}"#)
        XCTAssertEqual(try wire(MetricsCommand.reset), #"{"reset":true}"#)

        // And back, which is the direction the shell actually uses.
        XCTAssertEqual(try BridgeJSON.decode(FeatureRuntimeCommand.self,
                                             from: Data(#"{"install":"mixer"}"#.utf8)),
                       .install("mixer"))
        XCTAssertEqual(try BridgeJSON.decode(L10nCommand.self,
                                             from: Data(#"{"setLanguage":"ja"}"#.utf8)),
                       .setLanguage("ja"))
        XCTAssertEqual(try BridgeJSON.decode(MetricsCommand.self,
                                             from: Data(#"{"sample":12.5}"#.utf8)),
                       .sample(12.5))
    }

    // MARK: Dispatch

    func testACommandReachesTheService() throws {
        try bridge.command("fake", json: #"{"setValue":42}"#)
        XCTAssertEqual(service.applied, [.setValue(42)])
        XCTAssertEqual(service.value, 42)
        XCTAssertEqual(try bridge.snapshotJSON("fake"),
                       #"{"label":"start","tags":["a","b"],"value":42}"#)
    }

    func testAServiceMayRefuseACommand() {
        XCTAssertThrowsError(try bridge.command("fake", json: #"{"explode":true}"#)) { error in
            XCTAssertEqual(error as? BridgeError,
                           .rejected(service: "fake", reason: "asked to explode"))
            XCTAssertEqual((error as? BridgeError)?.cStatus, -2)
        }
    }

    // MARK: Error paths

    func testAnUnknownServiceIsRefusedEverywhere() {
        XCTAssertThrowsError(try bridge.snapshotJSON("nope")) {
            XCTAssertEqual($0 as? BridgeError, .unknownService("nope"))
        }
        XCTAssertThrowsError(try bridge.command("nope", json: "{}")) {
            XCTAssertEqual($0 as? BridgeError, .unknownService("nope"))
        }
        XCTAssertThrowsError(try bridge.subscribe(to: "nope") { _ in }) {
            XCTAssertEqual($0 as? BridgeError, .unknownService("nope"))
        }
        XCTAssertEqual(BridgeError.unknownService("nope").cStatus, -1)
    }

    func testMalformedCommandJSONIsRefusedWithItsOwnStatus() {
        for json in ["", "{", "[]", "{}", #"{"setValue":"not a number"}"#,
                     #"{"setValue":1,"setLabel":"x"}"#, #"{"noSuchCommand":1}"#] {
            XCTAssertThrowsError(try bridge.command("fake", json: json),
                                 "\(json) should not decode") { error in
                guard case .malformedCommand? = error as? BridgeError else {
                    return XCTFail("\(json) gave \(error), expected .malformedCommand")
                }
                XCTAssertEqual((error as? BridgeError)?.cStatus, -3)
            }
        }
        XCTAssertEqual(service.applied, [], "nothing reached the service")
    }

    func testASnapshotThatCannotEncodeFailsLoudly() {
        let unencodable = UnencodableBridgeService()
        bridge.register(unencodable)
        XCTAssertThrowsError(try bridge.snapshotJSON("unencodable")) { error in
            guard case .snapshotEncodingFailed? = error as? BridgeError else {
                return XCTFail("expected .snapshotEncodingFailed, got \(error)")
            }
        }
        XCTAssertThrowsError(try bridge.subscribe(to: "unencodable") { _ in },
                             "a subscriber that can never be fed is not registered")
        // And the C surface turns it into NULL rather than a crash.
        BridgeCSurface.bridge = bridge
        defer { BridgeCSurface.bridge = .shared }
        XCTAssertNil("unencodable".withCString { BridgeCSurface.snapshot($0) })
    }

    // MARK: Subscription and diffing

    func testSubscribeDeliversOnceImmediately() throws {
        var received: [String] = []
        let token = try bridge.subscribe(to: "fake") { received.append($0) }
        XCTAssertGreaterThanOrEqual(token, 1, "the C contract promises a token >= 1")
        XCTAssertEqual(received.count, 1, "the header promises one immediate delivery")
        XCTAssertEqual(received.first, #"{"label":"start","tags":["a","b"],"value":0}"#)
    }

    func testAnUnchangedSnapshotIsNotDelivered() throws {
        var received: [String] = []
        try bridge.subscribe(to: "fake") { received.append($0) }
        bridge.resetCounters()

        // A command that changes nothing: the same value, set again.
        try bridge.command("fake", json: #"{"setValue":0}"#)
        XCTAssertEqual(received.count, 1, "no delivery for an unchanged snapshot")
        XCTAssertEqual(bridge.suppressedCount, 1)
        XCTAssertEqual(bridge.deliveryCount, 0)

        // A command that changes something: one delivery.
        try bridge.command("fake", json: #"{"setValue":1}"#)
        XCTAssertEqual(received.count, 2)
        XCTAssertEqual(bridge.deliveryCount, 1)
        XCTAssertEqual(received.last, #"{"label":"start","tags":["a","b"],"value":1}"#)

        // Publishing again by hand, with nothing changed, is also suppressed.
        XCTAssertFalse(bridge.publish("fake"))
        XCTAssertEqual(received.count, 2)

        // …unless the caller invalidates, which is the escape hatch the shell
        // needs after a language change.
        bridge.invalidate("fake")
        XCTAssertTrue(bridge.publish("fake"))
        XCTAssertEqual(received.count, 3)
    }

    func testUnsubscribeStopsDelivery() throws {
        var received = 0
        let token = try bridge.subscribe(to: "fake") { _ in received += 1 }
        bridge.unsubscribe(token)
        try bridge.command("fake", json: #"{"setValue":9}"#)
        XCTAssertEqual(received, 1, "only the immediate delivery")
        bridge.unsubscribe(token)      // twice is a no-op, not a crash
        bridge.unsubscribe(9_999)      // and neither is a token nobody owns
    }

    func testTwoSubscribersStayInLockstep() throws {
        var a: [String] = []
        var b: [String] = []
        try bridge.subscribe(to: "fake") { a.append($0) }
        try bridge.command("fake", json: #"{"setValue":1}"#)
        try bridge.subscribe(to: "fake") { b.append($0) }
        try bridge.command("fake", json: #"{"setValue":2}"#)
        XCTAssertEqual(a.count, 3, "immediate, then two changes")
        XCTAssertEqual(b.count, 2, "immediate, then one change")
        XCTAssertEqual(a.last, b.last, "every subscriber sees the same bytes")
    }

    // MARK: Threading

    func testACallbackMayArriveFromAnotherThread() throws {
        let delivered = expectation(description: "snapshot delivered off the main thread")
        var wasMainThread = true
        try bridge.subscribe(to: "fake") { _ in
            // The immediate delivery is on this thread; only record the one
            // that comes from the queue below.
            if !Thread.isMainThread {
                wasMainThread = false
                delivered.fulfill()
            }
        }
        DispatchQueue.global().async {
            self.service.value = 77
            self.bridge.publish("fake")
        }
        wait(for: [delivered], timeout: 5)
        XCTAssertFalse(wasMainThread,
                       "the contract in corebridge.h is that the callback may arrive anywhere")
    }

    // MARK: The C surface

    func testTheCSurfaceAnswersAllFourCalls() throws {
        BridgeCSurface.bridge = bridge
        defer { BridgeCSurface.bridge = .shared }

        // vs_snapshot / vs_free
        let pointer = "fake".withCString { BridgeCSurface.snapshot($0) }
        let json = String(cString: try XCTUnwrap(pointer))
        XCTAssertEqual(json, #"{"label":"start","tags":["a","b"],"value":0}"#)
        BridgeCSurface.free(pointer)

        // vs_command
        XCTAssertEqual("fake".withCString { service in
            #"{"setLabel":"hello"}"#.withCString { BridgeCSurface.command(service, $0) }
        }, 0)
        XCTAssertEqual("fake".withCString { service in
            #"{"explode":true}"#.withCString { BridgeCSurface.command(service, $0) }
        }, -2)
        XCTAssertEqual("fake".withCString { service in
            "not json".withCString { BridgeCSurface.command(service, $0) }
        }, -3)
        XCTAssertEqual("nope".withCString { service in
            "{}".withCString { BridgeCSurface.command(service, $0) }
        }, -1)

        // vs_subscribe
        XCTAssertGreaterThanOrEqual("fake".withCString {
            BridgeCSurface.subscribe($0, { _, _ in }, nil)
        }, 1)
        XCTAssertEqual("nope".withCString {
            BridgeCSurface.subscribe($0, { _, _ in }, nil)
        }, -1)
        XCTAssertEqual("fake".withCString { BridgeCSurface.subscribe($0, nil, nil) }, -1,
                       "a NULL callback is refused, not dereferenced")
        XCTAssertNil("nope".withCString { BridgeCSurface.snapshot($0) })
    }

    func testEveryVsSnapshotIsMatchedByAVsFree() {
        BridgeCSurface.bridge = bridge
        defer { BridgeCSurface.bridge = .shared }
        let before = BridgeCSurface.liveSnapshotStrings

        var held: [UnsafeMutablePointer<CChar>] = []
        for index in 0..<200 {
            service.value = index
            guard let pointer = "fake".withCString({ BridgeCSurface.snapshot($0) }) else {
                return XCTFail("vs_snapshot returned NULL for a registered service")
            }
            held.append(pointer)
        }
        XCTAssertEqual(BridgeCSurface.liveSnapshotStrings, before + 200,
                       "200 outstanding allocations, all of them counted")

        // The bytes survive being held: a strdup'd copy, not a view into a
        // Swift string that has since been released.
        XCTAssertEqual(String(cString: held[0]),
                       #"{"label":"start","tags":["a","b"],"value":0}"#)

        for pointer in held { BridgeCSurface.free(pointer) }
        XCTAssertEqual(BridgeCSurface.liveSnapshotStrings, before,
                       "every vs_snapshot was matched by a vs_free")

        // A NULL free is the no-op C callers rely on.
        BridgeCSurface.free(nil)
        XCTAssertEqual(BridgeCSurface.liveSnapshotStrings, before)
    }

    // MARK: The three adopted services

    func testFeatureRuntimeInstallsAndUninstalls() throws {
        let store = FakeAvailabilityStore(ids: ["alpha", "beta", "gamma"],
                                          installed: ["alpha"],
                                          unsupported: ["gamma"])
        let features = FeatureRuntimeBridgeService(store: store, bridge: bridge)
        bridge.register(features)

        XCTAssertEqual(features.bridgeSnapshot().installedCount, 1)
        XCTAssertEqual(features.bridgeSnapshot().installableCount, 2,
                       "gamma is unsupported and not installed")
        XCTAssertFalse(features.bridgeSnapshot().needsRestartToUnload)

        try bridge.command("featureRuntime", json: #"{"install":"beta"}"#)
        XCTAssertTrue(store.installed.contains("beta"))
        XCTAssertEqual(features.bridgeSnapshot().installedCount, 2)

        // Uninstalling something that loaded this session asks for a restart.
        try bridge.command("featureRuntime", json: #"{"uninstall":"alpha"}"#)
        XCTAssertTrue(features.bridgeSnapshot().needsRestartToUnload)

        // The hardware gate refuses an install and never an uninstall.
        XCTAssertThrowsError(try bridge.command("featureRuntime",
                                                json: #"{"install":"gamma"}"#)) {
            XCTAssertEqual(($0 as? BridgeError)?.cStatus, -2)
        }
        XCTAssertThrowsError(try bridge.command("featureRuntime",
                                                json: #"{"install":"nothere"}"#))

        try bridge.command("featureRuntime", json: #"{"installAll":true}"#)
        XCTAssertEqual(store.installed, ["alpha", "beta"],
                       "install-all passes the same gate as a row, so gamma stays out")
        try bridge.command("featureRuntime", json: #"{"uninstallAll":true}"#)
        XCTAssertTrue(store.installed.isEmpty)
    }

    func testTheL10nSnapshotCarriesTheWholeCatalog() throws {
        let previous = L10n.shared.language
        defer { L10n.shared.language = previous }

        let l10n = L10nBridgeService(bridge: bridge)
        bridge.register(l10n)
        let snapshot = l10n.bridgeSnapshot()

        XCTAssertEqual(snapshot.languages.count, AppLanguage.allCases.count)
        XCTAssertEqual(snapshot.language, L10n.shared.language.rawValue)
        XCTAssertGreaterThan(snapshot.strings.count, 800,
                             "the catalog, not a sample of it")
        XCTAssertEqual(snapshot.strings["menuQuit"], L10n.shared.s.menuQuit)

        // Every field of `Strings` is a String, so `Mirror` drops nothing.
        let coverage = L10nBridgeService.catalogCoverage(L10n.shared.s)
        XCTAssertEqual(coverage.fields, coverage.strings,
                       "a non-String field of Strings would vanish from the catalog")
        XCTAssertEqual(coverage.fields, snapshot.strings.count)

        // Every language answers with a catalog of the same shape, which is
        // the check that the shell cannot render one language with holes in it.
        for language in AppLanguage.allCases {
            L10n.shared.language = language
            let next = l10n.bridgeSnapshot()
            XCTAssertEqual(next.language, language.rawValue)
            XCTAssertEqual(next.strings.count, coverage.fields,
                           "\(language.rawValue) is short of a field")
            XCTAssertEqual(next.usesFewCountForm, language == .ru,
                           "only Russian has a few-count form")
        }
    }

    func testSettingTheLanguageIsACommand() throws {
        let previous = L10n.shared.language
        defer { L10n.shared.language = previous }
        L10n.shared.language = .enUS

        let l10n = L10nBridgeService(bridge: bridge)
        bridge.register(l10n)
        var received: [String] = []
        try bridge.subscribe(to: "l10n") { received.append($0) }

        try bridge.command("l10n", json: #"{"setLanguage":"ja"}"#)
        XCTAssertEqual(L10n.shared.language, .ja)
        XCTAssertEqual(received.count, 2, "the change reached the subscriber")
        XCTAssertTrue(received[1].contains(#""language":"ja""#))

        // The same language again changes nothing, so nothing is delivered.
        try bridge.command("l10n", json: #"{"setLanguage":"ja"}"#)
        XCTAssertEqual(received.count, 2)

        XCTAssertThrowsError(try bridge.command("l10n", json: #"{"setLanguage":"kl"}"#)) {
            XCTAssertEqual(($0 as? BridgeError)?.cStatus, -2)
        }
    }

    func testTheMetricsRingBufferHoldsSixtySamples() throws {
        let metrics = MetricsBridgeService(bridge: bridge)
        bridge.register(metrics)

        for index in 0..<200 { metrics.record(Double(index % 100)) }
        let snapshot = metrics.bridgeSnapshot()
        XCTAssertEqual(snapshot.history.count, MetricsBridgeService.historyLength)
        XCTAssertEqual(snapshot.capacity, 60)
        XCTAssertEqual(snapshot.history.last, snapshot.cpu, "cpu is the newest sample")
        XCTAssertEqual(snapshot.history.first, Double(140 % 100),
                       "oldest first, and the oldest 140 samples are gone")

        // Out of range and non-finite samples are clamped at the source.
        metrics.record(1_000)
        XCTAssertEqual(metrics.bridgeSnapshot().cpu, 100)
        metrics.record(-5)
        XCTAssertEqual(metrics.bridgeSnapshot().cpu, 0)
        metrics.record(.nan)
        XCTAssertEqual(metrics.bridgeSnapshot().cpu, 0)

        XCTAssertThrowsError(try bridge.command("metrics", json: #"{"sample":null}"#))
        try bridge.command("metrics", json: #"{"reset":true}"#)
        XCTAssertTrue(metrics.bridgeSnapshot().history.isEmpty)
    }

    func testTheStandardServicesRegisterUnderTheIdsTheShellNames() {
        let standard = CoreBridge()
        standard.registerStandardServices()
        XCTAssertEqual(standard.registeredServiceIDs, ["featureRuntime", "l10n", "metrics"])
        XCTAssertNoThrow(try standard.snapshotJSON("metrics"))
        XCTAssertNoThrow(try standard.snapshotJSON("l10n"))
        XCTAssertNoThrow(try standard.snapshotJSON("featureRuntime"))
    }

    // MARK: The diffing measurement

    /// The number behind `BRIDGE.md` § "Diffing: a full snapshot with an
    /// unchanged fast path". It prints rather than only asserting, because the
    /// decision is a measurement and a measurement that only lives in a
    /// reviewer's memory is not one.
    func testDiffingBudgetOnASixtySampleHistory() throws {
        let metrics = MetricsBridgeService(bridge: bridge)
        bridge.register(metrics)

        // Fill the ring first: the interesting regime is the steady state,
        // where every tick drops the oldest sample and adds a new one.
        for index in 0..<60 { metrics.record(Double(index) * 1.37) }

        var fullBytes = 0
        var patchBytes = 0
        var previous = try JSONSerialization.jsonObject(
            with: BridgeJSON.encode(metrics.bridgeSnapshot()))

        for tick in 0..<60 {
            metrics.record(Double((tick * 7) % 100) + 0.25)
            let encoded = try BridgeJSON.encode(metrics.bridgeSnapshot())
            fullBytes += encoded.count
            let next = try JSONSerialization.jsonObject(with: encoded)
            let patch = MergePatch.between(previous, next)
            patchBytes += try JSONSerialization
                .data(withJSONObject: patch, options: [.sortedKeys]).count
            previous = next
        }

        // The unchanged case: sixty publishes with nothing moving.
        bridge.resetCounters()
        var deliveries = 0
        try bridge.subscribe(to: "metrics") { _ in deliveries += 1 }
        let idleStart = bridge.deliveredBytes
        for _ in 0..<60 { bridge.publish("metrics") }
        let idleBytes = bridge.deliveredBytes - idleStart

        print("[bridge-diff] metrics, 60 changing ticks: "
              + "full=\(fullBytes) B, merge-patch=\(patchBytes) B, "
              + "patch saves \(fullBytes - patchBytes) B "
              + "(\(String(format: "%.1f", 100.0 * Double(fullBytes - patchBytes) / Double(fullBytes))) %)")
        print("[bridge-diff] metrics, 60 idle publishes: "
              + "delivered=\(idleBytes) B in \(deliveries - 1) callbacks, "
              + "suppressed=\(bridge.suppressedCount)")

        let l10n = L10nBridgeService(bridge: bridge)
        bridge.register(l10n)
        let catalogBytes = try BridgeJSON.encode(l10n.bridgeSnapshot()).count
        print("[bridge-diff] l10n catalog snapshot: \(catalogBytes) B, "
              + "\(l10n.bridgeSnapshot().strings.count) strings")

        // The claim the decision rests on: a merge patch cannot express a
        // change inside an array, so on the one hot payload the bridge has it
        // resends the whole history and saves next to nothing.
        XCTAssertLessThan(Double(fullBytes - patchBytes) / Double(fullBytes), 0.10,
                          "a merge patch saves less than a tenth on a 60-sample history")
        // And the fast path is total when nothing moves.
        XCTAssertEqual(idleBytes, 0, "sixty idle publishes deliver nothing")
        XCTAssertEqual(bridge.suppressedCount, 60)
    }
}

// MARK: - Support

private final class FakeAvailabilityStore: FeatureAvailabilityStore {
    let featureIDs: [String]
    private(set) var installed: Set<String>
    private let unsupported: Set<String>

    init(ids: [String], installed: Set<String>, unsupported: Set<String>) {
        featureIDs = ids
        self.installed = installed
        self.unsupported = unsupported
    }

    func isInstalled(_ id: String) -> Bool { installed.contains(id) }
    func isInstallable(_ id: String) -> Bool { !unsupported.contains(id) }
    func setInstalled(_ id: String, _ value: Bool) {
        if value { installed.insert(id) } else { installed.remove(id) }
    }
}

/// RFC 7386 JSON merge patch, for the measurement in
/// `testDiffingBudgetOnASixtySampleHistory` and nowhere else.
///
/// It lives in the test rather than in the core on purpose: the core does not
/// ship a merge patch, and a measurement that rejects an alternative has to be
/// able to build the alternative.
private enum MergePatch {
    static func between(_ old: Any, _ new: Any) -> Any {
        guard let oldMap = old as? [String: Any], let newMap = new as? [String: Any] else {
            return new
        }
        var patch: [String: Any] = [:]
        for (key, value) in newMap {
            guard let existing = oldMap[key] else {
                patch[key] = value
                continue
            }
            if !equal(existing, value) { patch[key] = between(existing, value) }
        }
        for key in oldMap.keys where newMap[key] == nil {
            patch[key] = NSNull()
        }
        return patch
    }

    /// Structural equality over what `JSONSerialization` produces. Written out
    /// rather than leaning on `NSObject.isEqual`, whose bridging differs
    /// between corelibs-foundation and Darwin — which is precisely the kind of
    /// difference a measurement must not depend on.
    static func equal(_ a: Any, _ b: Any) -> Bool {
        switch (a, b) {
        case let (a as [String: Any], b as [String: Any]):
            return a.count == b.count && a.allSatisfy { key, value in
                b[key].map { equal(value, $0) } ?? false
            }
        case let (a as [Any], b as [Any]):
            return a.count == b.count && zip(a, b).allSatisfy { equal($0.0, $0.1) }
        case let (a as String, b as String):
            return a == b
        case let (a as NSNumber, b as NSNumber):
            return a == b
        case (is NSNull, is NSNull):
            return true
        default:
            return false
        }
    }
}
