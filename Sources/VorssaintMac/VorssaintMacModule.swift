// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Placeholder anchor for the macOS platform module (WP-10).
//
// `VorssaintMac` will hold today's AppKit/IOKit/ScreenCaptureKit/CoreAudio
// services and the SwiftUI views once WP-12 has written the platform adapter.
// Nothing has moved yet. The whole body is guarded so that a full
// `swift build` on Linux does not fail on a target it has no business
// building, and so that `build.sh` can compile this directory straight into
// the single-module macOS app.

#if os(macOS)
import Foundation

/// Placeholder anchor for the macOS platform module.
public enum VorssaintMacModule {
    /// Identification line for the macOS platform module.
    public static let banner = "VorssaintMac 0.1.0-dev"
}
#endif
