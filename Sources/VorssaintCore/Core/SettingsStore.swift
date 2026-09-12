// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// One stored preference value, in the six shapes the 644 keys of
/// `DefaultsKeyInventory` actually use.
///
/// It exists because JSON cannot tell a `Bool` from a number and has no
/// `Data`, while `UserDefaults` (and the plist a settings backup is written
/// as) can and does. A settings store that lost that distinction would turn
/// `keepAwakeAutoStart: true` into `1` on the way to Linux and back, and
/// `SettingsBackupSupport.valueLooksRight` would then drop the key on import.
///
/// There is deliberately no `date` case: every timestamp in the key table is
/// stored as a `timeIntervalSince1970` `Double`.
public enum SettingsValue: Equatable {
    case bool(Bool)
    case integer(Int)
    case double(Double)
    case string(String)
    case data(Data)
    case array([SettingsValue])
    case dictionary([String: SettingsValue])

    /// The `Any` a `UserDefaults`-shaped caller expects back.
    public var anyValue: Any {
        switch self {
        case .bool(let value): return value
        case .integer(let value): return value
        case .double(let value): return value
        case .string(let value): return value
        case .data(let value): return value
        case .array(let values): return values.map(\.anyValue)
        case .dictionary(let values): return values.mapValues(\.anyValue)
        }
    }

    /// Classifies a value on its way into a store. `nil` for anything the
    /// key table never holds, which a store then refuses to write rather than
    /// storing in a shape it cannot read back.
    ///
    /// `NSNumber` is tested through `objCType` rather than `CFGetTypeID`:
    /// swift-corelibs-foundation does not vend the CoreFoundation identity
    /// functions (`CORE_MOVES.md` records that as the gap in
    /// `SettingsBackupSupport`), and `value as? Bool` answers `true` for the
    /// number 1 on Darwin, so neither of the obvious tests is portable.
    /// `objCType` is: a boxed boolean is `"c"` on both platforms, a boxed
    /// `Double` is `"d"`, and a boxed integer is neither.
    public static func from(_ value: Any) -> SettingsValue? {
        if let value = value as? SettingsValue { return value }
        if let number = value as? NSNumber {
            switch UInt8(bitPattern: number.objCType.pointee) {
            case UInt8(ascii: "c"), UInt8(ascii: "B"): return .bool(number.boolValue)
            case UInt8(ascii: "f"), UInt8(ascii: "d"): return .double(number.doubleValue)
            default: return .integer(number.intValue)
            }
        }
        // The plain Swift types, for a platform (or a call) where the
        // NSNumber bridge above did not engage.
        if let value = value as? Bool { return .bool(value) }
        if let value = value as? Int { return .integer(value) }
        if let value = value as? Double { return .double(value) }
        if let value = value as? String { return .string(value) }
        if let value = value as? Data { return .data(value) }
        if let values = value as? [Any] {
            let mapped = values.compactMap(SettingsValue.from)
            guard mapped.count == values.count else { return nil }
            return .array(mapped)
        }
        if let values = value as? [String: Any] {
            var mapped: [String: SettingsValue] = [:]
            for (key, element) in values {
                guard let element = SettingsValue.from(element) else { return nil }
                mapped[key] = element
            }
            return .dictionary(mapped)
        }
        return nil
    }

    // The readers below are the store's accessors, in one place, so the two
    // implementations cannot disagree about what "an integer read of a string"
    // means. They follow `UserDefaults`: a number reads as a bool, a numeric
    // string reads as a number, and anything else reads as the zero value.

    public var boolValue: Bool {
        switch self {
        case .bool(let value): return value
        case .integer(let value): return value != 0
        case .double(let value): return value != 0
        case .string(let value): return (value as NSString).boolValue
        case .data, .array, .dictionary: return false
        }
    }

