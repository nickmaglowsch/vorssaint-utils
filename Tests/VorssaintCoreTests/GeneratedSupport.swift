// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Hand-written support for the generated cases (WP-16). `formatSpecifiers`
// and `placeholderShape` are copied verbatim from Tests/MetricsTests.swift,
// where they are private static helpers of the harness struct.

import Foundation

enum GeneratedSupport {
    /// The placeholders a format string carries, sorted, so two languages can
    /// be compared without caring about the order they read in.
    static func placeholderShape(_ value: String) -> [String] {
        formatSpecifiers(in: value).sorted()
    }

    static func formatSpecifiers(in format: String) -> [String] {
        var specifiers: [String] = []
        var index = format.startIndex
        while index < format.endIndex {
            guard format[index] == "%" else {
                index = format.index(after: index)
                continue
            }
            index = format.index(after: index)
            if index < format.endIndex, format[index] == "%" {
                index = format.index(after: index)
                continue
            }
            while index < format.endIndex {
                let character = format[index]
                if character.isLetter || character == "@" {
                    specifiers.append(String(character))
                    index = format.index(after: index)
                    break
                }
                index = format.index(after: index)
            }
        }
        return specifiers
    }
}
