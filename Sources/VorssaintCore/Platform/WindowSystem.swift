// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if canImport(CoreGraphics)
import CoreGraphics
#endif
import Foundation

// The Swift mirror of the `window` section of
// `linux/platform/include/vorssaint_platform.h` (WP-C1). The mapping table in
// `linux/platform/README.md` § "How WP-12 mirrors it" is the contract:
//
//   vs_window_system      -> protocol WindowSystem
//   vs_window_capability  -> WindowSystemCapabilities (OptionSet)
//   vs_window_info        -> WindowInfo
//   vs_window_flag        -> WindowInfo.Flags (OptionSet)
//   vs_rect               -> WindowRect (integer members, CGRect on demand)
//   vs_result             -> PlatformError, VS_OK a normal return
//   vs_window_event + cb  -> WindowEvent, delivered from dispatch()
//   list + free_list      -> one call returning [WindowInfo]
//
// Two rules from that README hold here: the C side is the source of truth for
// names and semantics, and the Swift side adds no behaviour — retries,
// tolerance checks and read-back live in C, so `moveResize` carries a
// tolerance rather than a Swift-side comparison loop.

/// `vs_window_id`: `uint64_t`, opaque, stable for the lifetime of the window
/// within one backend instance.
///
/// Not `PlatformWindowID` (which is `UInt32`, the capture-surface identifier
/// `CGWindowID` already is). They are different domains: X11 uses an XID,
/// wlroots a handle serial, Hyprland a 64-bit window *address*, so nothing
/// narrower than 64 bits can carry this. The macOS adapter widens its
/// `CGWindowID` into one of these losslessly; the reverse narrowing is only
/// ever valid on macOS, and `WindowSystemCapabilities` does not promise it.
public typealias WindowSystemID = UInt64

/// `vs_rect`: integer members, top-left origin, layout coordinates.
///
/// Integers rather than `CGFloat` because the C struct is `int32_t` and the
/// wrapper marshals rather than converts. `cgRect` is there for the services
/// above, which speak `CGRect`.
public struct WindowRect: Equatable, Codable {
    public var x: Int32
    public var y: Int32
    public var width: Int32
    public var height: Int32

    public init(x: Int32, y: Int32, width: Int32, height: Int32) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public init(_ rect: CGRect) {
        self.init(x: Int32(rect.origin.x.rounded()),
                  y: Int32(rect.origin.y.rounded()),
                  width: Int32(rect.size.width.rounded()),
                  height: Int32(rect.size.height.rounded()))
    }

    public var cgRect: CGRect {
        CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(width), height: CGFloat(height))
    }

    public static let zero = WindowRect(x: 0, y: 0, width: 0, height: 0)
}

