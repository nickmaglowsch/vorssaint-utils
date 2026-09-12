// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import CryptoKit
import Foundation
import Security

// The Keychain/CryptoKit half of CommandBarSupport.swift, split out by WP-12
// so the search layer could move into Sources/VorssaintCore.
//
// `CommandBarQueryHabits` keys every stored query digest with an HMAC whose
// per-install key lives in the login Keychain; `Security` and `CryptoKit`
// exist on neither Linux nor the core. The Linux backend stores the same
// digests under libsecret through the `TrashAndFiles`/secret-store side of
// the platform layer (docs/linux-port/PLATFORM.md).
//
// The text below is verbatim from the original file.

/// Which durable result won after a typed query. Preferences hold only keyed
/// digests of query prefixes; the per-install key lives separately in the
/// Keychain.
enum CommandBarQueryHabits {
    typealias Store = [String: [String: CommandBarUse]]

    struct PreparedQuery {
        fileprivate let keys: [(digest: String, specificity: Int)]
        var keyCount: Int { keys.count }
        var isEmpty: Bool { keys.isEmpty }
    }

    /// The active field's keyed prefixes. Typing one more character should
    /// hash that character's prefix, not every prefix the field already had.
    struct PreparationCache {
        fileprivate var normalizedQuery = ""
        fileprivate var key: Data?
        fileprivate var prepared = PreparedQuery(keys: [])

        mutating func reset() {
            normalizedQuery = ""
            key = nil
            prepared = PreparedQuery(keys: [])
        }
    }

    static let storedQueryLimit = 320
    private static let resultLimit = 4

    static func decode(_ raw: String?) -> Store {
        guard let raw, let data = raw.data(using: .utf8),
              let store = try? JSONDecoder().decode(Store.self, from: data)
        else { return [:] }
        return store
    }

