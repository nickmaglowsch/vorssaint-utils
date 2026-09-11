// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike: proof that the vendored logic does not merely compile on Linux
// but produces the same answers. Two representatives — one string/URL parser,
// one locale-sensitive numeric parser (the one most likely to differ between
// Apple Foundation and swift-corelibs-foundation).

import XCTest
@testable import VorssaintCoreSpike

final class URLCleaningSpikeTests: XCTestCase {
    func testStripsGlobalTrackers() {
        let result = URLCleaning.clean("https://example.com/a?utm_source=news&keep=1&fbclid=zz")
        XCTAssertEqual(result?.url, "https://example.com/a?keep=1")
        XCTAssertEqual(result?.removed.sorted(), ["fbclid", "utm_source"])
    }

    func testStripsHostSpecificTrackerOnlyOnThatHost() {
        let youtube = URLCleaning.clean("https://youtu.be/abc?si=token&t=42")
        XCTAssertEqual(youtube?.url, "https://youtu.be/abc?t=42")

        // `si` is a real parameter elsewhere and must survive.
        let other = URLCleaning.clean("https://example.org/x?si=token")
        XCTAssertEqual(other?.url, "https://example.org/x?si=token")
        XCTAssertEqual(other?.removed, [])
    }

    func testLeavesCleanLinkAlone() {
        let result = URLCleaning.clean("https://example.com/a?keep=1")
        XCTAssertEqual(URLCleaning.outcome(for: result, input: "https://example.com/a?keep=1"),
                       .unchanged)
    }

    func testNonURLTextIsRejected() {
        XCTAssertNil(URLCleaning.clean("just some words"))
    }
}

final class CommandBarMathSpikeTests: XCTestCase {
    private let en = Locale(identifier: "en_US")

    func testArithmetic() {
        let result = CommandBarMath.evaluate("12 * (3 + 4)",
                                             decimalSeparator: ".",
                                             groupingSeparator: ",",
                                             locale: en)
        XCTAssertEqual(result?.value, 84)
        XCTAssertEqual(result?.formatted, "84")
    }

    func testFloatingPointNoiseIsRoundedAway() {
        let result = CommandBarMath.evaluate("0.1 + 0.2",
                                             decimalSeparator: ".",
                                             groupingSeparator: ",",
                                             locale: en)
        XCTAssertEqual(result?.formatted, "0.3")
    }

    func testPercentOf() {
        let result = CommandBarMath.evaluate("20% of 250",
                                             decimalSeparator: ".",
                                             groupingSeparator: ",",
                                             locale: en)
        XCTAssertEqual(result?.value, 50)
    }

    func testADateIsNotAnExpression() {
        XCTAssertNil(CommandBarMath.evaluate("2026-07-27",
                                             decimalSeparator: ".",
                                             groupingSeparator: ",",
                                             locale: en))
    }

    func testBareNumberIsNotAnExpression() {
        XCTAssertNil(CommandBarMath.evaluate("42",
                                             decimalSeparator: ".",
                                             groupingSeparator: ",",
                                             locale: en))
    }
}
