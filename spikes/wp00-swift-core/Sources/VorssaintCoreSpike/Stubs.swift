// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// WP-00 spike stubs: the minimum non-framework shims the vendored Vorssaint
// sources are expected to need on Linux. Anything added here is a finding —
// it names something the real port must either provide in `VorssaintCore` or
// push behind a `Platform` protocol. Apple *frameworks* are shimmed as
// separate targets instead (see `Sources/*Shim`).

import Foundation

#if os(Linux)
/// `FileManager.trashItem(at:resultingItemURL:)` has no
/// swift-corelibs-foundation counterpart. The Linux answer is the
/// freedesktop.org trash spec (`~/.local/share/Trash`) or
/// `org.freedesktop.FileManager1.TrashFiles`; the spike only needs a symbol.
public enum LinuxTrash {
    public static func trashItem(at url: URL) throws {
        throw CocoaError(.featureUnsupported)
    }
}

/// `Bundle.main` exists on Linux but has no bundle identifier and no
/// resources next to an SwiftPM executable. Files that read
/// `Bundle.main.bundleIdentifier` need a real replacement in the port; the
/// spike records which ones by leaving this unused.
public enum LinuxBundleIdentity {
    public static let identifier = "org.vorssaint.linux.spike"
}
#endif
