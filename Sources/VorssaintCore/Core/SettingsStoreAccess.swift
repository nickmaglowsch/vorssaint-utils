// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// The one place the app asks for its settings store.
///
/// WP-14 deliberately does *not* rewrite the 615 `UserDefaults.standard`
/// call sites in `Sources/Vorssaint`: a mechanical sweep of that size through
/// the macOS product is exactly the change the macOS gate cannot usefully
/// prove anything about. Instead the protocol and the two implementations
/// land, this accessor names the one the process should use, and the call
/// sites migrate per feature as each one is ported — the migration state is
/// tracked in `docs/linux-port/SETTINGS.md`.
///
/// On macOS it is `UserDefaults.standard`, wrapped and otherwise untouched,
/// so a file that migrates changes which object it talks to and nothing else.
public enum SettingsStoreAccess {
    private static let lock = NSLock()
    private static var override: SettingsStore?

    private static let platformDefault: SettingsStore = {
        #if canImport(Darwin)
        return UserDefaultsSettingsStore(.standard)
        #else
        return JSONSettingsStore()
        #endif
    }()

    /// The process's store.
    public static var shared: SettingsStore {
        lock.lock()
        defer { lock.unlock() }
        return override ?? platformDefault
    }

    /// Points the app at another store. The Linux shell uses it to name a
    /// settings file chosen on the command line, and tests use it to work in
    /// a temporary directory; passing `nil` puts the platform default back.
    public static func use(_ store: SettingsStore?) {
        lock.lock()
        override = store
        lock.unlock()
    }
}
