// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import CoreFoundation
import Foundation

/// The macOS `Transliterator`: the exact `CFStringTransform` pair
/// `CommandBarSearch.pinyinKeywords` ran inline before WP-12.
///
/// `String.applyingTransform` is the same call underneath on Darwin, but the
/// Mac search index is a behaviour the port must not perturb, so the adapter
/// keeps the original spelling rather than an equivalent one.
struct CoreFoundationTransliterator: Transliterator {
    func mandarinLatin(_ text: String) -> String? {
        let romanized = NSMutableString(string: text)
        guard CFStringTransform(romanized, nil, kCFStringTransformMandarinLatin, false)
        else { return nil }
        return romanized as String
    }

    func strippingDiacritics(_ text: String) -> String {
        let stripped = NSMutableString(string: text)
        // The original ignored this return value too: a transform that cannot
        // run leaves the string as it found it, which is the wanted fallback.
        CFStringTransform(stripped, nil, kCFStringTransformStripDiacritics, false)
        return stripped as String
    }

    var capabilities: TransliteratorCapabilities {
        TransliteratorCapabilities(canRomanizeMandarin: true, canStripDiacritics: true)
    }
}
#endif