/// `vs_window_info`.
///
/// The field list is `SwitcherItem`'s, which is what the C header says it was
/// shaped from. "Unknown is never zero": `pid` and `workspace` are `nil` here
/// rather than the C side's `-1`, because Swift has an option type and 0 is a
/// real pid and a real workspace.
public struct WindowInfo: Equatable, Identifiable {
    /// `vs_window_flag`.
    public struct Flags: OptionSet, Hashable, Codable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }

        public static let minimized = Flags(rawValue: 1 << 0)
        public static let focused = Flags(rawValue: 1 << 1)
        public static let fullscreen = Flags(rawValue: 1 << 2)
        public static let maximized = Flags(rawValue: 1 << 3)
        /// The window is on the workspace that is currently shown. The
        /// switcher's `isOnHiddenSpace` is the negation of this.
        public static let onCurrentWorkspace = Flags(rawValue: 1 << 4)
        /// `frame` carries a real frame. Foreign-toplevel compositors report
        /// no geometry, so the switcher must not assume one.
        public static let hasGeometry = Flags(rawValue: 1 << 5)
        /// `processID` is real rather than the C side's -1 placeholder.
        public static let hasProcessID = Flags(rawValue: 1 << 6)
        /// Mapped and would be drawn if nothing covered it; `isOnScreen`.
        public static let onScreen = Flags(rawValue: 1 << 7)
    }

    public let id: WindowSystemID
    /// Wayland `app_id`, X11 `WM_CLASS` instance, bundle identifier on macOS.
    /// The stable identity every per-app rule matches on — `SwitcherAppRule`,
    /// `MouseAppExceptionSupport`, `AutoQuitSupport`.
    public let appID: String
    /// `SwitcherItem.appName`.
    public let appName: String
    /// `SwitcherItem.title`. May be empty; the switcher falls back to the app
    /// name itself.
    public let title: String
    /// Owning process, `nil` when the backend cannot say. autoQuit needs this
    /// to signal the application.
    public let processID: Int32?
    /// Layout coordinates, top-left origin, meaningful only when
    /// `flags` contains `.hasGeometry`.
    public let frame: WindowRect
    public let flags: Flags
    /// Workspace / virtual desktop index, `nil` when unknown.
    public let workspace: Int32?
    /// Output (monitor) name, `""` when unknown. A name rather than an id
    /// because that is what the C header carries and what Wayland gives.
    public let output: String
    /// Stacking order, 0 = bottom-most, valid only when `stackingIsValid`.
    /// `list()` returns windows in this order, so a caller that needs front to
    /// back reads the array backwards.
    public let stackingIndex: UInt32
    public let stackingIsValid: Bool

    public init(id: WindowSystemID, appID: String, appName: String, title: String,
                processID: Int32?, frame: WindowRect, flags: Flags,
                workspace: Int32?, output: String,
                stackingIndex: UInt32, stackingIsValid: Bool) {
        self.id = id
        self.appID = appID
        self.appName = appName
        self.title = title
        self.processID = processID
        self.frame = frame
        self.flags = flags
        self.workspace = workspace
        self.output = output
        self.stackingIndex = stackingIndex
        self.stackingIsValid = stackingIsValid
    }

    public var isMinimized: Bool { flags.contains(.minimized) }
    public var isFocused: Bool { flags.contains(.focused) }
    public var isFullscreen: Bool { flags.contains(.fullscreen) }
    public var isOnScreen: Bool { flags.contains(.onScreen) }
    /// `SwitcherItem.isOnHiddenSpace`.
    public var isOnHiddenWorkspace: Bool { !flags.contains(.onCurrentWorkspace) }
    public var hasGeometry: Bool { flags.contains(.hasGeometry) }
}

/// `vs_window_capability`. An `OptionSet` because the C side is a bitmask and
/// the mirror must keep its shape.
///
/// "A member whose capability bit is clear still exists and returns
/// `VS_ERR_UNSUPPORTED`" — so in Swift every method exists too, and throws
/// `PlatformError.unsupported`. Callers never test for `nil`.
///
/// Capabilities only ever shrink, and only when the channel that carried them
/// goes away, which arrives as `WindowEvent.backendLost`. A caller that cached
/// this value re-reads it on that event and nowhere else.
public struct WindowSystemCapabilities: OptionSet, Hashable, Codable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// `list` returns the session's toplevels.
    public static let canList = WindowSystemCapabilities(rawValue: 1 << 0)
    /// `activate` raises and focuses a window.
    public static let canActivate = WindowSystemCapabilities(rawValue: 1 << 1)
    /// `close` asks a window to close, never kills the process.
    public static let canClose = WindowSystemCapabilities(rawValue: 1 << 2)
    /// `setMinimized` both minimizes and restores.
    public static let canMinimize = WindowSystemCapabilities(rawValue: 1 << 3)
    /// `moveResize` places a window and `geometry` reads it back.
    public static let canMoveResize = WindowSystemCapabilities(rawValue: 1 << 4)
    /// `setWorkspace` switches the active workspace.
    public static let canWorkspaceSwitch = WindowSystemCapabilities(rawValue: 1 << 5)
    /// The backend pushes events. Without it the caller polls `list` itself.
    public static let hasLiveEvents = WindowSystemCapabilities(rawValue: 1 << 6)
    /// The backend can name a per-window capture source for live previews.
    /// Reserved for WP-C3; no backend sets it yet.
    public static let hasPreviews = WindowSystemCapabilities(rawValue: 1 << 7)
}

/// `vs_window_event` plus `vs_window_event_type`, as one value.
public enum WindowEvent: Equatable {
    case added(WindowInfo)
    case removed(WindowSystemID)
    /// Title, state or geometry changed. `info` is `nil` for backends that
    /// cannot describe the window in the event.
    case changed(WindowSystemID, WindowInfo?)
    case activated(WindowSystemID, WindowInfo?)
    case workspaceChanged(Int32)
    /// The backend lost the channel its control verbs rode on. `capabilities`
    /// has already shrunk to what still works — often listing alone — so the
    /// caller re-reads it, and recreates the backend when it wants the rest
    /// back.
    case backendLost
}

