// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation

/// The macOS `TrashAndFiles`: `FileManager.trashItem`, which is the API the
/// seven call sites in `CORE_MOVES.md` § 4.1 use today and which
/// swift-corelibs-foundation does not have.
///
/// A wrapper, not a rewrite: the seven services keep calling `FileManager`
/// directly until the per-feature migration moves them onto this protocol.
/// What this type adds is a Linux-shaped surface over the same call, so the
/// freedesktop implementation has something to be equivalent *to*.
struct MacTrashAndFiles: TrashAndFiles {
    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.filesTrash, .filesRestoreFromTrash, .filesEnumerateTrash],
            unavailableReasons: [
                // Finder owns the trash; an app emptying it behind Finder's
                // back is how you lose someone's files without an undo.
                .filesEmptyTrash: "macOS leaves emptying the Trash to Finder",
                .filesTrashViaFileManager: "FileManager.trashItem is the system path here",
            ])
    }

    @discardableResult func trash(_ url: URL) throws -> TrashedItem {
        var resulting: NSURL?
        try FileManager.default.trashItem(at: url, resultingItemURL: &resulting)
        guard let trashedURL = resulting as URL? else {
            throw PlatformError.backendFailure("trashItem reported no resulting URL")
        }
        return TrashedItem(trashedURL: trashedURL, originalURL: url, deletedAt: Date())
    }

    @discardableResult func restore(_ item: TrashedItem) throws -> Bool {
        guard !FileManager.default.fileExists(atPath: item.originalURL.path) else { return false }
        try FileManager.default.moveItem(at: item.trashedURL, to: item.originalURL)
        return true
    }

    func trashContents() throws -> [TrashedItem] {
        let trash = try FileManager.default.url(for: .trashDirectory, in: .userDomainMask,
                                                appropriateFor: nil, create: false)
        let entries = try FileManager.default.contentsOfDirectory(
            at: trash, includingPropertiesForKeys: [.addedToDirectoryDateKey])
        return entries.map { url in
            let added = (try? url.resourceValues(forKeys: [.addedToDirectoryDateKey]))?
                .addedToDirectoryDate
            // macOS keeps the original location in its own index, which is not
            // readable from here; the Linux `.trashinfo` sidecar is. So the
            // original is the trashed name until a caller says otherwise —
            // honest, and enough for the cleaner's size estimate, which is the
            // only reader today.
            return TrashedItem(trashedURL: url, originalURL: url, deletedAt: added ?? Date())
        }
    }

    func emptyTrash() throws {
        throw PlatformError.unsupported(.filesEmptyTrash)
    }

    func trashSize() throws -> UInt64 {
        try trashContents().reduce(into: UInt64(0)) { total, item in
            let values = try? item.trashedURL.resourceValues(forKeys: [.totalFileAllocatedSizeKey])
            total += UInt64(values?.totalFileAllocatedSize ?? 0)
        }
    }

    func directory(for purpose: StandardDirectory) -> URL {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        func search(_ directory: FileManager.SearchPathDirectory) -> URL {
            (try? FileManager.default.url(for: directory, in: .userDomainMask,
                                          appropriateFor: nil, create: false))
                ?? home
        }
        switch purpose {
        case .configuration: return home.appendingPathComponent("Library/Preferences")
        case .cache: return search(.cachesDirectory)
        case .data: return search(.applicationSupportDirectory)
        case .logs: return home.appendingPathComponent("Library/Logs")
        case .downloads: return search(.downloadsDirectory)
        case .pictures: return search(.picturesDirectory)
        case .documents: return search(.documentDirectory)
        case .desktop: return search(.desktopDirectory)
        case .autostart: return home.appendingPathComponent("Library/LaunchAgents")
        }
    }

    func isSameVolume(_ a: URL, _ b: URL) -> Bool {
        let keys: Set<URLResourceKey> = [.volumeIdentifierKey]
        guard let left = try? a.resourceValues(forKeys: keys).volumeIdentifier,
              let right = try? b.resourceValues(forKeys: keys).volumeIdentifier
        else { return false }
        return left.isEqual(right)
    }
}
#endif
