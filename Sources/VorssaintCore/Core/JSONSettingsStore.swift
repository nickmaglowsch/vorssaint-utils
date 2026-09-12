// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

/// The Linux settings store: one JSON file under `$XDG_CONFIG_HOME`.
///
/// There is no `UserDefaults` daemon on Linux — swift-corelibs-foundation's
/// `UserDefaults` is a plist written by the process itself, with none of
/// `cfprefsd`'s coalescing, crash safety or cross-process notification — so
/// the port owns the file. The three properties that daemon gave us for free
/// are the three this type has to provide:
///
/// - **Atomicity.** A write goes to a temp file in the same directory, is
///   `fsync`ed, and is then `rename`d over the real one. A crash leaves
///   either the old file or the new one, never half of either.
/// - **Coalescing.** Writes are debounced (`flushInterval`), because the app
///   writes settings from sliders and pointer taps and a per-keystroke
///   `fsync` on a laptop disk is not acceptable.
/// - **Recovery.** A file that does not parse is moved aside, reported
///   through `recoveryHandler`, and the store starts from the registered
///   defaults. It is never silently deleted and never silently ignored:
///   losing someone's settings quietly is the one outcome worse than losing
///   them loudly.
///
/// The format is documented in `docs/linux-port/SETTINGS.md`.
///
/// It compiles on macOS as well as Linux (it is plain Foundation plus
/// `rename`/`fsync`), which is what lets the cross-platform backup test run
/// on both legs of the gate rather than only on the Linux one.
public final class JSONSettingsStore: SettingsStore {
    /// What was wrong with the file that was there, and where it was put.
    public struct Recovery: Equatable {
        public let url: URL
        public let movedTo: URL
        public let reason: String
    }

    public let url: URL

    /// How long a write waits for its neighbours before the file is rewritten.
    /// `0` writes through, which is what the tests use.
    public var flushInterval: TimeInterval

    /// Called when a damaged file was moved aside. Set it before the first
    /// read; the shell turns it into a notification the person can act on.
    public var recoveryHandler: ((Recovery) -> Void)?

    /// The most recent recovery, for a caller that came too late to have set
    /// a handler.
    public private(set) var lastRecovery: Recovery?

    private let formatVersion = 1
    private let queue = DispatchQueue(label: "com.vorssaint.settings-store")
    private let lock = NSRecursiveLock()

    private var values: [String: SettingsValue] = [:]
    private var registered: [String: SettingsValue] = [:]
    private var observers: [UUID: (Set<String>) -> Void] = [:]
    private var pendingKeys: Set<String> = []
    private var flushScheduled = false
    private var didLoad = false

    /// `$XDG_CONFIG_HOME/vorssaint/settings.json`, with the specification's
    /// fallback to `~/.config` when the variable is unset *or empty* — the
    /// spec says a relative or empty value is to be treated as unset.
    public static func defaultURL(
        environment: [String: String] = ProcessInfo.processInfo.environment,
        homeDirectory: URL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
    ) -> URL {
        let configured = environment["XDG_CONFIG_HOME"] ?? ""
        let base: URL = configured.hasPrefix("/")
            ? URL(fileURLWithPath: configured, isDirectory: true)
            : homeDirectory.appendingPathComponent(".config", isDirectory: true)
        return base
            .appendingPathComponent("vorssaint", isDirectory: true)
            .appendingPathComponent("settings.json", isDirectory: false)
    }

    public init(url: URL? = nil, flushInterval: TimeInterval = 0.5) {
        self.url = url ?? JSONSettingsStore.defaultURL()
        self.flushInterval = flushInterval
    }

    deinit { flushNow() }

    // MARK: - Reading

    public func object(forKey key: String) -> Any? { storedValue(key)?.anyValue }

    public func bool(forKey key: String) -> Bool { storedValue(key)?.boolValue ?? false }

    public func integer(forKey key: String) -> Int { storedValue(key)?.integerValue ?? 0 }

    public func double(forKey key: String) -> Double { storedValue(key)?.doubleValue ?? 0 }

    public func string(forKey key: String) -> String? { storedValue(key)?.stringValue }

    public func data(forKey key: String) -> Data? {
        guard case .data(let value)? = storedValue(key) else { return nil }
        return value
    }

    public func array(forKey key: String) -> [Any]? {
        guard case .array(let values)? = storedValue(key) else { return nil }
        return values.map(\.anyValue)
    }

