// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import Foundation

/// The macOS `SessionEvents`, over the `NSWorkspace` notifications the app
/// already observes plus the two distributed notifications the screen lock
/// posts.
///
/// Nothing here changes what the services do: they keep their own observers
/// until the per-feature migration. This is the shape the logind
/// implementation has to match.
final class MacSessionEvents: SessionEvents {
    private var observers: [SessionObservationToken: (SessionEvent) -> Void] = [:]
    private var tokens: [NSObjectProtocol] = []
    private var nextToken: UInt64 = 1
    private var locked = false

    var platformCapabilities: PlatformCapabilitySet {
        .all(.sessionSleepWakeEvents, .sessionLockEvents, .sessionFrontmostApplication,
             .sessionAppearanceEvents, .sessionIdleTime,
             .sessionDisplayConfigurationEvents)
    }

    var frontmostApplicationID: String? {
        NSWorkspace.shared.frontmostApplication?.bundleIdentifier
    }

    var isScreenLocked: Bool { locked }

    var isSessionActive: Bool { NSApplication.shared.isActive || !locked }

    var prefersDarkAppearance: Bool {
        NSApp?.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
    }

    var idleTime: TimeInterval? {
        // The smallest gap since any of the input kinds a person actually
        // produces. `CGEventType(rawValue: ~0)` — the "any event" idiom — is a
        // raw value no case carries, so its failable initializer returns nil;
        // naming the cases avoids a force-unwrap that would crash.
        let kinds: [CGEventType] = [.keyDown, .flagsChanged, .leftMouseDown,
                                    .rightMouseDown, .mouseMoved, .scrollWheel]
        return kinds
            .map { CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: $0) }
            .min()
    }

    func observe(_ onEvent: @escaping (SessionEvent) -> Void) -> SessionObservationToken {
        let token = SessionObservationToken(rawValue: nextToken)
        nextToken += 1
        observers[token] = onEvent
        if tokens.isEmpty { startObserving() }
        return token
    }

    func stopObserving(_ token: SessionObservationToken) {
        observers[token] = nil
        if observers.isEmpty { stopObservingAll() }
    }

    private func startObserving() {
        let workspace = NSWorkspace.shared.notificationCenter
        func add(_ name: Notification.Name,
                 on center: NotificationCenter,
                 _ event: @escaping (Notification) -> SessionEvent?) {
            tokens.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                guard let self, let resolved = event(note) else { return }
                self.deliver(resolved)
            })
        }

        add(NSWorkspace.willSleepNotification, on: workspace) { _ in .willSleep }
        add(NSWorkspace.didWakeNotification, on: workspace) { _ in .didWake }
        add(NSWorkspace.screensDidSleepNotification, on: workspace) { _ in .screensDidSleep }
        add(NSWorkspace.screensDidWakeNotification, on: workspace) { _ in .screensDidWake }
        add(NSWorkspace.sessionDidResignActiveNotification, on: workspace) { _ in .sessionBecameInactive }
        add(NSWorkspace.sessionDidBecomeActiveNotification, on: workspace) { _ in .sessionBecameActive }
        add(NSWorkspace.willPowerOffNotification, on: workspace) { _ in .willLogOut }
        add(NSWorkspace.didActivateApplicationNotification, on: workspace) { note in
            let application = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            return .frontmostApplicationChanged(appID: application?.bundleIdentifier)
        }
        add(NSApplication.didChangeScreenParametersNotification,
            on: NotificationCenter.default) { _ in .displayConfigurationChanged }

        let distributed = DistributedNotificationCenter.default()
        tokens.append(distributed.addObserver(forName: .init("com.apple.screenIsLocked"),
                                              object: nil, queue: .main) { [weak self] _ in
            self?.locked = true
            self?.deliver(.screenLocked)
        })
        tokens.append(distributed.addObserver(forName: .init("com.apple.screenIsUnlocked"),
                                              object: nil, queue: .main) { [weak self] _ in
            self?.locked = false
            self?.deliver(.screenUnlocked)
        })
    }

    private func stopObservingAll() {
        let workspace = NSWorkspace.shared.notificationCenter
        for token in tokens {
            workspace.removeObserver(token)
            NotificationCenter.default.removeObserver(token)
            DistributedNotificationCenter.default().removeObserver(token)
        }
        tokens.removeAll()
    }

    private func deliver(_ event: SessionEvent) {
        for observer in observers.values { observer(event) }
    }
}
#endif
