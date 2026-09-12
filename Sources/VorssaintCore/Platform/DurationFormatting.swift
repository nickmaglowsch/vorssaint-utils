// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Writes a span of whole days the way the reader's language writes it:
/// "150 days", "150 dias", "150 Tage".
///
/// A seam for the same reason as `MeasurementFormatting`:
/// swift-corelibs-foundation marks `DateComponentsFormatter`
/// `@available(*, unavailable, message: "Not supported in swift-corelibs-
/// foundation")`. Found by the Linux compiler on run 34693363900, not by the
/// WP-00 census, which counted imports rather than APIs.
///
/// `CommandBarDates.daysUntil` is the only caller: the command bar's "days
/// until 25/12" answer.
public protocol DurationFormatting {
    /// A count of whole days, spelled out. `nil` when it cannot be written.
    func string(days: Int, locale: Locale, calendar: Calendar) -> String?

    var capabilities: DurationFormattingCapabilities { get }
}

public struct DurationFormattingCapabilities: Equatable {
    /// `false` means the word "days" is not translated — the *number* still
    /// is. The command bar's answer is then readable but not in the reader's
    /// language, which the Capabilities page reports rather than hiding.
    public let localizesUnitNames: Bool

    public init(localizesUnitNames: Bool) {
        self.localizesUnitNames = localizesUnitNames
    }
}

/// The portable implementation: a localized number and an untranslated unit.
///
/// There is no localized day-count formatter in swift-corelibs-foundation, so
/// this degrades in the open rather than pretending: `NumberFormatter` gets
/// the number right in every region (which is the part a decimal-comma reader
/// notices), and the word is English. The Linux shell can install a better one
/// from the toolkit's own ICU once WP-20 has one; `capabilities` is how a
/// caller finds out which it got.
public struct PlainDurationFormatter: DurationFormatting {
    public init() {}

    public func string(days: Int, locale: Locale, calendar: Calendar) -> String? {
        let numbers = NumberFormatter()
        numbers.locale = locale
        numbers.numberStyle = .decimal
        numbers.maximumFractionDigits = 0
        let value = numbers.string(from: NSNumber(value: days)) ?? String(days)
        return days == 1 ? "\(value) day" : "\(value) days"
    }

    public var capabilities: DurationFormattingCapabilities {
        DurationFormattingCapabilities(localizesUnitNames: false)
    }
}

/// The process-wide formatter. macOS installs the `DateComponentsFormatter`
/// one, so the command bar's answer reads exactly as it did before WP-12.
public enum DurationFormatters {
    public static var current: DurationFormatting = PlainDurationFormatter()
}
