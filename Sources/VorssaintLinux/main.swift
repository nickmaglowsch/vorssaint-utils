// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Placeholder entry point for the Linux executable (WP-10).
//
// SwiftPM has no way to declare a target Linux-only, so the body is guarded:
// on macOS this compiles to an executable that does nothing, which keeps
// `swift build` on macOS green without a second package manifest.

#if os(Linux)
import Foundation
import VorssaintCore

print(VorssaintCoreVersion.banner)
print("VorssaintLinux \(VorssaintCoreVersion.string) placeholder — no UI yet")
#endif
