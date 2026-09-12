// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if canImport(CoreGraphics)
import CoreGraphics
#endif
import Foundation

// Mirrors `org.vorssaint.Helper1` (`docs/linux-port/PRIVILEGES.md` § 4.1,
// merged as WP-S1), which is the only way input interception happens on Linux:
//
//   Enable(b)          -> setEnabled(_:)
//   SetRules(s json)   -> setRules(_:)
//   GetDevices(out s)  -> devices()
//   GetCapabilities()  -> platformCapabilities / inputCapabilities
//   Event(t,q,q,i)     -> startRecordingTap(onEvent:)   [tap mode only]
//   Rules / Backend / Owner properties -> state
//
// The shape of the helper is what shapes this protocol: **rules go in, events
// do not come out.** Outside tap mode the helper emits nothing, deliberately —
// "a helper that streamed every event to a session process would have moved
// the keylogger into the unprivileged half and gained nothing". So the core
// declares *what it wants done* and the privileged half does it; only the
// shortcut recorder, for the seconds it is open, asks to see keys.
//
// The macOS adapter honours the same contract over a `CGEventTap`: it
// interprets the same rules itself, and only opens the equivalent of tap mode
// while `ShortcutRecordingTap` is recording.

/// What the interceptor should do, as a document rather than a callback.
///
/// The first five fields are the helper's seven-key schema verbatim
/// (`PRIVILEGES.md` § 4.1); the rest are what the macOS features
/// (`MiddleClickSupport`, `SmoothScrollSupport`, `MouseNavigationSupport`)
/// already do and the Linux relay has not grown yet. `inputCapabilities` says
/// which half of this document the running backend will honour, so a feature
/// that sets a field the backend ignores can say so instead of appearing to
/// work.
public struct InputRules: Equatable, Codable {
    /// Tap-versus-hold on one key (the Super Key feature).
    public var tapHold: Bool
    /// Milliseconds; the helper range-checks this at ≤ 5000.
    public var tapThresholdMilliseconds: Int
    /// Key chatter suppression (`KeyboardDebounceSupport`).
    public var chatter: Bool
    /// Milliseconds.
    public var chatterMilliseconds: Int
    /// evdev keycode of the key that taps, or `nil` for none.
    public var tapSource: Int?
    /// evdev keycode emitted on a tap.
    public var tapOutput: Int?
    /// evdev keycode emitted on a hold.
    public var holdOutput: Int?

    /// Milliseconds within which a second click of the same button is
    /// swallowed (`MouseClickDebounceSupport`). 0 disables.
    public var clickDebounceMilliseconds: Int
    /// Turn a discrete wheel notch into a run of small deltas
    /// (`SmoothScrollSupport`).
    public var smoothScroll: Bool
    /// Buttons to remap, by the 1-based numbering
    /// `MouseButtonShortcutSupport.buttonRange` already uses.
    public var remappedButtons: Set<Int>
    /// `app_id`s the rules must not apply to (`MouseAppExceptionSupport`).
    public var exemptApplicationIDs: Set<String>

    public init(tapHold: Bool = false,
                tapThresholdMilliseconds: Int = 200,
                chatter: Bool = false,
                chatterMilliseconds: Int = 40,
                tapSource: Int? = nil,
                tapOutput: Int? = nil,
                holdOutput: Int? = nil,
                clickDebounceMilliseconds: Int = 0,
                smoothScroll: Bool = false,
                remappedButtons: Set<Int> = [],
                exemptApplicationIDs: Set<String> = []) {
        self.tapHold = tapHold
        self.tapThresholdMilliseconds = tapThresholdMilliseconds
        self.chatter = chatter
        self.chatterMilliseconds = chatterMilliseconds
        self.tapSource = tapSource
        self.tapOutput = tapOutput
        self.holdOutput = holdOutput
        self.clickDebounceMilliseconds = clickDebounceMilliseconds
        self.smoothScroll = smoothScroll
        self.remappedButtons = remappedButtons
        self.exemptApplicationIDs = exemptApplicationIDs
    }
}

