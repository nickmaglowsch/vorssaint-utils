// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation

/// The macOS `DurationFormatting`: the `DateComponentsFormatter`
/// `CommandBarDates.daysUntil` built inline before WP-12, with the same
/// calendar, the same `allowedUnits` and the same `unitsStyle`.
struct FoundationDurationFormatter: DurationFormatting {
    func string(days: Int, locale: Locale, calendar: Calendar) -> String? {
        let formatter = DateComponentsFormatter()
        formatter.calendar = {
            var calendar = calendar
            calendar.locale = locale
            return calendar
        }()
        formatter.allowedUnits = [.day]
        formatter.unitsStyle = .full
        return formatter.string(from: DateComponents(day: days))
    }

    var capabilities: DurationFormattingCapabilities {
        DurationFormattingCapabilities(localizesUnitNames: true)
    }
}
#endif
