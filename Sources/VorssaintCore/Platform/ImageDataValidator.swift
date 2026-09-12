// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Answers "can this blob be decoded as an image?" without the core knowing
/// what decodes it.
///
/// One question, one method, because there is exactly one core caller:
/// `RadialMenuSupport.sanitized` used to write `NSImage(data: customData) ==
/// nil` to drop a stored custom icon that no longer decodes
/// (`docs/linux-port/CORE_MOVES.md` § 4.3). That single AppKit call was what
/// kept `RadialMenuSupport` — and through it `FeatureCatalog`,
/// `FeaturePresets`, `SettingsBackupSupport` and the mouse-exception layer —
/// out of the platform-free core.
///
/// The sanitiser calls this *after* the byte-count check, exactly as before,
/// so an oversized blob is still rejected without decoding it.
public protocol ImageDataValidator {
    /// `true` when `data` decodes to an image the platform can draw.
    func isValidImageData(_ data: Data) -> Bool
}

/// An `ImageDataValidator` built from a closure, for callers that have the
/// check but not a type to hang it on (tests, and the Linux shell before its
/// decoder is wired).
public struct ClosureImageDataValidator: ImageDataValidator {
    private let check: (Data) -> Bool

    public init(_ check: @escaping (Data) -> Bool) {
        self.check = check
    }

    public func isValidImageData(_ data: Data) -> Bool { check(data) }
}

/// Accepts any non-empty blob.
///
/// This is the *default*, not the macOS behaviour: a platform that has not
/// installed a decoder must not silently delete the user's stored icons, so
/// the fallback keeps them. Every real build installs a validator before
/// anything can sanitise a wheel — `Sources/Vorssaint/main.swift` and the
/// `Tests/MetricsTests.swift` harness do it on their first line, and the
/// macOS test that asserts a corrupted icon is dropped is what proves it.
public struct PermissiveImageDataValidator: ImageDataValidator {
    public init() {}

    public func isValidImageData(_ data: Data) -> Bool { !data.isEmpty }
}

/// The process-wide validator. A single mutable static in the same style as
/// `MetricFormat.locale`, which the test harness already pins the same way.
public enum ImageDataValidation {
    /// Installed by the platform layer at start-up. Replaceable in tests.
    public static var current: ImageDataValidator = PermissiveImageDataValidator()
}