    public var integerValue: Int {
        switch self {
        case .bool(let value): return value ? 1 : 0
        case .integer(let value): return value
        case .double(let value): return SettingsValue.clampedInt(value)
        case .string(let value):
            return Int(value) ?? SettingsValue.clampedInt(Double(value) ?? 0)
        case .data, .array, .dictionary: return 0
        }
    }

    public var doubleValue: Double {
        switch self {
        case .bool(let value): return value ? 1 : 0
        case .integer(let value): return Double(value)
        case .double(let value): return value
        case .string(let value): return Double(value) ?? 0
        case .data, .array, .dictionary: return 0
        }
    }

    /// `Int(Double)` traps on a value outside `Int`'s range and on NaN, and a
    /// settings file is a thing people edit.
    static func clampedInt(_ value: Double) -> Int {
        guard value.isFinite else { return 0 }
        if value >= Double(Int.max) { return Int.max }
        if value <= Double(Int.min) { return Int.min }
        return Int(value)
    }

    /// `UserDefaults.string(forKey:)` also answers for a stored number.
    public var stringValue: String? {
        switch self {
        case .string(let value): return value
        case .bool(let value): return value ? "1" : "0"
        case .integer(let value): return String(value)
        case .double(let value): return String(value)
        case .data, .array, .dictionary: return nil
        }
    }
}

/// A live change subscription. Cancelling is idempotent, and letting the
/// token go cancels it, so a caller cannot leak a handler into a store that
/// outlives it.
public final class SettingsObservation {
    private var cancelAction: (() -> Void)?

    init(cancel: @escaping () -> Void) { cancelAction = cancel }

    public func cancel() {
        cancelAction?()
        cancelAction = nil
    }

    deinit { cancel() }
}

/// Where the app's settings live.
///
/// The surface is `UserDefaults`', method for method, because that is what
/// 615 call sites across `Sources/Vorssaint` already speak (WP-14): the point
/// of the protocol is that those call sites can move to it one file at a time
/// without any of them changing shape. Two implementations:
/// `UserDefaultsSettingsStore` (macOS, a thin wrapper whose behaviour is the
/// system's) and `JSONSettingsStore` (Linux, `$XDG_CONFIG_HOME/vorssaint/
/// settings.json`). `docs/linux-port/SETTINGS.md` is the contract and the
/// file format.
public protocol SettingsStore: AnyObject {
    func object(forKey key: String) -> Any?
    func bool(forKey key: String) -> Bool
    func integer(forKey key: String) -> Int
    func double(forKey key: String) -> Double
    func string(forKey key: String) -> String?
    func data(forKey key: String) -> Data?
    func array(forKey key: String) -> [Any]?
    func stringArray(forKey key: String) -> [String]?
    func dictionary(forKey key: String) -> [String: Any]?

    func set(_ value: Any?, forKey key: String)
    func set(_ value: Bool, forKey key: String)
    func set(_ value: Int, forKey key: String)
    func set(_ value: Double, forKey key: String)

    /// The fallbacks a key falls back to while nothing has been written to
    /// it. Registering the same key twice is the last registration winning,
    /// exactly as `UserDefaults.register(defaults:)` behaves.
    func register(defaults: [String: Any])

    /// Back to the registered default, or to absent where there is none.
    func removeObject(forKey key: String)

    /// Makes every pending write durable. `JSONSettingsStore` debounces, so
    /// this is what a quit path, a settings export and a test call.
    func flush()

    /// Fires with the keys that changed. Every write through this store
    /// reports, and on macOS so does a change another process made.
    func observeChanges(_ handler: @escaping (Set<String>) -> Void) -> SettingsObservation
}

public extension SettingsStore {
    /// The accessor `SettingsBackupSupport.payload(appVersion:valueFor:)`
    /// wants: the value including the registered default, so an exported file
    /// reproduces a setup the person never touched a control for.
    func snapshot(of keys: [String]) -> [String: Any] {
        var out: [String: Any] = [:]
        for key in keys {
            if let value = object(forKey: key) { out[key] = value }
        }
        return out
    }
}
