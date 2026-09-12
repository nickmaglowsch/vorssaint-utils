// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-14. Hand-written, not produced by Tools/linux-port/port-tests.py: these
// checks are new, not a selection out of the macOS harness.

import Foundation
import XCTest
@testable import VorssaintCore

final class SettingsStoreTests: XCTestCase {
    private var directory: URL!

    override func setUpWithError() throws {
        directory = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
            .appendingPathComponent("vorssaint-settings-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: directory)
    }

    private func makeStore(_ name: String = "settings.json") -> JSONSettingsStore {
        // flushInterval 0: a test that waits on a debounce tests the clock.
        JSONSettingsStore(url: directory.appendingPathComponent(name), flushInterval: 0)
    }

    /// Representative values per type, assigned to the keys by position so
    /// every one of the 644 keys carries a different shape than its
    /// neighbours and the sweep cannot pass by storing everything as a string.
    private func representativeValue(_ index: Int) -> SettingsValue {
        switch index % 7 {
        case 0: return .bool(index % 14 == 0)
        case 1: return .integer(index * 31 - 7)
        case 2: return .double(Double(index) / 8 + 0.5)
        case 3: return .string("value \(index) ✓ \u{1F600}")
        case 4:
            // Spelled out with explicit types rather than as one expression:
            // the one-liner this replaces made Swift 6.1 give up on Linux with
            // "unable to type-check this expression in reasonable time"
            // (run 34714111996), because the literal range's element type is
            // only pinned by a `&+` three levels in. Same bytes either way.
            let length = index % 23 + 1
            let seed = UInt8(index % 251)
            var bytes: [UInt8] = []
            bytes.reserveCapacity(length)
            for offset in 0..<length {
                bytes.append(UInt8(offset) &+ seed)
            }
            return .data(Data(bytes))
        case 5: return .array([.string("a\(index)"), .integer(index), .bool(index % 2 == 0)])
        default: return .dictionary(["n": .integer(index),
                                     "flag": .bool(index % 3 == 0),
                                     "text": .string("d\(index)")])
        }
    }

    // MARK: - The acceptance criterion: every key, both stores

    func testEveryKeyRoundTripsThroughTheJSONStore() throws {
        let keys = DefaultsKeyInventory.allKeys
        XCTAssertEqual(keys.count, Set(keys).count, "the key inventory has no duplicates")
        XCTAssertEqual(keys.count, 587 + FeatureSupportCatalog.allFeatureIDs.count,
                       "587 fixed keys plus one availability key per feature")

        let store = makeStore()
        var expected: [String: SettingsValue] = [:]
        for (index, key) in keys.enumerated() {
            let value = representativeValue(index)
            expected[key] = value
            store.set(value.anyValue, forKey: key)
        }
        store.flush()

        // A second store over the same file: this is the round trip that
        // matters, since it goes through the encoder, the file and back.
        let reopened = JSONSettingsStore(url: store.url, flushInterval: 0)
        for key in keys {
            let want = expected[key]!
            switch want {
            case .bool(let value):
                XCTAssertEqual(reopened.bool(forKey: key), value, key)
            case .integer(let value):
                XCTAssertEqual(reopened.integer(forKey: key), value, key)
            case .double(let value):
                XCTAssertEqual(reopened.double(forKey: key), value, accuracy: 1e-12, key)
            case .string(let value):
                XCTAssertEqual(reopened.string(forKey: key), value, key)
            case .data(let value):
                XCTAssertEqual(reopened.data(forKey: key), value, key)
            case .array:
                XCTAssertNotNil(reopened.array(forKey: key), key)
            case .dictionary:
                XCTAssertNotNil(reopened.dictionary(forKey: key), key)
            }
            XCTAssertEqual(SettingsValue.from(reopened.object(forKey: key) as Any), want,
                           "\(key) reads back as the shape it was written as")
        }
    }

    func testEveryKeyRoundTripsThroughUserDefaults() throws {
        let suite = "com.vorssaint.settings-test.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = UserDefaultsSettingsStore(defaults)

        for (index, key) in DefaultsKeyInventory.allKeys.enumerated() {
            let value = representativeValue(index)
            store.set(value.anyValue, forKey: key)
            XCTAssertEqual(SettingsValue.from(store.object(forKey: key) as Any), value,
                           "\(key) reads back as the shape it was written as")
        }
    }

    // MARK: - The store contract

    func testRegisteredDefaultsAndRemoval() {
        let store = makeStore()
        store.register(defaults: ["a": true, "b": 7, "c": "x"])
        XCTAssertTrue(store.bool(forKey: "a"))
        XCTAssertEqual(store.integer(forKey: "b"), 7)
        store.set(false, forKey: "a")
        XCTAssertFalse(store.bool(forKey: "a"))
        store.removeObject(forKey: "a")
        XCTAssertTrue(store.bool(forKey: "a"), "removal falls back to the registered default")
        XCTAssertNil(store.string(forKey: "never-written"))
        XCTAssertEqual(store.integer(forKey: "never-written"), 0)
    }

    func testABoolIsNotAOneAndAOneIsNotABool() {
        let store = makeStore()
        store.set(true, forKey: "flag")
        store.set(1, forKey: "count")
        store.flush()
        let reopened = JSONSettingsStore(url: store.url, flushInterval: 0)
        XCTAssertEqual(SettingsValue.from(reopened.object(forKey: "flag") as Any), .bool(true))
        XCTAssertEqual(SettingsValue.from(reopened.object(forKey: "count") as Any), .integer(1))
    }

    func testChangeNotification() {
        let store = makeStore()
        var seen: [Set<String>] = []
        let observation = store.observeChanges { seen.append($0) }
        store.set(true, forKey: "one")
        store.set(true, forKey: "one")          // unchanged: no second report
        store.set(2, forKey: "two")
        store.removeObject(forKey: "one")
        observation.cancel()
        store.set(3, forKey: "three")           // after cancel: not reported
        XCTAssertEqual(seen, [["one"], ["two"], ["one"]])
    }

    func testAtomicWriteLeavesNoTemporaryFilesBehind() throws {
        let store = makeStore()
        for index in 0..<50 { store.set(index, forKey: "k\(index)") }
        store.flush()
        let left = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        XCTAssertEqual(left.sorted(), ["settings.json"],
                       "the temp file is renamed over the real one, not left beside it")
    }

    func testACorruptFileIsMovedAsideAndReported() throws {
        let url = directory.appendingPathComponent("settings.json")
        try Data("{ \"formatVersion\": 1, \"settings\": { trunc".utf8).write(to: url)
        let store = JSONSettingsStore(url: url, flushInterval: 0)
        var reported: JSONSettingsStore.Recovery?
        store.recoveryHandler = { reported = $0 }

        XCTAssertFalse(store.bool(forKey: "anything"), "a damaged file reads as empty")
        let recovery = try XCTUnwrap(reported ?? store.lastRecovery)
        XCTAssertFalse(FileManager.default.fileExists(atPath: url.path),
                       "the damaged file is not left where the store would read it again")
        XCTAssertTrue(FileManager.default.fileExists(atPath: recovery.movedTo.path),
                      "it is moved aside, never deleted")
        XCTAssertTrue(recovery.movedTo.lastPathComponent.hasPrefix("settings.json.corrupt-"))
        XCTAssertFalse(recovery.reason.isEmpty, "the person is told what was wrong")

        store.set(true, forKey: "fresh")
        store.flush()
        XCTAssertTrue(JSONSettingsStore(url: url, flushInterval: 0).bool(forKey: "fresh"),
                      "the store carries on from the registered defaults")
    }

    func testAFileFromAFutureFormatIsNotGuessedAt() throws {
        let url = directory.appendingPathComponent("settings.json")
        try Data(#"{"formatVersion":99,"settings":{}}"#.utf8).write(to: url)
        let store = JSONSettingsStore(url: url, flushInterval: 0)
        _ = store.object(forKey: "anything")
        let recovery = try XCTUnwrap(store.lastRecovery)
        XCTAssertTrue(recovery.reason.contains("99"), recovery.reason)
    }

    func testDebouncedFlushCoalescesAndEventuallyWrites() throws {
        let url = directory.appendingPathComponent("settings.json")
        let store = JSONSettingsStore(url: url, flushInterval: 0.05)
        store.set(true, forKey: "a")
        store.set(true, forKey: "b")
        XCTAssertFalse(FileManager.default.fileExists(atPath: url.path),
                       "a write does not reach the disk on the spot")
        let written = expectation(description: "settings.json appears")
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) {
            if FileManager.default.fileExists(atPath: url.path) { written.fulfill() }
        }
        wait(for: [written], timeout: 3)
        let reopened = JSONSettingsStore(url: url, flushInterval: 0)
        XCTAssertTrue(reopened.bool(forKey: "a"))
        XCTAssertTrue(reopened.bool(forKey: "b"))
    }

    func testXDGConfigHomeIsHonouredAndFallsBackTheWayTheSpecSays() {
        let home = URL(fileURLWithPath: "/home/person", isDirectory: true)
        XCTAssertEqual(
            JSONSettingsStore.defaultURL(environment: ["XDG_CONFIG_HOME": "/xdg"],
                                         homeDirectory: home).path,
            "/xdg/vorssaint/settings.json")
        XCTAssertEqual(
            JSONSettingsStore.defaultURL(environment: [:], homeDirectory: home).path,
            "/home/person/.config/vorssaint/settings.json")
        XCTAssertEqual(
            JSONSettingsStore.defaultURL(environment: ["XDG_CONFIG_HOME": ""],
                                         homeDirectory: home).path,
            "/home/person/.config/vorssaint/settings.json",
            "an empty value is treated as unset, per the basedir spec")
        XCTAssertEqual(
            JSONSettingsStore.defaultURL(environment: ["XDG_CONFIG_HOME": "relative/path"],
                                         homeDirectory: home).path,
            "/home/person/.config/vorssaint/settings.json",
            "a relative value is treated as unset, per the basedir spec")
    }

    // MARK: - The backup crosses platforms

    /// The settings backup is an XML property list (`SettingsBackup.swift`)
    /// and the format does not change for the port, so a file written on one
    /// platform has to import on the other. `PropertyListSerialization` is on
    /// both toolchains, so this test runs on both legs of the gate: it writes
    /// a backup out of one store, reads it back into the other, and compares
    /// the shared keys value for value.
    func testABackupWrittenOnOnePlatformImportsOnTheOther() throws {
        let shared = Array(DefaultsKeyInventory.allKeys.prefix(200))
        let suite = "com.vorssaint.settings-test.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let mac = UserDefaultsSettingsStore(defaults)
        let linux = makeStore()

        var written: [String: SettingsValue] = [:]
        for (index, key) in shared.enumerated() {
            let value = representativeValue(index)
            written[key] = value
            mac.set(value.anyValue, forKey: key)
        }

        // Export, exactly as SettingsBackup.runExportPanel writes it.
        let payload: [String: Any] = [
            SettingsBackupFormat.formatVersionKey: SettingsBackupFormat.formatVersion,
            SettingsBackupFormat.appVersionKey: "3.3.5",
            SettingsBackupFormat.settingsKey: mac.snapshot(of: shared),
        ]
        let data = try PropertyListSerialization.data(fromPropertyList: payload,
                                                      format: .xml,
                                                      options: 0)

        // Import on the other side.
        let decoded = try XCTUnwrap(
            try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
                as? [String: Any])
        XCTAssertEqual(decoded[SettingsBackupFormat.formatVersionKey] as? Int,
                       SettingsBackupFormat.formatVersion)
        let settings = try XCTUnwrap(decoded[SettingsBackupFormat.settingsKey] as? [String: Any])
        XCTAssertEqual(settings.count, shared.count)
        for (key, value) in settings { linux.set(value, forKey: key) }
        linux.flush()

        let reopened = JSONSettingsStore(url: linux.url, flushInterval: 0)
        for key in shared {
            XCTAssertEqual(SettingsValue.from(reopened.object(forKey: key) as Any),
                           written[key], "\(key) survived the crossing")
        }

        // And back the other way, from the JSON store's own values.
        let returning = try PropertyListSerialization.data(
            fromPropertyList: [
                SettingsBackupFormat.formatVersionKey: SettingsBackupFormat.formatVersion,
                SettingsBackupFormat.appVersionKey: "3.3.5",
                SettingsBackupFormat.settingsKey: reopened.snapshot(of: shared),
            ],
            format: .xml, options: 0)
        let back = try XCTUnwrap(
            try PropertyListSerialization.propertyList(from: returning, options: [], format: nil)
                as? [String: Any])
        let backSettings = try XCTUnwrap(back[SettingsBackupFormat.settingsKey] as? [String: Any])
        let fresh = try XCTUnwrap(UserDefaults(suiteName: suite + ".back"))
        defer { fresh.removePersistentDomain(forName: suite + ".back") }
        let macAgain = UserDefaultsSettingsStore(fresh)
        for (key, value) in backSettings { macAgain.set(value, forKey: key) }
        for key in shared {
            XCTAssertEqual(SettingsValue.from(macAgain.object(forKey: key) as Any),
                           written[key], "\(key) survived the return crossing")
        }
    }
}