    public func stringArray(forKey key: String) -> [String]? {
        guard case .array(let values)? = storedValue(key) else { return nil }
        let strings = values.compactMap { element -> String? in
            guard case .string(let value) = element else { return nil }
            return value
        }
        return strings.count == values.count ? strings : nil
    }

    public func dictionary(forKey key: String) -> [String: Any]? {
        guard case .dictionary(let values)? = storedValue(key) else { return nil }
        return values.mapValues(\.anyValue)
    }

    /// The value, or the registered default behind it. Loading is lazy so a
    /// store can be constructed before the home directory is known good.
    private func storedValue(_ key: String) -> SettingsValue? {
        lock.lock()
        defer { lock.unlock() }
        loadIfNeeded()
        return values[key] ?? registered[key]
    }

    // MARK: - Writing

    public func set(_ value: Any?, forKey key: String) {
        guard let value else {
            removeObject(forKey: key)
            return
        }
        guard let stored = SettingsValue.from(value) else {
            // Refusing is the honest answer: writing a shape this store
            // cannot read back would lose the setting on the next launch.
            return
        }
        write(stored, forKey: key)
    }

    public func set(_ value: Bool, forKey key: String) { write(.bool(value), forKey: key) }
    public func set(_ value: Int, forKey key: String) { write(.integer(value), forKey: key) }
    public func set(_ value: Double, forKey key: String) { write(.double(value), forKey: key) }

    public func register(defaults registrations: [String: Any]) {
        lock.lock()
        for (key, value) in registrations {
            guard let value = SettingsValue.from(value) else { continue }
            registered[key] = value
        }
        lock.unlock()
    }

    public func removeObject(forKey key: String) {
        lock.lock()
        loadIfNeeded()
        let existed = values.removeValue(forKey: key) != nil
        if existed { pendingKeys.insert(key) }
        lock.unlock()
        guard existed else { return }
        scheduleFlush()
        announce([key])
    }

    private func write(_ value: SettingsValue, forKey key: String) {
        lock.lock()
        loadIfNeeded()
        let changed = values[key] != value
        values[key] = value
        if changed { pendingKeys.insert(key) }
        lock.unlock()
        guard changed else { return }
        scheduleFlush()
        announce([key])
    }

    // MARK: - Change notification

    public func observeChanges(_ handler: @escaping (Set<String>) -> Void) -> SettingsObservation {
        let id = UUID()
        lock.lock()
        observers[id] = handler
        lock.unlock()
        return SettingsObservation { [weak self] in
            guard let self else { return }
            self.lock.lock()
            self.observers[id] = nil
            self.lock.unlock()
        }
    }

    private func announce(_ keys: Set<String>) {
        lock.lock()
        let handlers = Array(observers.values)
        lock.unlock()
        for handler in handlers { handler(keys) }
    }

    // MARK: - Persistence

    public func flush() { flushNow() }

    private func scheduleFlush() {
        guard flushInterval > 0 else {
            flushNow()
            return
        }
        lock.lock()
        let alreadyScheduled = flushScheduled
        flushScheduled = true
        lock.unlock()
        guard !alreadyScheduled else { return }
        queue.asyncAfter(deadline: .now() + flushInterval) { [weak self] in
            self?.flushNow()
        }
    }

    private func flushNow() {
        lock.lock()
        flushScheduled = false
        guard !pendingKeys.isEmpty else {
            lock.unlock()
            return
        }
        pendingKeys.removeAll()
        let snapshot = values
        lock.unlock()
        try? writeAtomically(snapshot)
    }

    /// Temp file in the same directory, `fsync`, `rename`, `fsync` the
    /// directory. Same directory because `rename(2)` is only atomic within one
    /// filesystem; the directory `fsync` because without it the rename itself
    /// can be lost across a power cut even though the file's bytes were not.
    private func writeAtomically(_ snapshot: [String: SettingsValue]) throws {
        let directory = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        let file = SettingsFile(formatVersion: formatVersion, settings: snapshot)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(file)

        let temporary = directory.appendingPathComponent(
            ".settings.json.\(ProcessInfo.processInfo.processIdentifier).\(UInt32.random(in: 0...UInt32.max))")
        try data.write(to: temporary)
        if let handle = try? FileHandle(forWritingTo: temporary) {
            try? handle.synchronize()
            try? handle.close()
        }
        let renamed = temporary.withUnsafeFileSystemRepresentation { source in
            url.withUnsafeFileSystemRepresentation { destination -> Bool in
                guard let source, let destination else { return false }
                return rename(source, destination) == 0
            }
        }
        guard renamed else {
            try? FileManager.default.removeItem(at: temporary)
            throw CocoaError(.fileWriteUnknown)
        }
        syncDirectory(directory)
    }

