// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation

/// Installs the macOS platform implementations of the core's seams.
///
/// Called on the first line of `Sources/Vorssaint/main.swift` and of the
/// `Tests/MetricsTests.swift` harness, before anything can read a stored
/// wheel, so the app and the tests see exactly the pre-WP-12 behaviour.
enum MacPlatformSeams {
    static func install() {
        ImageDataValidation.current = AppKitImageDataValidator()
        Transliteration.current = CoreFoundationTransliterator()
        MeasurementFormatters.current = FoundationMeasurementFormatter()
    }
}
#endif
