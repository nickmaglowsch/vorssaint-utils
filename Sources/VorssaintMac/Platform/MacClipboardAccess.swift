// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import Foundation

/// The macOS `ClipboardAccess`, over `NSPasteboard.general`.
///
/// The change *watch* is a poll of `changeCount`, because that is all macOS
/// offers and it is what `ClipboardHistoryService` already does. The Linux
/// backends get a real change event from `ext_data_control_manager_v1`, so
/// `.clipboardWatch` is present on both but by different means — which is why
/// the protocol exposes watching as a capability rather than assuming a timer.
final class MacClipboardAccess: ClipboardAccess {
    private let pasteboard = NSPasteboard.general
    private var watchers: [ClipboardWatchToken: (ClipboardContent) -> Void] = [:]
    private var nextToken: UInt64 = 1
    private var timer: Timer?
    private var lastChangeCount: Int

    init() {
        lastChangeCount = NSPasteboard.general.changeCount
    }

    deinit { timer?.invalidate() }

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.clipboardRead, .clipboardWrite, .clipboardWatch,
                        .clipboardConcealedEntries],
            unavailableReasons: [
                .clipboardPrimarySelection: "macOS has no primary selection",
                .clipboardSourceApplication:
                    "NSPasteboard does not name the owner; the history service "
                    + "infers it from the frontmost application instead",
            ])
    }

    private static let types: [ClipboardFlavor: NSPasteboard.PasteboardType] = [
        .utf8Text: .string,
        .html: .html,
        .rtf: .rtf,
        .png: .png,
        .tiff: .tiff,
        .fileList: .fileURL,
    ]

    func read(flavors: [ClipboardFlavor]) -> ClipboardContent? {
        var found: [ClipboardFlavor: Data] = [:]
        for flavor in flavors {
            guard let type = Self.types[flavor], let data = pasteboard.data(forType: type)
            else { continue }
            found[flavor] = data
        }
        guard !found.isEmpty else { return nil }
        return ClipboardContent(flavors: found, sourceApplicationID: nil)
    }

    func write(_ content: ClipboardContent, concealed: Bool) throws {
        pasteboard.clearContents()
        var wrote = false
        for (flavor, data) in content.flavors {
            guard let type = Self.types[flavor] else { continue }
            wrote = pasteboard.setData(data, forType: type) || wrote
        }
        if concealed {
            // What password managers set so other clipboard managers skip the
            // entry. Failing to set it must not fail the write itself.
            _ = pasteboard.setData(Data(), forType: .init("org.nspasteboard.ConcealedType"))
        }
        guard wrote else {
            throw PlatformError.backendFailure("no writable flavor in the content")
        }
        lastChangeCount = pasteboard.changeCount
    }

    func clear() {
        pasteboard.clearContents()
        lastChangeCount = pasteboard.changeCount
    }

    func watch(_ onChange: @escaping (ClipboardContent) -> Void) -> ClipboardWatchToken? {
        let token = ClipboardWatchToken(rawValue: nextToken)
        nextToken += 1
        watchers[token] = onChange
        if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
                self?.poll()
            }
        }
        return token
    }

    func stopWatching(_ token: ClipboardWatchToken) {
        watchers[token] = nil
        if watchers.isEmpty {
            timer?.invalidate()
            timer = nil
        }
    }

    func readPrimarySelection() -> ClipboardContent? { nil }

    private func poll() {
        let count = pasteboard.changeCount
        guard count != lastChangeCount else { return }
        lastChangeCount = count
        guard let content = read(flavors: Array(Self.types.keys)) else { return }
        for watcher in watchers.values { watcher(content) }
    }
}
#endif
