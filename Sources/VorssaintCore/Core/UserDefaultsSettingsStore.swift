// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// The macOS settings store: `UserDefaults`, wrapped and nothing else.
///
/// Every method forwards. There is no caching, no coalescing and no
/// validation here on purpose — the macOS product's behaviour is the system's
/// behaviour, and the gate that proves WP-14 changed nothing is the macOS
/// build running the same 31 565 checks it ran before. The only thing this
/// type adds is the change subscription `SettingsStore` promises, which
/// `UserDefaults` already publishes as a notification.
public final class UserDefaultsSettingsStore: SettingsStore {
    public let defaults: UserDefaults

    private var observers: [UUID: (Set<String>) -> Void] = [:]
    private var notificationObserver: NSObjectProtocol?
    private let lock = NSLock()

    public init(_ defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    deinit {
        if let notificationObserver {
            NotificationCenter.default.removeObserver(notificationObserver)
        }
    }

    public func object(forKey key: String) -> Any? { defaults.object(forKey: key) }
    public func bool(forKey key: String) -> Bool { defaults.bool(forKey: key) }
    public func integer(forKey key: String) -> Int { defaults.integer(forKey: key) }
    public func double(forKey key: String) -> Double { defaults.double(forKey: key) }
    public func string(forKey key: String) -> String? { defaults.string(forKey: key) }
    public func data(forKey key: String) -> Data? { defaults.data(forKey: key) }
    public func array(forKey key: String) -> [Any]? { defaults.array(forKey: key) }
    public func stringArray(forKey key: String) -> [String]? { defaults.stringArray(forKey: key) }
    public func dictionary(forKey key: String) -> [String: Any]? { defaults.dictionary(forKey: key) }

    public func set(_ value: Any?, forKey key: String) {
        defaults.set(value, forKey: key)
        announce(key)
    }

    public func set(_ value: Bool, forKey key: String) {
        defaults.set(value, forKey: key)
        announce(key)
    }

    public func set(_ value: Int, forKey key: String) {
        defaults.set(value, forKey: key)
        announce(key)
    }

    public func set(_ value: Double, forKey key: String) {
        defaults.set(value, forKey: key)
        announce(key)
    }

    public func register(defaults registrations: [String: Any]) {
        defaults.register(defaults: registrations)
    }

    public func removeObject(forKey key: String) {
        defaults.removeObject(forKey: key)
        announce(key)
    }

    /// `UserDefaults` writes on its own schedule and the system flushes it;
    /// there is nothing pending this type owns.
    public func flush() {}

    public func observeChanges(_ handler: @escaping (Set<String>) -> Void) -> SettingsObservation {
        let id = UUID()
        lock.lock()
        observers[id] = handler
        let needsNotification = notificationObserver == nil
        lock.unlock()
        if needsNotification { startObservingDefaults() }
        return SettingsObservation { [weak self] in
            guard let self else { return }
            self.lock.lock()
            self.observers[id] = nil
            self.lock.unlock()
        }
    }

    /// A write made anywhere else in the process — or by another process
    /// sharing the domain — arrives without a key, so the handlers are told
    /// "something changed" with an empty set rather than being left behind.
    private func startObservingDefaults() {
        let observer = NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification,
            object: defaults,
            queue: nil
        ) { [weak self] _ in
            self?.announce(nil)
        }
        lock.lock()
        notificationObserver = observer
        lock.unlock()
    }

    private func announce(_ key: String?) {
        lock.lock()
        let handlers = Array(observers.values)
        lock.unlock()
        guard !handlers.isEmpty else { return }
        let keys: Set<String> = key.map { [$0] } ?? []
        for handler in handlers { handler(keys) }
    }
}
