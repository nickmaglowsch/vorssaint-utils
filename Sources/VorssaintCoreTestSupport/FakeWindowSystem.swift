// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation
import VorssaintCore

/// A `WindowSystem` with no compositor behind it.
///
/// Scriptable in the two ways the real backends differ: `capabilities` can be
/// narrowed so a test sees what a foreign-toplevel session sees, and
/// `moveResizeLands` can be told to accept the request and not apply it, which
/// is `VS_ERR_NOT_APPLIED` — the failure mode the C header exists to make
/// visible and the one every real backend produces sooner or later.
///
/// Events follow the C contract: queued by the test, delivered only from
/// `dispatch()`, on the calling thread.
public final class FakeWindowSystem: WindowSystem {
    public var name: String
    public var capabilities: WindowSystemCapabilities

    /// The session's windows, bottom-most first.
    public var windows: [WindowInfo]

    /// When false, `moveResize` records the attempt and then throws
    /// `PlatformError.notApplied` without changing the window — a compositor
    /// that says yes and does nothing.
    public var moveResizeLands = true

    /// Every mutating call, in order, so a test can assert what was asked for
    /// rather than only what came back.
    public private(set) var calls: [Call] = []

    public enum Call: Equatable {
        case activate(WindowSystemID)
        case close(WindowSystemID)
        case setMinimized(WindowSystemID, Bool)
        case moveResize(WindowSystemID, WindowRect, Int32)
        case setWorkspace(Int32)
    }

    private var callback: ((WindowEvent) -> Void)?
    private var pending: [WindowEvent] = []
    private var workspace: Int32 = 0

    public init(name: String = "fake",
                capabilities: WindowSystemCapabilities = [
                    .canList, .canActivate, .canClose, .canMinimize,
                    .canMoveResize, .canWorkspaceSwitch, .hasLiveEvents
                ],
                windows: [WindowInfo] = []) {
        self.name = name
        self.capabilities = capabilities
        self.windows = windows
    }

    // MARK: Scripting

    /// Queues an event for the next `dispatch()`.
    public func enqueue(_ event: WindowEvent) {
        pending.append(event)
    }

    /// Narrows `capabilities` and queues `backendLost`, which is the only way
    /// the C contract allows capabilities to change.
    public func loseBackend(keeping remaining: WindowSystemCapabilities) {
        capabilities = remaining
        pending.append(.backendLost)
    }

    public func clearCalls() { calls.removeAll() }

    // MARK: WindowSystem

    public func list() throws -> [WindowInfo] {
        try require(.canList)
        return windows
    }

    public func activate(_ id: WindowSystemID) throws {
        try require(.canActivate)
        calls.append(.activate(id))
        guard let index = windows.firstIndex(where: { $0.id == id }) else {
            throw PlatformError.notFound
        }
        for other in windows.indices {
            windows[other] = withFlags(windows[other], setting: .focused, to: other == index)
        }
    }

    public func close(_ id: WindowSystemID) throws {
        try require(.canClose)
        calls.append(.close(id))
        guard windows.contains(where: { $0.id == id }) else { throw PlatformError.notFound }
        windows.removeAll { $0.id == id }
        pending.append(.removed(id))
    }

    public func setMinimized(_ minimized: Bool, of id: WindowSystemID) throws {
        try require(.canMinimize)
        calls.append(.setMinimized(id, minimized))
        guard let index = windows.firstIndex(where: { $0.id == id }) else {
            throw PlatformError.notFound
        }
        windows[index] = withFlags(windows[index], setting: .minimized, to: minimized)
    }

    public func moveResize(_ id: WindowSystemID, frame: WindowRect, tolerance: Int32) throws {
        try require(.canMoveResize)
        calls.append(.moveResize(id, frame, tolerance))
        guard let index = windows.firstIndex(where: { $0.id == id }) else {
            throw PlatformError.notFound
        }
        guard moveResizeLands else { throw PlatformError.notApplied }
        let old = windows[index]
        windows[index] = WindowInfo(
            id: old.id, appID: old.appID, appName: old.appName, title: old.title,
            processID: old.processID, frame: frame,
            flags: old.flags.union(.hasGeometry), workspace: old.workspace,
            output: old.output, stackingIndex: old.stackingIndex,
            stackingIsValid: old.stackingIsValid)
    }

    public func geometry(of id: WindowSystemID) throws -> WindowRect {
        try require(.canMoveResize)
        guard let window = windows.first(where: { $0.id == id }) else {
            throw PlatformError.notFound
        }
        guard window.hasGeometry else { throw PlatformError.unsupported(.windowMoveResize) }
        return window.frame
    }

    public func currentWorkspace() throws -> Int32 { workspace }

    public func setWorkspace(_ workspace: Int32) throws {
        try require(.canWorkspaceSwitch)
        calls.append(.setWorkspace(workspace))
        self.workspace = workspace
        pending.append(.workspaceChanged(workspace))
    }

    public func setEventCallback(_ callback: ((WindowEvent) -> Void)?) {
        self.callback = callback
    }

    public var eventFileDescriptor: Int32? {
        capabilities.contains(.hasLiveEvents) ? -1 : nil
    }

    @discardableResult public func dispatch() throws -> Int {
        let queued = pending
        pending.removeAll()
        guard let callback else { return 0 }
        for event in queued { callback(event) }
        return queued.count
    }

    // MARK: Helpers

    private func require(_ capability: WindowSystemCapabilities) throws {
        guard capabilities.contains(capability) else {
            throw PlatformError.unsupported(.windowList)
        }
    }

    private func withFlags(_ window: WindowInfo,
                           setting flag: WindowInfo.Flags,
                           to on: Bool) -> WindowInfo {
        WindowInfo(id: window.id, appID: window.appID, appName: window.appName,
                   title: window.title, processID: window.processID,
                   frame: window.frame,
                   flags: on ? window.flags.union(flag) : window.flags.subtracting(flag),
                   workspace: window.workspace, output: window.output,
                   stackingIndex: window.stackingIndex,
                   stackingIsValid: window.stackingIsValid)
    }
}

public extension WindowInfo {
    /// A window with everything a test does not care about filled in.
    static func fake(id: WindowSystemID,
                     appID: String = "com.example.app",
                     appName: String = "Example",
                     title: String = "Window",
                     processID: Int32? = 1234,
                     frame: WindowRect = WindowRect(x: 0, y: 0, width: 800, height: 600),
                     flags: Flags = [.hasGeometry, .hasProcessID, .onScreen, .onCurrentWorkspace],
                     workspace: Int32? = 0,
                     output: String = "fake-0",
                     stackingIndex: UInt32 = 0,
                     stackingIsValid: Bool = true) -> WindowInfo {
        WindowInfo(id: id, appID: appID, appName: appName, title: title,
                   processID: processID, frame: frame, flags: flags,
                   workspace: workspace, output: output,
                   stackingIndex: stackingIndex, stackingIsValid: stackingIsValid)
    }
}