/// Lists, focuses and moves windows.
///
/// Consumed by `WindowEnumerator`, `WindowActivator`, `WindowLayoutService`
/// and `AutoQuitService`, which is the set the C header says it was shaped
/// from. The macOS adapter implements it over AppKit and Accessibility; the
/// Linux side wraps `vs_window_system` and nothing else.
///
/// Threading follows the C contract exactly: **one instance belongs to one
/// thread**, no call is reentrant, and events are delivered only from inside
/// `dispatch()`, on the thread that calls it. Nothing here starts a thread.
public protocol WindowSystem: PlatformService {
    /// Backend identity, for logs and the Capabilities page: `"x11"`, `"wlr"`,
    /// `"hyprland"`, `"kwin"`, `"gnome"`, `"appkit"`.
    var name: String { get }

    /// `vs_window_system.capabilities`. Re-read after `WindowEvent.backendLost`
    /// and at no other time.
    var capabilities: WindowSystemCapabilities { get }

    /// Snapshot of every toplevel, bottom-most first. Budget: 100 ms.
    func list() throws -> [WindowInfo]

    /// Raise and focus, restoring it first when minimized.
    func activate(_ id: WindowSystemID) throws

    /// Ask the window to close, so unsaved-changes dialogs still appear.
    func close(_ id: WindowSystemID) throws

    func setMinimized(_ minimized: Bool, of id: WindowSystemID) throws

    /// Place the window. The backend reads the frame back and throws
    /// `PlatformError.notApplied` when the window landed outside `tolerance`
    /// pixels of the request — which is how `WindowLayoutService` decides
    /// success today. A tolerance of 0 skips the read-back.
    func moveResize(_ id: WindowSystemID, frame: WindowRect, tolerance: Int32) throws

    /// Current frame, for the caller's own read-back and for layout history.
    func geometry(of id: WindowSystemID) throws -> WindowRect

    func currentWorkspace() throws -> Int32

    func setWorkspace(_ workspace: Int32) throws

    /// Install the event sink; `nil` removes it. The callback runs only inside
    /// `dispatch()`, and must not call back into this instance until
    /// `dispatch()` has returned.
    func setEventCallback(_ callback: ((WindowEvent) -> Void)?)

    /// Pollable descriptor that becomes readable when events are pending, or
    /// `nil` when the backend has none (`.hasLiveEvents` clear). On macOS this
    /// is always `nil`: AppKit and Accessibility deliver on the run loop, and
    /// the adapter drains into `dispatch()` from there.
    var eventFileDescriptor: Int32? { get }

    /// Drain what is pending into the callback. Never blocks. Returns how many
    /// events were delivered.
    @discardableResult func dispatch() throws -> Int
}

public extension WindowSystem {
    /// The uniform capability form, derived from the C bitmask so the two can
    /// never disagree.
    var platformCapabilities: PlatformCapabilitySet {
        var supported: Set<PlatformCapability> = []
        if capabilities.contains(.canList) { supported.insert(.windowList) }
        if capabilities.contains(.canActivate) { supported.insert(.windowFocus) }
        if capabilities.contains(.canClose) { supported.insert(.windowClose) }
        if capabilities.contains(.canMinimize) { supported.insert(.windowMinimize) }
        if capabilities.contains(.canMoveResize) { supported.insert(.windowMoveResize) }
        if capabilities.contains(.canWorkspaceSwitch) { supported.insert(.windowWorkspaces) }
        if capabilities.contains(.hasLiveEvents) { supported.insert(.windowEvents) }
        if capabilities.contains(.hasPreviews) { supported.insert(.windowPreviews) }
        return PlatformCapabilitySet(supported: supported)
    }
}

public extension PlatformCapability {
    static let windowList = PlatformCapability("window.list")
    static let windowFocus = PlatformCapability("window.focus")
    static let windowClose = PlatformCapability("window.close")
    static let windowMinimize = PlatformCapability("window.minimize")
    static let windowMoveResize = PlatformCapability("window.moveResize")
    static let windowWorkspaces = PlatformCapability("window.workspaces")
    static let windowEvents = PlatformCapability("window.events")
    static let windowPreviews = PlatformCapability("window.previews")
}
