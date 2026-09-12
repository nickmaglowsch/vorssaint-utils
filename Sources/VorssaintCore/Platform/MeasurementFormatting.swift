// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Writes a measurement the way the reader's language writes it.
///
/// A seam rather than a call, because `MeasurementFormatter` is one of the few
/// Foundation classes swift-corelibs-foundation marks
/// `@available(*, unavailable, message: "Not supported in swift-corelibs-foundation")` —
/// found by the Linux compiler on run 34665056778, not by the WP-00 census,
/// which counted imports rather than APIs.
///
/// `CommandBarUnits.format` is the only caller: the command bar's unit
/// conversions ("12 ft to m").
public protocol MeasurementFormatting {
    /// The measurement in the unit it is already in — never rescaled, because
    /// "to mb" must not answer in gigabytes — with at most
    /// `maximumFractionDigits` and no forced minimum.
    func string(from measurement: Measurement<Dimension>,
                locale: Locale,
                maximumFractionDigits: Int) -> String

    var capabilities: MeasurementFormattingCapabilities { get }
}

public struct MeasurementFormattingCapabilities: Equatable {
    /// `false` means unit *names* come out as their symbols: "12.5 m" rather
    /// than a localized "12,5 Meter". The number itself is still localized.
    public let localizesUnitNames: Bool

    public init(localizesUnitNames: Bool) {
        self.localizesUnitNames = localizesUnitNames
    }
}

/// The portable implementation: a localized number plus the unit's symbol.
///
/// `NumberFormatter` is present and correct on Linux, so the number — which is
/// the part a decimal-comma region actually notices — is right everywhere; only
/// the unit's spelled-out name is lost, and `capabilities` says so. The symbol
/// is what `UnitLength.feet.symbol` and friends already carry ("ft", "m",
/// "MB"), which is also what the person typed.
public struct SymbolMeasurementFormatter: MeasurementFormatting {
    public init() {}

    public func string(from measurement: Measurement<Dimension>,
                       locale: Locale,
                       maximumFractionDigits: Int) -> String {
        let numbers = NumberFormatter()
        numbers.locale = locale
        numbers.numberStyle = .decimal
        numbers.maximumFractionDigits = maximumFractionDigits
        numbers.minimumFractionDigits = 0
        let value = numbers.string(from: NSNumber(value: measurement.value))
            ?? String(measurement.value)
        return "\(value) \(measurement.unit.symbol)"
    }

    public var capabilities: MeasurementFormattingCapabilities {
        MeasurementFormattingCapabilities(localizesUnitNames: false)
    }
}

/// The process-wide formatter. macOS installs the `MeasurementFormatter` one
/// so the command bar's conversions read exactly as they did before WP-12.
public enum MeasurementFormatters {
    public static var current: MeasurementFormatting = SymbolMeasurementFormatter()
}
