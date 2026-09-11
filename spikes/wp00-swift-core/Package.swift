// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike package. Not part of the product build: it exists only to put
// the candidate Vorssaint sources in front of a real Linux Swift compiler.
// `sync.sh` vendors the sources; `Sources/*Shim` stand in for the Apple
// frameworks; OpenCombine stands in for Combine.

import PackageDescription

let package = Package(
    name: "VorssaintCoreSpike",
    products: [
        .library(name: "VorssaintCoreSpike", targets: ["VorssaintCoreSpike"]),
        .library(name: "VorssaintCoreWide", targets: ["VorssaintCoreWide"]),
        .executable(name: "hello-spike", targets: ["HelloSpike"])
    ],
    dependencies: [
        .package(url: "https://github.com/OpenCombine/OpenCombine.git", from: "0.14.0")
    ],
    targets: [
        // --- Apple framework shims -----------------------------------------
        .target(name: "CoreGraphics", path: "Sources/CoreGraphicsShim"),
        .target(name: "Carbon", path: "Sources/CarbonShim"),
        .target(name: "AppKit", dependencies: ["CoreGraphics"], path: "Sources/AppKitShim"),
        .target(name: "SwiftUI", dependencies: ["CoreGraphics"], path: "Sources/SwiftUIShim"),
        .target(name: "CryptoKit", path: "Sources/CryptoKitShim"),
        .target(name: "Security", path: "Sources/SecurityShim"),
        .target(name: "UniformTypeIdentifiers", path: "Sources/UTTypeShim"),
        .target(name: "ImageIO", dependencies: ["CoreGraphics"], path: "Sources/ImageIOShim"),
        .target(name: "ApplicationServices", dependencies: ["CoreGraphics"],
                path: "Sources/ApplicationServicesShim"),
        .target(name: "CoreAudio", path: "Sources/CoreAudioShim"),

        // --- Combine abstraction -------------------------------------------
        .target(
            name: "VorssaintCombine",
            dependencies: [
                .product(name: "OpenCombine", package: "OpenCombine", condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineFoundation", package: "OpenCombine", condition: .when(platforms: [.linux])),
                .product(name: "OpenCombineDispatch", package: "OpenCombine", condition: .when(platforms: [.linux]))
            ]
        ),
        // Lets vendored sources keep their unmodified `import Combine`.
        .target(name: "Combine", dependencies: ["VorssaintCombine"], path: "Sources/CombineShim"),

        // --- The thing under test ------------------------------------------
        // The core candidate set: the 101 files the census does not call
        // "not portable", plus Defaults.swift and GlobalShortcut.swift,
        // which the rest of the set needs in scope (`DefaultsKey`,
        // `GlobalShortcut`) and which are pure logic behind a Carbon key
        // table.
        .target(
            name: "VorssaintCoreSpike",
            dependencies: [
                "CoreGraphics", "Carbon", "AppKit", "Combine", "VorssaintCombine"
            ]
        ),
        // All 120 census files, including the ones the census calls not
        // portable. Expected to fail; its log is the error census.
        .target(
            name: "VorssaintCoreWide",
            dependencies: [
                "CoreGraphics", "Carbon", "AppKit", "SwiftUI", "CryptoKit", "Security",
                "UniformTypeIdentifiers", "ImageIO", "ApplicationServices", "CoreAudio",
                "Combine", "VorssaintCombine"
            ]
        ),

        .executableTarget(name: "HelloSpike"),

        .testTarget(name: "VorssaintCoreSpikeTests", dependencies: ["VorssaintCoreSpike"])
    ]
)
