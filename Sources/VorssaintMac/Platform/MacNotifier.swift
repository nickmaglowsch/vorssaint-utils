// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation
import UserNotifications

/// The macOS `PlatformNotifier`, wrapping `Services/Notifier.swift`.
///
/// A wrapper in the strict sense: it calls `Notifier.post(title:body:)` and
/// `Notifier.requestPermission()` and adds nothing, so the notifications the
/// app shows are byte-for-byte the ones it showed before WP-12.
///
/// The action-button path is deliberately *not* reimplemented here.
/// `Notifier.postWhatsAppOrganization` builds a `UNNotificationCategory` with
/// an undo action and reads a transaction UUID back out of the response; that
/// is feature logic living in the feature, and moving it would be the
/// per-feature migration this work package is explicitly not doing. So
/// `platformCapabilities` reports `.notifyActions` absent with the reason,
/// rather than a second, differently-behaving implementation of the same
/// notification.
struct MacNotifier: PlatformNotifier {
    var onResponse: ((NotificationResponse) -> Void)?

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.notifyPost, .notifyRequiresAuthorization],
            unavailableReasons: [
                .notifyActions: "action buttons stay in Services/Notifier.swift until "
                    + "the managed-downloads feature is migrated",
                .notifyWithdraw: "not wired; Notifier does not withdraw today",
            ])
    }

    var authorizationStatus: NotificationAuthorization {
        // Read synchronously from the last known settings: `UNUserNotification-
        // Center.getNotificationSettings` is asynchronous, and every caller
        // here wants an answer now to decide whether to draw a row. A caller
        // that needs the authoritative answer calls `requestAuthorization()`.
        MacNotifierAuthorizationCache.lastKnown
    }

    func requestAuthorization() async -> NotificationAuthorization {
        Notifier.requestPermission()
        let settings = await UNUserNotificationCenter.current().notificationSettings()
        let status: NotificationAuthorization
        switch settings.authorizationStatus {
        case .notDetermined: status = .notDetermined
        case .denied: status = .denied
        default: status = .authorized
        }
        MacNotifierAuthorizationCache.lastKnown = status
        return status
    }

    @discardableResult func post(_ notification: PlatformNotification) throws -> String {
        guard notification.actions.isEmpty else {
            throw PlatformError.unsupported(.notifyActions)
        }
        Notifier.post(title: notification.title, body: notification.body)
        return notification.identifier ?? UUID().uuidString
    }

    func withdraw(_ identifier: String) {}
}

/// Last authorization answer seen, so the synchronous accessor has something
/// true to say rather than guessing. Starts `.notDetermined`, which is what it
/// genuinely is until something asks.
enum MacNotifierAuthorizationCache {
    static var lastKnown: NotificationAuthorization = .notDetermined
}
#endif
