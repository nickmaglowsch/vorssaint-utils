// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import PackageDescription

// Target layout (docs/linux-port/PLAN.md § 5, WP-10):
//
//   VorssaintCore    Foundation-only, builds on Darwin and Linux.
//   VorssaintMac     macOS platform module; body guarded by `#if os(macOS)`.
//   VorssaintLinux   Linux executable; body guarded by `#if os(Linux)`.
//   VorssaintCombine Combine on Darwin, OpenCombine 0.14 on Linux (WP-13).
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
// No target may be named `Combine`: Swift 6.1 rejects
// `circular dependency between modules 'VorssaintCombine' and 'Combine'`
// (docs/linux-port/spikes/00-swift-core.md § 6).

let package = Package(
    name: "Vorssaint",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "VorssaintCore", targets: ["VorssaintCore"]),
        .library(name: "VorssaintCombine", targets: ["VorssaintCombine"]),
        .library(name: "VorssaintMac", targets: ["VorssaintMac"]),
        .executable(name: "VorssaintLinux", targets: ["VorssaintLinux"])
    ],
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

        // Combine on Darwin, OpenCombine on Linux. WP-13 owns the contents.
        .target(
            name: "VorssaintCombine",
            dependencies: [
                .product(name: "OpenCombine", package: "OpenCombine",
                         condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineDispatch", package: "OpenCombine",
                         condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineFoundation", package: "OpenCombine",
                         condition: .when(platforms: [.linux]))
            ],
            path: "Sources/VorssaintCombine"
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

        // macOS platform module. WP-12 moves the AppKit/IOKit services here.
        .target(
            name: "VorssaintMac",
            dependencies: ["VorssaintCore", "VMStatisticsCompat", "HIDEventSystem"],
            path: "Sources/VorssaintMac"
        ),

        // Linux executable. Body is `#if os(Linux)`; a no-op binary on macOS.
        .executableTarget(
            name: "VorssaintLinux",
            dependencies: ["VorssaintCore"],
            path: "Sources/VorssaintLinux"
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
)
