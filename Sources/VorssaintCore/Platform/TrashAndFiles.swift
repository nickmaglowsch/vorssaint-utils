// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Where a file went when it was trashed, so it can be put back.
public struct TrashedItem: Equatable {
    /// Where it is now: `~/.local/share/Trash/files/<name>` on Linux,
    /// the `.Trash` URL `FileManager.trashItem` reports on macOS.
    public let trashedURL: URL
    /// Where it came from, which the freedesktop spec stores in the
    /// `.trashinfo` sidecar and macOS stores in its own index.
    public let originalURL: URL
    public let deletedAt: Date

    public init(trashedURL: URL, originalURL: URL, deletedAt: Date) {
        self.trashedURL = trashedURL
        self.originalURL = originalURL
        self.deletedAt = deletedAt
    }
}

/// Moves files to the trash, and the few filesystem questions the platform
/// answers differently.
///
/// This is `FileManager.trashItem`'s home in the port — the fourth of the
/// Foundation gaps WP-00 § 8 condition 3 named. `CORE_MOVES.md` § 4.1 recorded
/// that it is used in **seven** files, none of them in the moved set:
///
///   Services/Cleaner/JunkCleaner.swift:199
///   Services/DiskImageInstaller/DiskImageInstallerService.swift:299
///   Services/ManagedDownloads/WhatsAppDownloadOrganizer.swift:584
///   Services/ManagedDownloads/WhatsAppDownloadManager.swift:218
///   Services/QuickTools/ScreenshotService.swift:429
///   Services/Uninstall/AppUninstaller.swift:302
///   Core/BundleMigration.swift:100
///
/// On Linux the implementation is the freedesktop trash specification: move
/// into `$XDG_DATA_HOME/Trash/files`, write the `.trashinfo` sidecar into
/// `Trash/info`, and use the volume's own `.Trash-$uid` for anything not on
/// the home filesystem — because `rename(2)` across filesystems fails, and a
/// silent copy-and-delete would be a very different promise. Where a file
/// manager is running, `org.freedesktop.FileManager1.TrashFiles` is preferred
/// so its own undo stack stays correct.
public protocol TrashAndFiles: PlatformService {
    /// Moves to the trash. Throws rather than deleting when the platform
    /// cannot trash — never silently falls back to an unrecoverable delete.
    @discardableResult func trash(_ url: URL) throws -> TrashedItem

    /// Puts one back where it came from. `false` when something is already at
    /// the original path.
    @discardableResult func restore(_ item: TrashedItem) throws -> Bool

    /// Everything currently in the trash, for the cleaner's "empty trash"
    /// size estimate.
    func trashContents() throws -> [TrashedItem]

    func emptyTrash() throws

    /// Bytes the trash is holding.
    func trashSize() throws -> UInt64

    /// The session's standard directory for a purpose, so the core never
    /// hard-codes `~/Library/...` or `~/.local/share/...`.
    func directory(for purpose: StandardDirectory) -> URL

    /// Whether two paths are on the same filesystem — the question that
    /// decides move-versus-copy, and which trash directory a file belongs in.
    func isSameVolume(_ a: URL, _ b: URL) -> Bool
}

/// The directories the app needs, named by what they are for rather than by
/// either platform's layout.
public enum StandardDirectory: String, CaseIterable {
    /// Settings. `~/Library/Preferences` / `$XDG_CONFIG_HOME`.
    case configuration
    /// Things the app can regenerate. `~/Library/Caches` / `$XDG_CACHE_HOME`.
    case cache
    /// Things it cannot. `~/Library/Application Support` / `$XDG_DATA_HOME`.
    case data
    /// Logs.
    case logs
    case downloads
    case pictures
    case documents
    case desktop
    /// Autostart entries. `~/Library/LaunchAgents` / `$XDG_CONFIG_HOME/autostart`.
    case autostart
}

public extension PlatformCapability {
    static let filesTrash = PlatformCapability("files.trash")
    /// Can put a trashed file back where it came from.
    static let filesRestoreFromTrash = PlatformCapability("files.restoreFromTrash")
    static let filesEnumerateTrash = PlatformCapability("files.enumerateTrash")
    static let filesEmptyTrash = PlatformCapability("files.emptyTrash")
    /// Trashing goes through the running file manager, so its undo stack
    /// stays correct.
    static let filesTrashViaFileManager = PlatformCapability("files.trashViaFileManager")
}
