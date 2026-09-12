// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// One thing a platform backend either can or cannot do.
///
/// The port's central rule (`docs/linux-port/PLAN.md` § 4.5) is that desktop
/// support is *declared, not assumed*: GNOME has no foreign-toplevel protocol,
/// a wlroots session has no KWin scripting, a Flatpak sandbox has no
/// `/dev/uinput`, and the same Linux build runs on all three. So every
/// platform protocol answers "what can you do here?" before a feature asks it
/// to do anything, and the feature hub renders the answer.
///
/// A raw string rather than an enum on purpose: a backend added later
/// (`WP-C2`'s GNOME extension, a new portal revision) declares a capability
/// without editing the core, and an unknown capability read back from a C
/// vtable is data, not a crash.
public struct PlatformCapability: RawRepresentable, Hashable, Codable, CustomStringConvertible {
    public let rawValue: String

    public init(rawValue: String) { self.rawValue = rawValue }
    public init(_ rawValue: String) { self.rawValue = rawValue }

    public var description: String { rawValue }
}

/// What one platform protocol's implementation can do on this session.
///
/// Deliberately not an `OptionSet`: capabilities are open-ended (see above),
/// and a set of names survives a round trip through the C vtable and through
/// a `CoreBridge` snapshot unchanged.
public struct PlatformCapabilitySet: Equatable, Codable {
    public let supported: Set<PlatformCapability>

    /// Why a capability is missing, when the backend knows. Rendered by the
    /// Capabilities page, so "your compositor does not implement
    /// `ext-foreign-toplevel-list`" reaches the person instead of a blank row.
    public let unavailableReasons: [PlatformCapability: String]

    public init(supported: Set<PlatformCapability>,
                unavailableReasons: [PlatformCapability: String] = [:]) {
        self.supported = supported
        self.unavailableReasons = unavailableReasons
    }

    /// Everything this protocol declares, available.
    public static func all(_ capabilities: PlatformCapability...) -> PlatformCapabilitySet {
        PlatformCapabilitySet(supported: Set(capabilities))
    }

    /// Nothing available, with one reason that covers all of it.
    public static func none(reason: String) -> PlatformCapabilitySet {
        PlatformCapabilitySet(supported: [], unavailableReasons: [:])
            .withBlanketReason(reason)
    }

    public func has(_ capability: PlatformCapability) -> Bool {
        supported.contains(capability)
    }

    public func reason(for capability: PlatformCapability) -> String? {
        has(capability) ? nil : (unavailableReasons[capability] ?? blanketReason)
    }

    private var blanketReason: String? { unavailableReasons[.blanket] }

    private func withBlanketReason(_ reason: String) -> PlatformCapabilitySet {
        PlatformCapabilitySet(supported: supported,
                              unavailableReasons: unavailableReasons.merging([.blanket: reason]) { a, _ in a })
    }
}

public extension PlatformCapability {
    /// Key under which a whole-protocol "why not" is stored.
    static let blanket = PlatformCapability("_blanket")
}

/// The one thing every platform protocol has in common.
///
/// A protocol in this directory is a *question the core asks the platform*,
/// never a class the core owns. Implementations live in
/// `Sources/VorssaintMac/Platform` (AppKit/IOKit/CoreAudio) and, on Linux, in
/// `Sources/VorssaintLinux` as thin wrappers over the C function-pointer table
/// in `linux/platform/include/vorssaint_platform.h` (`PLAN.md` § 5).
public protocol PlatformService {
    /// What this implementation can do on the session it is running in, in the
    /// one shape the Capabilities page and the feature hub render.
    ///
    /// Named `platformCapabilities` rather than `capabilities` because some
    /// protocols also carry a *native* capability value that mirrors a C
    /// bitmask one-to-one — `WindowSystem.capabilities` is
    /// `vs_window_capability` and must keep that name and that shape
    /// (`linux/platform/README.md`, "How WP-12 mirrors it", rule 1). Those
    /// protocols derive this uniform form from the native one.
    var platformCapabilities: PlatformCapabilitySet { get }
}

/// What a platform call fails with.
///
/// Four cases, because the caller acts differently on each: `unsupported` is
/// a feature-hub message, `denied` is a permission or polkit prompt,
/// `unavailable` is "try again, the backend is not up", and `backendFailure`
/// is a bug worth logging.
/// Mirrors `vs_result` in `linux/platform/include/vorssaint_platform.h`
/// case for case, so the Swift wrapper marshals rather than interprets
/// (`linux/platform/README.md`: "The Swift side adds no behaviour").
/// `VS_OK` has no case here — it is a normal return.
public enum PlatformError: Error, Equatable {
    /// `VS_ERR_UNSUPPORTED`. The running session's backend cannot do this at
    /// all. Check `platformCapabilities` first; this is the belt-and-braces
    /// answer.
    case unsupported(PlatformCapability)
    /// `VS_ERR_NOT_FOUND`. The object id is not, or no longer, known.
    case notFound
    /// `VS_ERR_BACKEND`. The compositor, display server or bridge refused or
    /// failed.
    case backendFailure(String)
    /// `VS_ERR_TIMEOUT`. The backend answered too slowly; the caller may retry.
    case timedOut
    /// `VS_ERR_INVALID`. Malformed arguments.
    case invalidArgument(String)
    /// `VS_ERR_NO_BACKEND`. No backend could be selected for this session.
    case noBackend
    /// `VS_ERR_NOT_APPLIED`. The request was sent and acknowledged, and
    /// reading the state back showed it did not take effect. This is the
    /// playbook's "read back after writing" rule with a name: compositors,
    /// D-Bus bridges and X11 window managers all report success without
    /// effect, and so does macOS Accessibility.
    case notApplied
    /// The user, the portal or polkit said no. No `vs_result` case — it
    /// arrives as `org.freedesktop.DBus.Error.AccessDenied` from the helper
    /// (`docs/linux-port/PRIVILEGES.md` § 4.1) and as a permission refusal on
    /// macOS.
    case denied(String)
    /// The backend exists but is not reachable right now: the helper is not
    /// installed, PipeWire has not started, the compositor is restarting.
    case unavailable(String)
}
