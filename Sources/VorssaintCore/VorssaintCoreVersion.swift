// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Placeholder anchor for the platform-free core (WP-10).
///
/// `VorssaintCore` is the Foundation-only module every platform target builds
/// against. It is nearly empty on purpose: WP-11 moves the real
/// Foundation-only files in. Until then this file is what proves the target
/// compiles on both Darwin and Linux.
public enum VorssaintCoreVersion {
    /// Version of the core module, independent of the app bundle version.
    public static let string = "0.1.0-dev"

    /// Name of the platform the core was compiled for. The only `#if os` in
    /// the core; it exists so `VorssaintLinux` can print a line that proves
    /// which compiler built it.
    public static var platformName: String {
        #if os(Linux)
        return "linux"
        #elseif os(macOS)
        return "macos"
        #else
        return "unknown"
        #endif
    }

    /// One-line identification, printed by the Linux executable.
    public static var banner: String {
        "VorssaintCore \(string) (\(platformName))"
    }
}
