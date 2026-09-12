// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import PackageDescription

// Target layout (docs/linux-port/PLAN.md § 5, WP-10):
//
//   VorssaintCore    Foundation-only, builds on Darwin and Linux.
//   VorssaintMac     macOS platform module; body guarded by `#if os(macOS)`.
//   VorssaintLinux   Linux executable; body guarded by `#if os(Linux)`.
//   Vorssaint        the existing macOS app executable, unchanged in content.
//
// SwiftPM has no Linux platform declaration and `platforms:` only constrains
// Apple platforms, so "Linux-only" and "macOS-only" are expressed as `#if os`
// guards inside the sources plus `.when(platforms:)` on dependencies. On
// macOS the Linux executable therefore still builds — it just has an empty
// body. On Linux the CI builds named targets (`--target VorssaintCore`,
// `--target VorssaintLinux`) rather than the whole package, because the
// `Vorssaint` app target is AppKit/IOKit code that will never compile there.
//
// The `VorssaintCombine` re-export target WP-10 scaffolded is gone (WP-12, on
// the decision recorded in docs/linux-port/COMBINE.md § 7): nothing could
// import it. `build.sh` compiles Sources/Vorssaint, Sources/VorssaintCore and
// Sources/VorssaintMac into one swiftc invocation, where that module does not
// exist, so every file needing Combine writes the per-file guard of
// COMBINE.md § 1 instead. The OpenCombine products stay, Linux-conditional, on
// VorssaintCore — they are what make the guard's `#else` branch resolve.
//
// No target may be named `Combine`: Swift 6.1 rejects
// `circular dependency between modules 'VorssaintCombine' and 'Combine'`
// (docs/linux-port/spikes/00-swift-core.md § 6). That rule outlives the target.

// The macOS half of the package, spliced in only when the manifest itself is
// compiled on a Mac (WP-16). `swift test` builds *every* target in the package,
// not just the test target and its dependencies, so on Linux the presence of
// `VorssaintMac` and the `Vorssaint` app — AppKit/IOKit code that will never
// compile there — would fail the run before a single test executed. `swift
// build --target …` is unaffected either way, so nothing about the macOS
// product changes: on macOS both lists are exactly what they were.
#if os(macOS)
let macOSProducts: [Product] = [
    .library(name: "VorssaintMac", targets: ["VorssaintMac"])
]
let macOSTargets: [Target] = [
    // macOS platform module. WP-12 moves the AppKit/IOKit services here.
    .target(
        name: "VorssaintMac",
        dependencies: ["VorssaintCore", "VMStatisticsCompat", "HIDEventSystem"],
        path: "Sources/VorssaintMac"
    ),

    // The macOS app. Its sources have not moved; build.sh still compiles
    // Sources/Vorssaint, Sources/VorssaintCore and Sources/VorssaintMac
    // into one module.
    .executableTarget(
        name: "Vorssaint",
        dependencies: [
            "VMStatisticsCompat", "HIDEventSystem",
            "VorssaintCore", "VorssaintMac"
        ],
        path: "Sources/Vorssaint"
    )
]
#else
let macOSProducts: [Product] = []
let macOSTargets: [Target] = []
#endif

let package = Package(
    name: "Vorssaint",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "VorssaintCore", targets: ["VorssaintCore"]),
        .library(name: "VorssaintCoreTestSupport", targets: ["VorssaintCoreTestSupport"]),
        .executable(name: "VorssaintLinux", targets: ["VorssaintLinux"])
    ] + macOSProducts,
    dependencies: [
        .package(url: "https://github.com/OpenCombine/OpenCombine.git", from: "0.14.0")
    ],
    targets: [
        .systemLibrary(
            name: "HIDEventSystem",
            path: "Sources/HIDEventSystem"
        ),
        .systemLibrary(
            name: "VMStatisticsCompat",
            path: "Sources/VMStatisticsCompat"
        ),

        // Platform-free core. WP-11 moves the real Foundation-only files here.
        //
        // It depends on the OpenCombine products directly rather than on
        // VorssaintCombine: build.sh compiles this directory into the single
        // macOS app module, where no `VorssaintCombine` module exists, so a
        // core file that needs Combine must spell it
        // `#if canImport(Darwin) import Combine #else import OpenCombine`.
        // These conditional dependencies are what make the `#else` branch
        // resolve under SwiftPM on Linux (PLAN.md § 5).
        .target(
            name: "VorssaintCore",
            dependencies: [
                .product(name: "OpenCombine", package: "OpenCombine",
                         condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineDispatch", package: "OpenCombine",
                         condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineFoundation", package: "OpenCombine",
                         condition: .when(platforms: [.linux]))
            ],
            path: "Sources/VorssaintCore"
        ),

        // Fake implementations of the Platform protocols (WP-12), for tests and
        // for the Linux shell's own harnesses.
        //
        // Not compiled into the macOS app: `build.sh` globs only
        // Sources/Vorssaint, Sources/VorssaintCore and Sources/VorssaintMac, so
        // nothing here can reach the product. Building it on Linux CI is what
        // proves the Platform protocols are usable across a real module
        // boundary, which the single-module Mac build cannot show.
        .target(
            name: "VorssaintCoreTestSupport",
            dependencies: ["VorssaintCore"],
            path: "Sources/VorssaintCoreTestSupport"
        ),

        // Linux executable. Body is `#if os(Linux)`; a no-op binary on macOS.
        .executableTarget(
            name: "VorssaintLinux",
            dependencies: ["VorssaintCore"],
            path: "Sources/VorssaintLinux"
        ),

        // The pure checks of the `build.sh --test` harness, selected out of
        // Tests/*.swift by Tools/linux-port/port-tests.py and committed as
        // Tests/VorssaintCoreTests/Generated*.swift (docs/linux-port/TESTS.md).
        // XCTest rather than swift-testing: it is the one test library that
        // ships with both toolchains this package is built with.
        //
        // `build.sh --test` does not read this directory — it still compiles
        // the original Tests/*.swift with swiftc — so the macOS check count is
        // untouched by anything here.
        .testTarget(
            name: "VorssaintCoreTests",
            dependencies: [
                "VorssaintCore",
                "VorssaintCoreTestSupport",
                .product(name: "OpenCombine", package: "OpenCombine",
                         condition: .when(platforms: [.linux]))
            ],
            path: "Tests/VorssaintCoreTests"
        )
    ] + macOSTargets
)