    static func encode(_ store: Store) -> String? {
        guard let data = try? JSONEncoder().encode(store) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func recording(_ store: Store,
                          preparedQuery: PreparedQuery,
                          resultID: String,
                          now: Double) -> Store {
        var next = store
        for queryKey in preparedQuery.keys {
            var choices = next[queryKey.digest] ?? [:]
            var use = choices[resultID] ?? CommandBarUse(count: 0, lastUsed: now)
            use.count = min(use.count + 1, 999)
            use.lastUsed = now
            choices[resultID] = use
            if choices.count > resultLimit {
                for choice in choices.sorted(by: { $0.value.lastUsed < $1.value.lastUsed })
                    .prefix(choices.count - resultLimit) {
                    choices.removeValue(forKey: choice.key)
                }
            }
            next[queryKey.digest] = choices
        }
        if next.count > storedQueryLimit {
            let surplus = next.sorted { latestUse(in: $0.value) < latestUse(in: $1.value) }
                .prefix(next.count - storedQueryLimit)
            for entry in surplus { next.removeValue(forKey: entry.key) }
        }
        return next
    }

    static func boost(for resultID: String,
                      preparedQuery: PreparedQuery,
                      store: Store,
                      now: Double) -> Int {
        preparedQuery.keys.reduce(0) { best, queryKey in
            guard let use = store[queryKey.digest]?[resultID] else { return best }
            let learned = CommandBarUsage.boost(for: use, now: now) * 5
                + queryKey.specificity * 6
            return max(best, min(learned, 720))
        }
    }

    static func removing(resultID: String, from store: Store) -> Store {
        store.reduce(into: Store()) { result, entry in
            var choices = entry.value
            choices.removeValue(forKey: resultID)
            if !choices.isEmpty { result[entry.key] = choices }
        }
    }

    static func prepare(_ query: String) -> PreparedQuery {
        var cache = PreparationCache()
        return prepare(query, cache: &cache)
    }

    static func prepare(_ query: String,
                        cache: inout PreparationCache) -> PreparedQuery {
        prepare(query, key: installationKeyCache.cachedKey, cache: &cache)
    }

    /// Injectable so the storage and ranking rules stay deterministic in
    /// tests without reading or writing the person's Keychain.
    static func prepare(_ query: String, key: Data) -> PreparedQuery {
        var cache = PreparationCache()
        return prepare(query, key: key, cache: &cache)
    }

    /// Injectable digest work gives the tests a stable operation count instead
    /// of asking shared CI hardware to meet a wall-clock threshold.
    static func prepare(_ query: String,
                        key: Data?,
                        cache: inout PreparationCache,
                        digest: (String, Data) -> String = digest) -> PreparedQuery {
        let normalized = String(CommandBarSearch.normalized(query).prefix(24))
        guard let key, key.count == 32 else {
            cache.normalizedQuery = normalized
            cache.key = nil
            cache.prepared = PreparedQuery(keys: [])
            return cache.prepared
        }

        let characters = Array(normalized)
        guard cache.key == key else {
            cache.normalizedQuery = normalized
            cache.key = key
            cache.prepared = PreparedQuery(keys: preparedKeys(characters, key: key, digest: digest))
            return cache.prepared
        }
        if normalized == cache.normalizedQuery { return cache.prepared }

        let old = cache.normalizedQuery
        if normalized.hasPrefix(old) {
            var keys = cache.prepared.keys
            let firstNewLength = Array(old).count + 1
            if firstNewLength <= characters.count {
                for length in firstNewLength...characters.count {
                    let prefix = String(characters.prefix(length))
                    keys.append((digest(prefix, key), length))
                }
            }
            cache.prepared = PreparedQuery(keys: keys)
        } else if old.hasPrefix(normalized) {
            cache.prepared = PreparedQuery(
                keys: cache.prepared.keys.filter { $0.specificity <= characters.count })
        } else {
            cache.prepared = PreparedQuery(keys: preparedKeys(characters, key: key, digest: digest))
        }
        cache.normalizedQuery = normalized
        return cache.prepared
    }

    private static func preparedKeys(_ characters: [Character],
                                     key: Data,
                                     digest: (String, Data) -> String) -> [(String, Int)] {
        guard !characters.isEmpty else { return [] }
        return (1...characters.count).map { length in
            (digest(String(characters.prefix(length)), key), length)
        }
    }

    private static func digest(_ prefix: String, key: Data) -> String {
        HMAC<SHA256>.authenticationCode(
            for: Data(prefix.utf8), using: SymmetricKey(data: key))
            .prefix(12)
            .map { String(format: "%02x", $0) }.joined()
    }

    private static func latestUse(in choices: [String: CommandBarUse]) -> Double {
        choices.values.map(\.lastUsed).max() ?? 0
    }

    /// Starts the only Keychain work used by query learning. Search and
    /// selection read the memory cache without waiting for this queue.
    static func warmInstallationKey(_ whenReady: (() -> Void)? = nil) {
        installationKeyCache.warm(whenReady)
    }

    static func removeInstallationKey() {
        installationKeyCache.stopAndRemove {
            _ = SecItemDelete([
                kSecClass: kSecClassGenericPassword,
                kSecAttrService: keyService,
                kSecAttrAccount: keyAccount,
            ] as CFDictionary)
        }.wait()
    }

    // Developer and official installations must never share or delete each
    // other's key; their learned choices already live in separate preferences.
    static func installationKeyService(bundleID: String) -> String {
        bundleID + ".command-bar-query-habits"
    }

    private static let keyService = installationKeyService(
        bundleID: Bundle.main.bundleIdentifier ?? "com.vorssaint.utils")
    private static let keyAccount = "hmac-key"

    private static let installationKeyCache = CommandBarQueryHabitKeyCache {
        loadInstallationKey(using: liveKeyStore)
    }

    static func loadInstallationKey(using store: CommandBarQueryHabitKeyStore) -> Data? {
        switch store.read() {
        case (errSecSuccess, let data) where data?.count == 32:
            return data
        case (errSecSuccess, _):
            return repairInstallationKey(using: store)
        case (errSecItemNotFound, _):
            return createInstallationKey(using: store)
        default:
            return nil
        }
    }

    private static func createInstallationKey(
        using store: CommandBarQueryHabitKeyStore
    ) -> Data? {
        guard let generated = store.randomKey(), generated.count == 32 else { return nil }
        switch store.add(generated) {
        case errSecSuccess:
            return persistedKey(using: store)
        case errSecDuplicateItem:
            let raced = store.read()
            if raced.0 == errSecSuccess, raced.1?.count == 32 { return raced.1 }
            guard raced.0 == errSecSuccess else { return nil }
            return repairInstallationKey(using: store, replacement: generated)
        default:
            return nil
        }
    }

    private static func repairInstallationKey(
        using store: CommandBarQueryHabitKeyStore,
        replacement: Data? = nil
    ) -> Data? {
        guard let generated = replacement ?? store.randomKey(), generated.count == 32,
              store.update(generated) == errSecSuccess
        else { return nil }
        return persistedKey(using: store)
    }

    private static func persistedKey(using store: CommandBarQueryHabitKeyStore) -> Data? {
        let stored = store.read()
        guard stored.0 == errSecSuccess, stored.1?.count == 32 else { return nil }
        return stored.1
    }

    private static let liveKeyStore = CommandBarQueryHabitKeyStore(
        read: {
            let lookup: [CFString: Any] = [
                kSecClass: kSecClassGenericPassword,
                kSecAttrService: keyService,
                kSecAttrAccount: keyAccount,
                kSecReturnData: true,
                kSecMatchLimit: kSecMatchLimitOne,
            ]
            var item: CFTypeRef?
            let status = SecItemCopyMatching(lookup as CFDictionary, &item)
            return (status, item as? Data)
        },
        randomKey: {
            var bytes = [UInt8](repeating: 0, count: 32)
            guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess
            else { return nil }
            return Data(bytes)
        },
        add: { data in
            SecItemAdd([
                kSecClass: kSecClassGenericPassword,
                kSecAttrService: keyService,
                kSecAttrAccount: keyAccount,
                kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
                kSecValueData: data,
            ] as CFDictionary, nil)
        },
        update: { data in
            let identity: [CFString: Any] = [
                kSecClass: kSecClassGenericPassword,
                kSecAttrService: keyService,
                kSecAttrAccount: keyAccount,
            ]
            return SecItemUpdate(identity as CFDictionary,
                                 [kSecValueData: data] as CFDictionary)
        })
}

/// One decoded copy of learned result choices. The service reloads this when a
/// presentation starts and ranking only reads the already-decoded dictionary.
struct CommandBarQueryHabitStoreCache {
    private(set) var store: CommandBarQueryHabits.Store = [:]