/// One entry of the helper's `GetDevices` array.
public struct InputDevice: Equatable, Identifiable {
    public enum Kind: String, Equatable, Codable {
        case keyboard, pointer, touchpad, other
    }

    /// The device node on Linux (`/dev/input/event3`), an IOKit path on macOS.
    public let id: String
    public let name: String
    public let kind: Kind
    /// Whether the relay currently holds an exclusive grab on it. Always
    /// `false` on macOS, where a `CGEventTap` does not grab.
    public let isGrabbed: Bool

    public init(id: String, name: String, kind: Kind, isGrabbed: Bool) {
        self.id = id
        self.name = name
        self.kind = kind
        self.isGrabbed = isGrabbed
    }
}

/// One event, seen only while a recording tap is open.
///
/// The helper's `Event(t timestamp_ns, q type, q code, i value)` carries raw
/// evdev; this is that decoded far enough for `ShortcutRecordingTap` to show
/// the user what they pressed, and no further.
public struct RecordedInput: Equatable {
    public enum Kind: Equatable {
        case keyDown, keyUp
        case buttonDown, buttonUp
    }

    public let kind: Kind
    /// Physical key in the same token spelling `ShortcutBinding` uses, or
    /// `nil` for a button event.
    public let keyToken: String?
    /// 1-based button number, or `nil` for a key event.
    public let button: Int?
    public let modifiers: ShortcutModifiers
    /// Seconds since the reference date, converted from the helper's
    /// nanosecond monotonic stamp.
    public let timestamp: TimeInterval

    public init(kind: Kind, keyToken: String?, button: Int?,
                modifiers: ShortcutModifiers, timestamp: TimeInterval) {
        self.kind = kind
        self.keyToken = keyToken
        self.button = button
        self.modifiers = modifiers
        self.timestamp = timestamp
    }
}

/// Why the interception stopped without being asked to.
public enum InterceptionLoss: Equatable {
    /// Another session claimed the relay
    /// (`org.vorssaint.Helper1.Error.NotOwner`).
    case takenByAnotherSession(String)
    /// macOS disabled a tap that ran too long, or the helper's watchdog fired.
    case disabledByPlatform(String)
    /// The privileged half went away.
    case backendGone(String)
    /// The user or polkit revoked it.
    case denied(String)
}

/// Sees input before the focused application does, and can change it.
///
/// The most privilege-laden protocol in the port. macOS is a `CGEventTap` with
/// the Accessibility permission; Linux has no unprivileged equivalent — it is
/// an evdev grab plus a `uinput` device inside `vorssaint-helper`, gated per
/// method by polkit, and inside a Flatpak sandbox it may be unavailable
/// altogether (`PRIVILEGES.md` § 6). So `inputCapabilities` here is the
/// difference between a feature appearing and a feature explaining itself.
public protocol InputInterceptor: PlatformService {
    /// `GetCapabilities`: answered by *trying*, never by inferring from a
    /// device name — a node can exist with no driver behind it, which is
    /// exactly what `/dev/uinput` did in the WP-03 container.
    var inputCapabilities: InputCapabilities { get }

    /// Which privileged backend is behind this: `"evdev"`, `"fake"`,
    /// `"cgeventtap"`. The helper's `Backend` property.
    var backendName: String { get }

    /// `Enable(b)`: claim or release the devices and start or stop the relay.
    /// This is the call that prompts — `auth_admin_keep` on Linux, the
    /// Accessibility sheet on macOS — so a feature calls it once, when the
    /// user switches the feature on, and never speculatively.
    func setEnabled(_ enabled: Bool) throws

    var isEnabled: Bool { get }

    /// `SetRules(s json)`. Replaces the whole document; the previous rules
    /// stay in force if this one is rejected.
    func setRules(_ rules: InputRules) throws

    var rules: InputRules { get }

