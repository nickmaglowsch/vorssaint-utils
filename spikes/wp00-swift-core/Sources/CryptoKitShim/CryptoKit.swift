// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike shim for CryptoKit. On Linux the real answer is
// apple/swift-crypto, which exposes the identical API under `import Crypto`;
// this stub only proves the call sites are API-compatible.

@_exported import Foundation

public struct SHA256Digest: Sequence, Hashable, Sendable {
    public let bytes: [UInt8]
    public func makeIterator() -> Array<UInt8>.Iterator { bytes.makeIterator() }
}

public enum SHA256 {
    public static let byteCount = 32
    public static func hash(data: some DataProtocol) -> SHA256Digest {
        SHA256Digest(bytes: Array(repeating: 0, count: byteCount))
    }
}
