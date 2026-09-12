// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// The envelope a settings backup is written in.
///
/// Split out of `Sources/Vorssaint/Core/SettingsBackupSupport.swift` by WP-14
/// so both platforms name the same three keys and the same version. Nothing
/// about the format changes for the port: the file stays an XML property list
/// with these three entries, which is what lets a backup written on a Mac
/// import on Linux for the keys both builds share. The rest of
/// `SettingsBackupSupport` — which keys travel, which never do, and what a
/// value has to look like before it is written back — stays in the Mac layer
/// until the thirty `Support` types it validates against reach the core.
enum SettingsBackupFormat {
    static let formatVersionKey = "vorssaintBackupVersion"
    static let appVersionKey = "vorssaintBackupAppVersion"
    static let settingsKey = "settings"
    static let formatVersion = 1
}