    /// `GetDevices`.
    func devices() throws -> [InputDevice]

    /// Tap mode: the relay listens without grabbing and without an output
    /// device, so the shortcut recorder can show the user what they pressed.
    /// **Ends when recording ends**; outside it nothing is emitted at all.
    func startRecordingTap(onEvent: @escaping (RecordedInput) -> Void) throws

    func stopRecordingTap()

    var isRecordingTap: Bool { get }

    /// Called when the platform dropped the interception on its own, so the
    /// feature can re-arm and tell the user why it stopped.
    var onInterceptionLost: ((InterceptionLoss) -> Void)? { get set }
}

/// The decoded form of the helper's `GetCapabilities` JSON.
public struct InputCapabilities: Equatable {
    /// A synthetic input device can be created (`/dev/uinput` opened, or a
    /// `CGEventTap` that may post). Without it nothing can be *changed*, only
    /// observed.
    public let canSynthesize: Bool
    /// Why not, with the errno the helper reports, because `ENOENT` (no node)
    /// and `ENODEV` (a node whose driver is missing) are different problems
    /// and only the first is a packaging mistake.
    public let synthesizeUnavailableReason: String?
    /// Events can be withheld from the focused application.
    public let canSwallow: Bool
    /// The `app_id` an event is going to can be named, for per-app exceptions.
    public let canNameTargetApplication: Bool
    /// Tap mode is available, so a shortcut can be recorded.
    public let canRecordTap: Bool
    /// Number of readable `/dev/input/event*` nodes; `nil` when `/dev/input`
    /// does not exist, which is not the same as zero devices.
    public let eventNodeCount: Int?
    /// A privileged helper must be installed and authorised first, so the
    /// feature must explain a prompt before it can be switched on.
    public let requiresHelper: Bool

    public init(canSynthesize: Bool, synthesizeUnavailableReason: String?,
                canSwallow: Bool, canNameTargetApplication: Bool,
                canRecordTap: Bool, eventNodeCount: Int?, requiresHelper: Bool) {
        self.canSynthesize = canSynthesize
        self.synthesizeUnavailableReason = synthesizeUnavailableReason
        self.canSwallow = canSwallow
        self.canNameTargetApplication = canNameTargetApplication
        self.canRecordTap = canRecordTap
        self.eventNodeCount = eventNodeCount
        self.requiresHelper = requiresHelper
    }
}

public extension InputInterceptor {
    var platformCapabilities: PlatformCapabilitySet {
        var supported: Set<PlatformCapability> = []
        var reasons: [PlatformCapability: String] = [:]
        if inputCapabilities.canSynthesize {
            supported.insert(.inputSynthesize)
        } else if let reason = inputCapabilities.synthesizeUnavailableReason {
            reasons[.inputSynthesize] = reason
        }
        if inputCapabilities.canSwallow { supported.insert(.inputSwallow) }
        if inputCapabilities.canNameTargetApplication { supported.insert(.inputTargetApplication) }
        if inputCapabilities.canRecordTap { supported.insert(.inputRecordTap) }
        if inputCapabilities.requiresHelper { supported.insert(.inputRequiresHelper) }
        return PlatformCapabilitySet(supported: supported, unavailableReasons: reasons)
    }
}

public extension PlatformCapability {
    /// Can emit events the user did not make.
    static let inputSynthesize = PlatformCapability("input.synthesize")
    /// Can stop an event from reaching the focused application. Without it,
    /// debounce and remapping can only add, never remove.
    static let inputSwallow = PlatformCapability("input.swallow")
    /// Can name the application an event is going to.
    static let inputTargetApplication = PlatformCapability("input.targetApplication")
    /// Tap mode, for the shortcut recorder.
    static let inputRecordTap = PlatformCapability("input.recordTap")
    /// Needs a privileged helper, so the feature must explain a prompt first.
    static let inputRequiresHelper = PlatformCapability("input.requiresHelper")
}
