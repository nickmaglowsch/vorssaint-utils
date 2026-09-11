// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
// WP-00 spike shim for SwiftUI. Only `Color` is referenced from a file that
// is otherwise logic; everything else that needs this module is view code.
@_exported import Foundation
@_exported import CoreGraphics

public struct Color: Hashable, Sendable {
    public var red: Double, green: Double, blue: Double, opacity: Double
    public init(red: Double, green: Double, blue: Double, opacity: Double = 1) {
        self.red = red; self.green = green; self.blue = blue; self.opacity = opacity
    }
    public static let clear = Color(red: 0, green: 0, blue: 0, opacity: 0)
    public static let black = Color(red: 0, green: 0, blue: 0)
    public static let white = Color(red: 1, green: 1, blue: 1)
}
