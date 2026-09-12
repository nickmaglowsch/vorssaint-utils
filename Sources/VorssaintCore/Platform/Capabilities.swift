// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// What kind of desktop this is.
///
/// Declared, never assumed (`PLAN.md` § 4.5). Every value here is *probed* at
/// start-up: the environment variable is a hint, the answer is whether the
/// interface actually responded.
public struct SessionDescription: Equatable {
    /// `"macos"`, `"gnome"`, `"plasma"`, `"sway"`, `"hyprland"`, `"xfce"`, …
    /// Free-form: a desktop nobody anticipated should read as itself rather
    /// than as "other".
    public let desktop: String
    /// `"wayland"`, `"x11"`, `"quartz"`.
    public let displayProtocol: String
    /// Desktop version where one is meaningful, for the per-version
    /// compatibility the GNOME extension needs (`PLAN.md` § 4.6).
    public let desktopVersion: String?
    /// `true` inside a Flatpak or Snap, where several capabilities are gone
    /// regardless of the desktop (`PRIVILEGES.md` § 6).
    public let isSandboxed: Bool

    public init(desktop: String, displayProtocol: String,
                desktopVersion: String?, isSandboxed: Bool) {
        self.desktop = desktop
        self.displayProtocol = displayProtocol
        self.desktopVersion = desktopVersion
        self.isSandboxed = isSandboxed
    }
}

/// One thing probed at start-up, and what the probe found.
public struct CapabilityProbe: Equatable, Identifiable {
    public let id: PlatformCapability
    public let isAvailable: Bool
    /// What was actually tried, so the Capabilities page can show a reason the
    /// user can act on: "org.kde.KWin is not on the session bus", "opening
    /// /dev/uinput: No such file or directory (errno 2)".
    ///
    /// The helper's `GetCapabilities` sets the standard here: answer by
    /// *trying*, never by inferring from a name, "because a device node can
    /// exist with no driver behind it" (`PRIVILEGES.md` § 4.1).
    public let detail: String?
    /// What the user or the packager can do about it, when there is something.
    public let remedy: String?

    public init(id: PlatformCapability, isAvailable: Bool, detail: String?, remedy: String?) {
        self.id = id
        self.isAvailable = isAvailable
        self.detail = detail
        self.remedy = remedy
    }
}

/// The one place that answers "what can this session do?".
///
/// `FeatureCatalog` gains `requiredCapabilities` per feature (WP-15); the hub,
/// the Capabilities page and the energy badges all render from what this
/// reports. It replaces the macOS Permissions model, which asked a fixed set
/// of yes/no questions that do not exist on Linux.
///
/// This protocol does not *do* anything — it aggregates what the other
/// thirteen report, plus the session facts none of them own.
public protocol Capabilities: PlatformService {
    var session: SessionDescription { get }

    /// Every probe, in a stable order, for the Capabilities page.
    var probes: [CapabilityProbe] { get }

    /// The fast question every feature asks.
    func has(_ capability: PlatformCapability) -> Bool

    /// Why not, and what to do about it. `nil` when the capability is present.
    func explanation(for capability: PlatformCapability) -> CapabilityProbe?

    /// Re-probes. Called after the user installs the GNOME extension, the
    /// helper, or `ddcutil`, so the hub updates without a restart.
    func refresh()

    /// Fires when a probe's answer changed, so the hub re-renders rather than
    /// polling.
    var onChange: (() -> Void)? { get set }
}

public extension Capabilities {
    /// A feature is offerable when every capability it names is present.
    func supportsAll(_ required: [PlatformCapability]) -> Bool {
        required.allSatisfy(has)
    }

    /// The ones standing in the way, for the message the hub shows.
    func missing(from required: [PlatformCapability]) -> [CapabilityProbe] {
        required.compactMap(explanation(for:))
    }
}