    mutating func reload(_ raw: String?,
                         decode: (String?) -> CommandBarQueryHabits.Store =
                            CommandBarQueryHabits.decode) {
        store = decode(raw)
    }

    mutating func record(preparedQuery: CommandBarQueryHabits.PreparedQuery,
                         resultID: String,
                         now: Double) {
        store = CommandBarQueryHabits.recording(
            store, preparedQuery: preparedQuery, resultID: resultID, now: now)
    }

    mutating func remove(resultID: String) {
        store = CommandBarQueryHabits.removing(resultID: resultID, from: store)
    }

    mutating func forgetAll() {
        store = [:]
    }
}

struct CommandBarQueryHabitKeyStore {
    let read: () -> (OSStatus, Data?)
    let randomKey: () -> Data?
    let add: (Data) -> OSStatus
    let update: (Data) -> OSStatus
}

/// A failed load returns to idle so a later panel opening can retry. Loading
/// never blocks a caller: only a valid persisted key enters the ready state.
final class CommandBarQueryHabitKeyCache {
    private enum State {
        case idle
        case loading
        case ready(Data)
        case stopped
    }

    private let lock = NSLock()
    private let queue: DispatchQueue
    private let load: () -> Data?
    private var state = State.idle
    private var readinessCallbacks: [() -> Void] = []

    init(queue: DispatchQueue = DispatchQueue(
            label: "org.vorssaint.command-bar-query-habit-key",
            qos: .utility),
         load: @escaping () -> Data?) {
        self.queue = queue
        self.load = load
    }

    var cachedKey: Data? {
        lock.lock()
        defer { lock.unlock() }
        guard case .ready(let key) = state else { return nil }
        return key
    }

    func warm(_ whenReady: (() -> Void)? = nil) {
        lock.lock()
        if case .stopped = state {
            lock.unlock()
            return
        }
        if case .ready = state {
            lock.unlock()
            whenReady?()
            return
        }
        if let whenReady { readinessCallbacks.append(whenReady) }
        guard case .idle = state else {
            lock.unlock()
            return
        }
        state = .loading

        queue.async { [self] in
            let key = load()
            lock.lock()
            guard case .loading = state else {
                lock.unlock()
                return
            }
            let callbacks: [() -> Void]
            if let key, key.count == 32 {
                state = .ready(key)
                callbacks = readinessCallbacks
            } else {
                state = .idle
                callbacks = []
            }
            readinessCallbacks = []
            lock.unlock()
            callbacks.forEach { $0() }
        }
        lock.unlock()
    }

    /// Stop before enqueueing deletion, so an in-flight load cannot restore
    /// the key or notify callers after uninstall has removed it.
    func stopAndRemove(_ remove: @escaping () -> Void) -> DispatchWorkItem {
        lock.lock()
        state = .stopped
        readinessCallbacks = []
        let removal = DispatchWorkItem(block: remove)
        queue.async(execute: removal)
        lock.unlock()
        return removal
    }
}
