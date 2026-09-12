// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Romanizes text so a Latin-keyboard query can find a title that is not
/// written in Latin script.
///
/// The one core caller is `CommandBarSearch.pinyinKeywords`, which used to run
/// `CFStringTransform(_:_:kCFStringTransformMandarinLatin,_:)` followed by
/// `kCFStringTransformStripDiacritics` inline (`docs/linux-port/CORE_MOVES.md`
/// § 4.3). Neither symbol exists on Linux; `String.applyingTransform` does, and
/// carries the same ICU transliterator underneath, so the Linux default asks
/// for the same two transforms by name.
///
/// `capabilities` is what makes the degradation honest: an implementation that
/// cannot romanize says so, and the command bar simply indexes no pinyin
/// keywords instead of silently indexing wrong ones.
public protocol Transliterator {
    /// Mandarin to Latin, or `nil` when this platform cannot do it.
    func mandarinLatin(_ text: String) -> String?

    /// Strips combining marks (the tone marks pinyin comes back with).
    func strippingDiacritics(_ text: String) -> String

    var capabilities: TransliteratorCapabilities { get }
}

/// What a `Transliterator` can actually do on this session.
public struct TransliteratorCapabilities: Equatable {
    /// `false` on a build whose ICU has no Han-to-Latin transliterator, which
    /// makes `mandarinLatin` return `nil` rather than the untransformed input.
    public let canRomanizeMandarin: Bool
    /// `false` would mean tone marks survive into the search index.
    public let canStripDiacritics: Bool

    public init(canRomanizeMandarin: Bool, canStripDiacritics: Bool) {
        self.canRomanizeMandarin = canRomanizeMandarin
        self.canStripDiacritics = canStripDiacritics
    }
}

/// The portable implementation, over `String.applyingTransform`.
///
/// `StringTransform.mandarinToLatin` is `kCFStringTransformMandarinLatin`
/// spelled in Swift; `swift-corelibs-foundation` implements
/// `applyingTransform` on top of the same ICU transliterator, and
/// `CommandBarEmoji.swift` already proves `applyingTransform` compiles and
/// runs on Linux Swift 6.1.3 (`docs/linux-port/CORE_MOVES.md` § 6.3).
///
/// `mandarinLatin` returns `nil` when the transform is unavailable *or* leaves
/// the text unchanged, which is exactly the `guard` the old inline code wrote:
/// `CFStringTransform` returning false, and "the romanization equals the
/// title", were both "no keywords".
public struct FoundationTransliterator: Transliterator {
    public init() {}

    public func mandarinLatin(_ text: String) -> String? {
        text.applyingTransform(.mandarinToLatin, reverse: false)
    }

    public func strippingDiacritics(_ text: String) -> String {
        text.applyingTransform(.stripDiacritics, reverse: false) ?? text
    }

    public var capabilities: TransliteratorCapabilities {
        // Probed, not assumed: a build whose ICU data was stripped returns nil
        // here and the command bar then indexes no pinyin at all.
        TransliteratorCapabilities(
            canRomanizeMandarin: "中文".applyingTransform(.mandarinToLatin, reverse: false) != nil,
            canStripDiacritics: "é".applyingTransform(.stripDiacritics, reverse: false) != nil)
    }
}

/// Romanizes nothing, and says so.
public struct NoTransliterator: Transliterator {
    public init() {}

    public func mandarinLatin(_ text: String) -> String? { nil }

    public func strippingDiacritics(_ text: String) -> String { text }

    public var capabilities: TransliteratorCapabilities {
        TransliteratorCapabilities(canRomanizeMandarin: false, canStripDiacritics: false)
    }
}

/// The process-wide transliterator. The macOS adapter replaces it with the
/// `CFStringTransform` pair the app shipped, so nothing about the Mac search
/// index changes; every other platform keeps the Foundation default.
public enum Transliteration {
    public static var current: Transliterator = FoundationTransliterator()
}
