// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// The one JSON spelling the bridge uses, in both directions.
///
/// **Stable key order.** `.sortedKeys` is not cosmetic here: the diff in
/// `CoreBridge` is a byte comparison of two encodings, so an encoder that is
/// free to order a dictionary's keys differently between two calls would
/// report every snapshot as changed and defeat the whole fast path. It also
/// makes a snapshot diffable by eye and by `diff`, which is how the CI ABI
/// client's output is checked.
///
/// **`.withoutEscapingSlashes`.** Paths appear in snapshots (the cleaner, the
/// shelf, managed downloads); `\/` is legal JSON that no reader wants to see.
///
/// A fresh coder per call rather than a shared one: `JSONEncoder` is a class
/// with mutable configuration, and a shared instance would be a data race the
/// moment a service publishes from a sampling thread — exactly what the
/// threading contract allows.
public enum BridgeJSON {
    public static func makeEncoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return encoder
    }

    public static func makeDecoder() -> JSONDecoder {
        JSONDecoder()
    }

    /// Encode a snapshot the way the bridge delivers it.
    public static func encode<T: Encodable>(_ value: T) throws -> Data {
        try makeEncoder().encode(value)
    }

    /// Decode a command the way the bridge receives it.
    public static func decode<T: Decodable>(_ type: T.Type, from data: Data) throws -> T {
        try makeDecoder().decode(type, from: data)
    }
}

/// The key of a command envelope: any string, because the key *is* the case
/// name and the set of case names is the service's business, not this file's.
public struct BridgeCommandKey: CodingKey {
    public let stringValue: String
    public var intValue: Int? { nil }

    public init?(stringValue: String) { self.stringValue = stringValue }
    public init?(intValue: Int) { return nil }
    public init(_ name: String) { stringValue = name }
}

/// Helpers for the single-key command shape every bridge command enum uses:
///
/// ```
/// {"install":"switcher"}          one associated value, a scalar
/// {"setLanguage":"pt-BR"}
/// {"moveResize":{"id":7,"x":0}}   several, an object
/// {"uninstallAll":true}           none — `true` so the object is never empty
/// ```
///
/// Swift's synthesised enum `Codable` would spell the first of those
/// `{"install":{"_0":"switcher"}}`. `_0` is a compiler detail; it would appear
/// in every QML file in the shell and in every line of `BRIDGE.md`, so each
/// command enum writes the fifteen lines below instead. The WP-01 stub already
/// speaks this shape (`{"recordShortcut":"Ctrl+Alt+K"}`), which is the other
/// reason it is the convention rather than the synthesis.
public enum BridgeCommandCoding {
    /// Read the single key of a command object, or throw the error the bridge
    /// turns into `-3`.
    public static func singleKey(
        _ container: KeyedDecodingContainer<BridgeCommandKey>,
        in decoder: Decoder
    ) throws -> BridgeCommandKey {
        guard container.allKeys.count == 1, let key = container.allKeys.first else {
            throw DecodingError.dataCorrupted(
                DecodingError.Context(
                    codingPath: decoder.codingPath,
                    debugDescription: "a bridge command is one object with exactly one key, "
                        + "got \(container.allKeys.count)"))
        }
        return key
    }

    /// The error a command enum throws for a key it does not have a case for.
    public static func unknownCase(
        _ key: BridgeCommandKey, _ decoder: Decoder
    ) -> DecodingError {
        DecodingError.dataCorrupted(
            DecodingError.Context(codingPath: decoder.codingPath,
                                  debugDescription: "unknown command \"\(key.stringValue)\""))
    }
}
