// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// One notification to show.
///
/// The fields are what `Services/Notifier.swift` already posts, including the
/// WhatsApp organizer's action buttons and its transaction id, which it reads
/// back out of the response to undo a move.
public struct PlatformNotification: Equatable {
    public struct Action: Equatable {
        /// Returned in the response so the caller can tell which was pressed.
        public let id: String
        public let title: String
        /// Destructive actions are drawn differently on both platforms.
        public let isDestructive: Bool

        public init(id: String, title: String, isDestructive: Bool = false) {
            self.id = id
            self.title = title
            self.isDestructive = isDestructive
        }
    }

    public let title: String
    public let body: String
    public let actions: [Action]
    /// Opaque payload handed back with the response. The WhatsApp organizer
    /// stores a transaction UUID here.
    public let userInfo: [String: String]
    /// Replaces an earlier notification with the same id rather than stacking
    /// a second one — `replaces_id` on the freedesktop interface, the request
    /// identifier on macOS.
    public let identifier: String?

    public init(title: String, body: String, actions: [Action] = [],
                userInfo: [String: String] = [:], identifier: String? = nil) {
        self.title = title
        self.body = body
        self.actions = actions
        self.userInfo = userInfo
        self.identifier = identifier
    }
}

/// The user's answer to a notification.
public struct NotificationResponse: Equatable {
    public let identifier: String?
    /// `nil` when the notification body itself was clicked rather than an
    /// action button.
    public let actionID: String?
    public let userInfo: [String: String]

    public init(identifier: String?, actionID: String?, userInfo: [String: String]) {
        self.identifier = identifier
        self.actionID = actionID
        self.userInfo = userInfo
    }
}

/// Whether the session will show notifications at all.
public enum NotificationAuthorization: Equatable {
    case notDetermined
    case authorized
    case denied
}

/// Posts notifications and reports what the user did with them.
///
/// Named `PlatformNotifier`, in a file of the same name, because
/// `Services/Notifier.swift` already declares `enum Notifier` and `build.sh`
/// compiles `Sources/Vorssaint`, `Sources/VorssaintCore` and
/// `Sources/VorssaintMac` into **one** `swiftc` invocation (`PLAN.md` § 5).
/// The type name would collide — and so would the *file* name: a single
/// invocation rejects two files with the same basename outright
/// (`error: filename "Notifier.swift" used twice`, run 34693363900), because
/// it uses filenames to tell private declarations apart. Any file added under
/// these three directories must have a basename unique across all three.
///
/// macOS is `UNUserNotificationCenter`, which needs an authorization request;
/// Linux is the `org.freedesktop.Notifications` D-Bus interface, or the
/// `Notification` portal inside a sandbox, neither of which asks permission —
/// which is exactly what `authorizationStatus` reports honestly rather than
/// pretending one platform's model onto the other.
public protocol PlatformNotifier: PlatformService {
    var authorizationStatus: NotificationAuthorization { get }

    /// Asks, where the platform has a concept of asking. Returns the status
    /// afterwards — `.authorized` immediately on a platform that never asks.
    func requestAuthorization() async -> NotificationAuthorization

    /// Shows it. Returns the id the platform assigned, for a later
    /// `withdraw`.
    @discardableResult func post(_ notification: PlatformNotification) throws -> String

    /// Takes one back down.
    func withdraw(_ identifier: String)

    /// Called when the user clicks a notification or one of its actions.
    var onResponse: ((NotificationResponse) -> Void)? { get set }
}

public extension PlatformCapability {
    static let notifyPost = PlatformCapability("notify.post")
    /// Action buttons. The freedesktop interface advertises this per server;
    /// some notification daemons show none.
    static let notifyActions = PlatformCapability("notify.actions")
    /// A posted notification can be taken back down.
    static let notifyWithdraw = PlatformCapability("notify.withdraw")
    /// The platform asks the user for permission before the first
    /// notification.
    static let notifyRequiresAuthorization = PlatformCapability("notify.requiresAuthorization")
}
