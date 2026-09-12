// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import CoreGraphics
import Foundation

/// The macOS `WindowSystem`, over the window server's own list.
///
/// `list()` is real: `CGWindowListCopyWindowInfo` is what `WindowEnumerator`
/// already reads, and the fields map one for one onto `vs_window_info` — which
/// is unsurprising, since the C header says it was shaped from `SwitcherItem`.
/// This is the adapter that proves the mirror is faithful in both directions.
///
/// The acting verbs are declared absent. Every one of them goes through
/// Accessibility in `WindowActivator` and `WindowLayoutService`, which hold a
/// per-process `AXUIElement` cache, a retry ladder and the frame read-back with
/// tolerance that `moveResize` names. A second implementation would either
/// duplicate that cache or race it. Migrating those services is per-feature
/// work; until then the capability bits are honest and a caller explains
/// itself rather than calling something that silently does nothing.
final class MacWindowSystem: WindowSystem {
    private var callback: ((WindowEvent) -> Void)?

    let name = "appkit"

    var capabilities: WindowSystemCapabilities { [.canList] }

    func list() throws -> [WindowInfo] {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]]
        else { throw PlatformError.backendFailure("CGWindowListCopyWindowInfo returned nothing") }

        // CGWindowList is front-to-back; `list()` promises bottom-most first,
        // so the array is reversed and the stacking index counts up from the
        // back, exactly as the C contract describes.
        let frontToBack = raw.filter { ($0[kCGWindowLayer as String] as? Int) == 0 }
        var windows: [WindowInfo] = []
        for (offset, entry) in frontToBack.reversed().enumerated() {
            guard let number = entry[kCGWindowNumber as String] as? UInt32 else { continue }
            let pid = entry[kCGWindowOwnerPID as String] as? Int32
            let owner = entry[kCGWindowOwnerName as String] as? String ?? ""
            let title = entry[kCGWindowName as String] as? String ?? ""
            let application = pid.flatMap { NSRunningApplication(processIdentifier: $0) }

            var flags: WindowInfo.Flags = [.onScreen, .onCurrentWorkspace]
            if pid != nil { flags.insert(.hasProcessID) }
            if application?.isActive == true { flags.insert(.focused) }

            var frame = WindowRect.zero
            if let bounds = entry[kCGWindowBounds as String] as? [String: CGFloat],
               let rect = CGRect(dictionaryRepresentation: bounds as CFDictionary) {
                frame = WindowRect(rect)
                flags.insert(.hasGeometry)
            }

            windows.append(WindowInfo(
                id: WindowSystemID(number),
                appID: application?.bundleIdentifier ?? owner,
                appName: owner,
                title: title,
                processID: pid,
                frame: frame,
                flags: flags,
                workspace: nil,
                output: "",
                stackingIndex: UInt32(offset),
                stackingIsValid: true))
        }
        return windows
    }

    func activate(_ id: WindowSystemID) throws {
        throw PlatformError.unsupported(.windowFocus)
    }

    func close(_ id: WindowSystemID) throws {
        throw PlatformError.unsupported(.windowClose)
    }

    func setMinimized(_ minimized: Bool, of id: WindowSystemID) throws {
        throw PlatformError.unsupported(.windowMinimize)
    }

    func moveResize(_ id: WindowSystemID, frame: WindowRect, tolerance: Int32) throws {
        throw PlatformError.unsupported(.windowMoveResize)
    }

    func geometry(of id: WindowSystemID) throws -> WindowRect {
        guard let window = try list().first(where: { $0.id == id }) else {
            throw PlatformError.notFound
        }
        guard window.hasGeometry else { throw PlatformError.unsupported(.windowMoveResize) }
        return window.frame
    }

    func currentWorkspace() throws -> Int32 {
        throw PlatformError.unsupported(.windowWorkspaces)
    }

    func setWorkspace(_ workspace: Int32) throws {
        throw PlatformError.unsupported(.windowWorkspaces)
    }

    func setEventCallback(_ callback: ((WindowEvent) -> Void)?) {
        self.callback = callback
    }

    /// Always `nil`: AppKit and Accessibility deliver on the run loop, not
    /// through a descriptor, so there is nothing to poll. `.hasLiveEvents` is
    /// clear to match, and a caller polls `list()` — which is what the macOS
    /// switcher does today.
    var eventFileDescriptor: Int32? { nil }

    @discardableResult func dispatch() throws -> Int { 0 }
}
#endif
