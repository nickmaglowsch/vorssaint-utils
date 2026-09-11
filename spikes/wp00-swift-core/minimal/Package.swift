// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike, minimal package. Separate from the parent package on purpose:
// `swift test` builds every target in a package, and the parent deliberately
// contains targets that do not compile (that is its job). This one holds only
// files that declare everything they use, so a green `swift test` here is a
// real statement about Linux behaviour rather than about the spike harness.

import PackageDescription

let package = Package(
    name: "VorssaintCoreMinimal",
    products: [
        .library(name: "VorssaintCoreMinimal", targets: ["VorssaintCoreMinimal"])
    ],
    targets: [
        .target(name: "VorssaintCoreMinimal"),
        .testTarget(name: "VorssaintCoreMinimalTests", dependencies: ["VorssaintCoreMinimal"])
    ]
)
