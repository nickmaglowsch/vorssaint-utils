// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation
import VorssaintCore

/// An `InputInterceptor` with no privileged helper behind it.
///
/// Models the two states that matter and that no unit test can otherwise
/// reach: a helper that is present and authorised, and one that is not. The
/// default is the second — `/dev/uinput` missing with an errno, which is
/// exactly what the WP-03 container reported — so a test that wants the happy
/// path has to ask for it.
public final class FakeInputInterceptor: InputInterceptor {
    public var inputCapabilities: InputCapabilities
    public var backendName: String
    public private(set) var isEnabled = false
    public private(set) var rules = InputRules()
    public private(set) var isRecordingTap = false
    public var onInterceptionLost: ((InterceptionLoss) -> Void)?

    /// What `devices()` answers.
    public var installedDevices: [InputDevice] = []
    /// Set to refuse `setEnabled(true)`, as polkit does.
    public var enableFailure: PlatformError?
    /// Every rule document that was accepted, in order.
    public private(set) var acceptedRules: [InputRules] = []

    private var tapHandler: ((RecordedInput) -> Void)?

    public init(inputCapabilities: InputCapabilities = .helperMissing,
                backendName: String = "fake") {
        self.inputCapabilities = inputCapabilities
        self.backendName = backendName
    }

    public func setEnabled(_ enabled: Bool) throws {
        if let enableFailure, enabled { throw enableFailure }
        isEnabled = enabled
        if !enabled { stopRecordingTap() }
    }

    public func setRules(_ rules: InputRules) throws {
        // The helper range-checks before accepting and keeps the previous
        // document when it refuses (PRIVILEGES.md § 4.1); so does this.
        guard rules.tapThresholdMilliseconds >= 0,
              rules.tapThresholdMilliseconds <= 5000,
              rules.chatterMilliseconds >= 0
        else { throw PlatformError.invalidArgument("tap_threshold_ms out of range") }
        self.rules = rules
        acceptedRules.append(rules)
    }

    public func devices() throws -> [InputDevice] { installedDevices }

    public func startRecordingTap(onEvent: @escaping (RecordedInput) -> Void) throws {
        guard inputCapabilities.canRecordTap else {
            throw PlatformError.unsupported(.inputRecordTap)
        }
        tapHandler = onEvent
        isRecordingTap = true
    }

    public func stopRecordingTap() {
        tapHandler = nil
        isRecordingTap = false
    }

    // MARK: Scripting

    /// Delivers one event, which only happens in tap mode — the whole point
    /// of the helper's design.
    public func emit(_ event: RecordedInput) {
        tapHandler?(event)
    }

    /// Drops the interception the way the platform does, so a test can check
    /// the feature re-arms and explains itself.
    public func lose(_ reason: InterceptionLoss) {
        isEnabled = false
        stopRecordingTap()
        onInterceptionLost?(reason)
    }
}

public extension InputCapabilities {
    /// No helper: nothing can be changed, nothing can be recorded. The errno
    /// is the one the WP-03 container actually produced.
    static let helperMissing = InputCapabilities(
        canSynthesize: false,
        synthesizeUnavailableReason: "open /dev/uinput: No such file or directory (errno 2)",
        canSwallow: false,
        canNameTargetApplication: false,
        canRecordTap: false,
        eventNodeCount: nil,
        requiresHelper: true)

    /// A helper that is installed and authorised.
    static let helperReady = InputCapabilities(
        canSynthesize: true,
        synthesizeUnavailableReason: nil,
        canSwallow: true,
        canNameTargetApplication: true,
        canRecordTap: true,
        eventNodeCount: 4,
        requiresHelper: true)

    /// macOS with Accessibility granted: a tap, no grab, no helper.
    static let eventTap = InputCapabilities(
        canSynthesize: true,
        synthesizeUnavailableReason: nil,
        canSwallow: true,
        canNameTargetApplication: true,
        canRecordTap: true,
        eventNodeCount: nil,
        requiresHelper: false)
}
