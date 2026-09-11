// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike shim for AppKit. Deliberately thin: the point of the spike is
// to find out how far the AppKit surface reaches into files that are
// otherwise pure logic, so this covers only the members the census found and
// nothing else. Every symbol here is a Platform-protocol candidate.

@_exported import Foundation
@_exported import CoreGraphics

public struct NSEventModifierFlagsStorage: OptionSet, Sendable, Hashable {
    public let rawValue: UInt
    public init(rawValue: UInt) { self.rawValue = rawValue }
}

open class NSEvent {
    public enum EventType: UInt, Sendable {
        case leftMouseDown = 1, leftMouseUp = 2, rightMouseDown = 3, rightMouseUp = 4
        case mouseMoved = 5, keyDown = 10, keyUp = 11, flagsChanged = 12
        case scrollWheel = 22, otherMouseDown = 25, otherMouseUp = 26
    }
    public struct ModifierFlags: OptionSet, Sendable, Hashable {
        public let rawValue: UInt
        public init(rawValue: UInt) { self.rawValue = rawValue }
        public static let capsLock = ModifierFlags(rawValue: 1 << 16)
        public static let shift = ModifierFlags(rawValue: 1 << 17)
        public static let control = ModifierFlags(rawValue: 1 << 18)
        public static let option = ModifierFlags(rawValue: 1 << 19)
        public static let command = ModifierFlags(rawValue: 1 << 20)
        public static let numericPad = ModifierFlags(rawValue: 1 << 21)
        public static let help = ModifierFlags(rawValue: 1 << 22)
        public static let function = ModifierFlags(rawValue: 1 << 23)
        public static let deviceIndependentFlagsMask = ModifierFlags(rawValue: 0xffff_0000)
    }
    public var scrollingDeltaY: CGFloat { 0 }
    public init() {}
}

open class NSFont {
    public struct Weight: Hashable, Sendable {
        public let rawValue: CGFloat
        public init(_ rawValue: CGFloat) { self.rawValue = rawValue }
        public static let regular = Weight(0)
        public static let medium = Weight(0.23)
        public static let semibold = Weight(0.3)
        public static let bold = Weight(0.4)
    }
    public static func systemFont(ofSize size: CGFloat, weight: Weight = .regular) -> NSFont {
        NSFont()
    }
    public init() {}
}

open class NSColor {
    public init() {}
    public static let black = NSColor()
    public static let white = NSColor()
    public static let clear = NSColor()
}

open class NSImage { public init() {} }
open class NSBitmapImageRep { public init() {} }
open class NSGraphicsContext {
    public static var current: NSGraphicsContext? { nil }
    public init() {}
}
open class NSPasteboard {
    public var types: [String]? { nil }
    public init() {}
}
open class NSScreen { public init() {} }

public final class NSApplicationStub {
    public func terminate(_ sender: Any?) {}
}
public let NSApp = NSApplicationStub()
