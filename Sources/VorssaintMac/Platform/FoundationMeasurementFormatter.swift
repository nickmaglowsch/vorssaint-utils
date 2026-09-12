// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation

/// The macOS `MeasurementFormatting`: the `MeasurementFormatter` that
/// `CommandBarUnits.format` built inline before WP-12, with the same three
/// settings in the same order.
struct FoundationMeasurementFormatter: MeasurementFormatting {
    func string(from measurement: Measurement<Dimension>,
                locale: Locale,
                maximumFractionDigits: Int) -> String {
        let formatter = MeasurementFormatter()
        formatter.locale = locale
        formatter.unitOptions = .providedUnit
        formatter.unitStyle = .medium
        formatter.numberFormatter.locale = locale
        formatter.numberFormatter.maximumFractionDigits = maximumFractionDigits
        formatter.numberFormatter.minimumFractionDigits = 0
        return formatter.string(from: measurement)
    }

    var capabilities: MeasurementFormattingCapabilities {
        MeasurementFormattingCapabilities(localizesUnitNames: true)
    }
}
#endif
