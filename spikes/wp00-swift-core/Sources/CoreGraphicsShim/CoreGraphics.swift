// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike shim. Stands in for Apple's CoreGraphics on Linux.
// swift-corelibs-foundation already provides the geometry family
// (CGFloat/CGPoint/CGSize/CGRect/CGVector/CGAffineTransform), so the module
// re-exports Foundation and adds only the handful of extra CoreGraphics
// symbols the Vorssaint sources touch.

@_exported import Foundation

public typealias CGWindowID = UInt32
public typealias CGDirectDisplayID = UInt32

public struct CGEventFlags: OptionSet, Sendable, Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
    public static let maskShift = CGEventFlags(rawValue: 0x0002_0000)
    public static let maskControl = CGEventFlags(rawValue: 0x0004_0000)
    public static let maskAlternate = CGEventFlags(rawValue: 0x0008_0000)
    public static let maskCommand = CGEventFlags(rawValue: 0x0010_0000)
    public static let maskAlphaShift = CGEventFlags(rawValue: 0x0001_0000)
    public static let maskSecondaryFn = CGEventFlags(rawValue: 0x0080_0000)
    public static let maskHelp = CGEventFlags(rawValue: 0x0040_0000)
    public static let maskNumericPad = CGEventFlags(rawValue: 0x0020_0000)
}

public enum CGEventType: UInt32, Sendable {
    case null = 0
    case leftMouseDown = 1
    case leftMouseUp = 2
    case rightMouseDown = 3
    case rightMouseUp = 4
    case mouseMoved = 5
    case leftMouseDragged = 6
    case rightMouseDragged = 7
    case keyDown = 10
    case keyUp = 11
    case flagsChanged = 12
    case scrollWheel = 22
    case otherMouseDown = 25
    case otherMouseUp = 26
    case otherMouseDragged = 27
}

/// Opaque placeholders. Anything that actually calls into them is, by
/// definition, not core code and must move behind the Platform protocol.
public final class CGEvent {}
public final class CGImage {}
public final class CGContext {}
public final class CGColorSpace {}
public final class CGPath {}
public final class CGMutablePath {}

public enum CGImageAlphaInfo: UInt32, Sendable {
    case none = 0
    case premultipliedLast = 1
    case premultipliedFirst = 2
    case last = 3
    case first = 4
    case noneSkipLast = 5
    case noneSkipFirst = 6
}

public func CGColorSpaceCreateDeviceRGB() -> CGColorSpace { CGColorSpace() }
public func CGDisplayBounds(_ display: CGDirectDisplayID) -> CGRect { .zero }