    private func syncDirectory(_ directory: URL) {
        directory.withUnsafeFileSystemRepresentation { path in
            guard let path else { return }
            let descriptor = open(path, O_RDONLY)
            guard descriptor >= 0 else { return }
            _ = fsync(descriptor)
            close(descriptor)
        }
    }

    /// Reads the file once. A file that is not there is not an error — it is
    /// a first launch.
    private func loadIfNeeded() {
        guard !didLoad else { return }
        didLoad = true
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            recover(reason: "reading \(url.path): \(error.localizedDescription)")
            return
        }
        do {
            let file = try JSONDecoder().decode(SettingsFile.self, from: data)
            guard file.formatVersion >= 1, file.formatVersion <= formatVersion else {
                recover(reason: "settings.json says format version "
                        + "\(file.formatVersion); this build reads 1...\(formatVersion)")
                return
            }
            values = file.settings
        } catch {
            recover(reason: "settings.json is not readable JSON: "
                    + "\(error.localizedDescription)")
        }
    }

    /// Moves the damaged file aside and reports it. Never deletes: the bytes
    /// are the only copy of settings that may go back years, and a person or
    /// a support thread can still get something out of them.
    private func recover(reason: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
        let aside = url.deletingLastPathComponent()
            .appendingPathComponent("settings.json.corrupt-\(stamp)")
        try? FileManager.default.removeItem(at: aside)
        try? FileManager.default.moveItem(at: url, to: aside)
        let recovery = Recovery(url: url, movedTo: aside, reason: reason)
        lastRecovery = recovery
        recoveryHandler?(recovery)
    }
}

/// The on-disk shape. Versioned from the first release, because the one thing
/// a settings file cannot do later is grow a version field.
struct SettingsFile: Codable {
    let formatVersion: Int
    let settings: [String: SettingsValue]
}

/// JSON tagged by type.
///
/// Untagged JSON cannot carry these settings: `true` and `1` are the same
/// number to `JSONSerialization` on one of the two platforms or the other,
/// `Data` has no JSON spelling at all, and `40` versus `40.0` is exactly the
/// distinction `SettingsBackupSupport.valueLooksRight` drops a key over. So
/// every value is `{"type": …, "value": …}` and the decode is driven by the
/// tag rather than by guessing from the literal.
extension SettingsValue: Codable {
    private enum CodingKeys: String, CodingKey { case type, value }

    private enum Tag: String, Codable {
        case bool, integer, double, string, data, array, dictionary
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        switch try container.decode(Tag.self, forKey: .type) {
        case .bool: self = .bool(try container.decode(Bool.self, forKey: .value))
        case .integer: self = .integer(try container.decode(Int.self, forKey: .value))
        case .double: self = .double(try container.decode(Double.self, forKey: .value))
        case .string: self = .string(try container.decode(String.self, forKey: .value))
        case .data: self = .data(try container.decode(Data.self, forKey: .value))
        case .array: self = .array(try container.decode([SettingsValue].self, forKey: .value))
        case .dictionary:
            self = .dictionary(try container.decode([String: SettingsValue].self, forKey: .value))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .bool(let value):
            try container.encode(Tag.bool, forKey: .type)
            try container.encode(value, forKey: .value)
        case .integer(let value):
            try container.encode(Tag.integer, forKey: .type)
            try container.encode(value, forKey: .value)
        case .double(let value):
            try container.encode(Tag.double, forKey: .type)
            try container.encode(value, forKey: .value)
        case .string(let value):
            try container.encode(Tag.string, forKey: .type)
            try container.encode(value, forKey: .value)
        case .data(let value):
            try container.encode(Tag.data, forKey: .type)
            try container.encode(value, forKey: .value)
        case .array(let values):
            try container.encode(Tag.array, forKey: .type)
            try container.encode(values, forKey: .value)
        case .dictionary(let values):
            try container.encode(Tag.dictionary, forKey: .type)
            try container.encode(values, forKey: .value)
        }
    }
}
