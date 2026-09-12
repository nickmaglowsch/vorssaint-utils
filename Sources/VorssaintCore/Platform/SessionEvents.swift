// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Something that happened to the session as a whole.
///
/// These are the `NSWorkspace` notifications the app already observes —
/// `willSleepNotification`, `didWakeNotification`, `screensDidSleep`,
/// `sessionDidResignActive`, `didActivateApplicationNotification` — named for
/// what they mean rather than for either platform's notification name. On
/// Linux they come from logind (`PrepareForSleep`, `Lock`, `Unlock`,
/// `LockedHint`) and from the window system's focus events.
public enum SessionEvent: Equatable {
    /// About to suspend. Both platforms give a short window here, and
    /// `BluetoothSleepSupport` and `KeepAwakeManager` use it.
    case willSleep
    case didWake
    case screensDidSleep
    case screensDidWake
    case screenLocked
    case screenUnlocked
    /// Fast user switching, or another session taking the seat.
    case sessionBecameInactive
    case sessionBecameActive
    /// The frontmost application changed. `appID` is the same identity string
    /// `WindowInfo.appID` carries.
    case frontmostApplicationChanged(appID: String?)
    /// The session is ending. The last chance to persist.
    case willLogOut
    /// Displays were added, removed or rearranged.
    case displayConfigurationChanged
    /// The desktop's light/dark preference changed — `AppAppearance` follows
    /// this. On Linux it is the `org.freedesktop.appearance color-scheme`
    /// setting from the Settings portal.
    case appearanceChanged(isDark: Bool)
}

/// Reports what happens to the session.
///
/// A pure observer: it changes nothing, which is why it has no capability for
/// acting. `PowerControl` holds the verbs (`sleepNow`, `lockSession`).
public protocol SessionEvents: PlatformService {
    /// Subscribes. The token unsubscribes.
    func observe(_ onEvent: @escaping (SessionEvent) -> Void) -> SessionObservationToken

    func stopObserving(_ token: SessionObservationToken)

    /// The frontmost application right now, for features that need the answer
    /// before the first event arrives.
    var frontmostApplicationID: String? { get }

    var isScreenLocked: Bool { get }

    var isSessionActive: Bool { get }

    /// `true` when the desktop is set to a dark appearance.
    var prefersDarkAppearance: Bool { get }

    /// How long since the user last touched anything. `nil` where the session
    /// will not say — which is the common Wayland answer, since idle time is
    /// an input fact and Wayland does not hand those out. `KeepAwake`'s
    /// automations degrade to their timer-only behaviour when this is `nil`.
    var idleTime: TimeInterval? { get }
}

public struct SessionObservationToken: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    static let sessionSleepWakeEvents = PlatformCapability("session.sleepWakeEvents")
    static let sessionLockEvents = PlatformCapability("session.lockEvents")
    /// Can report which application is frontmost. Needs the window layer on
    /// Linux, so it is unavailable wherever `WindowSystem` is.
    static let sessionFrontmostApplication = PlatformCapability("session.frontmostApplication")
    static let sessionAppearanceEvents = PlatformCapability("session.appearanceEvents")
    static let sessionIdleTime = PlatformCapability("session.idleTime")
    static let sessionDisplayConfigurationEvents = PlatformCapability("session.displayConfigurationEvents")
}
