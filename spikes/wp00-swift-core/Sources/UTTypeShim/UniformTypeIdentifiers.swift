// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
// WP-00 spike shim for UniformTypeIdentifiers. Linux equivalent is
// shared-mime-info / xdg-mime lookups behind a Platform protocol.
@_exported import Foundation

public struct UTType: Hashable, Sendable {
    public let identifier: String
    public init?(_ identifier: String) { self.identifier = identifier }
    public init(rawValue: String) { self.identifier = rawValue }
    public func conforms(to other: UTType) -> Bool { false }
    public static let image = UTType(rawValue: "public.image")
    public static let movie = UTType(rawValue: "public.movie")
    public static let png = UTType(rawValue: "public.png")
    public static let jpeg = UTType(rawValue: "public.jpeg")
}
