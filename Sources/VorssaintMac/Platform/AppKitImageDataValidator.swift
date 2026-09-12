// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import Foundation

/// The macOS `ImageDataValidator`: the `NSImage(data:)` probe that
/// `RadialMenuSupport.sanitized` used to make inline before WP-12 moved that
/// file into the core.
///
/// The expression is copied, not reinterpreted: `NSImage(data:) == nil` meant
/// "not a valid image", so this returns the negation and nothing else.
struct AppKitImageDataValidator: ImageDataValidator {
    func isValidImageData(_ data: Data) -> Bool {
        NSImage(data: data) != nil
    }
}
#endif
