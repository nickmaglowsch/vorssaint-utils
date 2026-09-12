// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import AppKit
import CoreGraphics
import Foundation

// The four remaining macOS Platform implementations, together because each is
// short for the same reason: the behaviour still lives in a service with its
// own state, and this work package does not rewrite services.
//
// They are complete conformances — the protocol compiles against them, the
// Linux side has a shape to match, and `platformCapabilities` names, per
// capability, which service still owns it. That is the honest form of "not yet
// wired": a caller reads `false` and explains itself, instead of calling
// something that quietly does nothing. `docs/linux-port/PLATFORM.md` lists
// which per-feature work package closes each one.

// MARK: - ShortcutRegistrar

/// Registration goes through `HotkeyManager`, which owns the Carbon
/// `EventHotKeyRef` table, the `GlobalShortcut` ↔ Carbon translation and the
/// re-registration on layout change. `ShortcutBinding` is deliberately not
/// `GlobalShortcut` (see its doc comment), so wiring the two is a translation
/// layer, not a wrap — WP-21's shortcut recorder work.
final class MacShortcutRegistrar: ShortcutRegistrar {
    private(set) var activeBindings: [ShortcutHandle: ShortcutBinding] = [:]

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.shortcutDirectBinding, .shortcutLayoutAwareLabels],
            unavailableReasons: [
                .shortcutOverrideSystem: "Carbon refuses a combination the system holds",
                .shortcutPortalBinding: "macOS binds directly; there is no session portal",
            ])
    }

    /// `false`: macOS binds what the app asks for, so the app draws its own
    /// recorder. The XDG GlobalShortcuts portal is the case this exists for.
    let bindingIsChosenBySession = false

    func register(_ binding: ShortcutBinding,
                  identifier: String,
                  onActivate: @escaping () -> Void)
        -> Result<ShortcutHandle, ShortcutRegistrationFailure> {
        .failure(.backendFailure("HotkeyManager owns registration until WP-21 migrates it"))
    }

    func unregister(_ handle: ShortcutHandle) {
        activeBindings[handle] = nil
    }
}

// MARK: - ScreenCapturer

/// Capture goes through `RecorderCaptureEngine` and `ScreenshotCaptureEngine`,
/// which hold the ScreenCaptureKit stream, its configuration and the
/// permission state. `displays()` is real because it is a pure read and the
/// layout feature needs work areas from somewhere.
final class MacScreenCapturer: ScreenCapturer {
    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.captureOwnPicker],
            unavailableReasons: [
                .captureDisplay: "ScreenshotCaptureEngine owns capture until it is migrated",
                .captureWindow: "ScreenshotCaptureEngine owns capture until it is migrated",
                .captureArea: "ScreenshotCaptureEngine owns capture until it is migrated",
                .captureStream: "RecorderCaptureEngine owns the SCStream until it is migrated",
                .captureCursor: "RecorderCaptureEngine owns the SCStream until it is migrated",
                .captureWindowExclusion: "RecorderCaptureEngine owns the SCStream until it is migrated",
                .captureSystemAudio: "RecorderSystemAudioTap owns this",
                .captureRestoreToken: "macOS has no portal restore token",
            ])
    }

    var restoreToken: String? { nil }

    func displays() throws -> [PlatformDisplay] {
        NSScreen.screens.enumerated().map { index, screen in
            let number = screen.deviceDescription[
                NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber
            return PlatformDisplay(
                id: number?.uint32Value ?? PlatformDisplayID(index),
                name: screen.localizedName,
                bounds: screen.frame,
                workArea: screen.visibleFrame,
                scale: screen.backingScaleFactor,
                isPrimary: index == 0)
        }
    }

    func captureFrame(_ target: CaptureTarget) async throws -> CapturedFrame {
        throw PlatformError.unsupported(.captureDisplay)
    }

    func startStream(_ target: CaptureTarget,
                     options: CaptureStreamOptions,
                     onFrame: @escaping (CapturedFrame) -> Void) async throws -> CaptureSession {
        throw PlatformError.unsupported(.captureStream)
    }

    func stop(_ session: CaptureSession) {}

    /// `nil`: macOS lets the app draw its own region picker, which is what
    /// `ScreenshotSelectionController` does. Wayland portals do not, and that
    /// difference is `.captureOwnPicker`.
    func pickTargetInteractively() async throws -> CaptureTarget? { nil }

    func restore(with token: String) async throws {
        throw PlatformError.unsupported(.captureRestoreToken)
    }
}

// MARK: - AudioGraph

/// Audio goes through `AppVolumeMixer` and `MixerRouting`, which hold the
/// CoreAudio tap, the render callback and the boost limiter. Per-application
/// volume on macOS only exists *because* of that tap; on PipeWire it is a node
/// property, which is the one place the Linux backend is simpler.
final class MacAudioGraph: AudioGraph {
    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [],
            unavailableReasons: [
                .audioSinkVolume: "SoundOutputSwitcher owns device volume until it is migrated",
                .audioSourceVolume: "AudioInputDeviceManager owns input volume until it is migrated",
                .audioStreamVolume: "AppVolumeMixer owns the per-app tap until it is migrated",
                .audioStreamRouting: "MixerRouting owns routing until it is migrated",
                .audioBoost: "BoostLimiter owns boost until it is migrated",
                .audioDefaultDeviceSwitch: "SoundOutputSwitcher owns this",
                .audioEvents: "the mixer owns its CoreAudio listeners",
            ])
    }

    func sinks() -> [AudioSink] { [] }
    func sources() -> [AudioSource] { [] }
    func streams() -> [AudioStream] { [] }

    func setVolume(_ volume: Double, ofSink id: String) -> Bool { false }
    func setMuted(_ muted: Bool, ofSink id: String) -> Bool { false }
    func setVolume(_ volume: Double, ofSource id: String) -> Bool { false }
    func setMuted(_ muted: Bool, ofSource id: String) -> Bool { false }
    func setVolume(_ volume: Double, ofStream id: String) -> Bool { false }
    func setMuted(_ muted: Bool, ofStream id: String) -> Bool { false }
    func setDefaultSink(_ id: String) -> Bool { false }
    func setDefaultSource(_ id: String) -> Bool { false }
    func route(stream id: String, toSink sinkID: String) -> Bool { false }

    func observe(_ onEvent: @escaping (AudioGraphEvent) -> Void) -> AudioObservationToken? { nil }
    func stopObserving(_ token: AudioObservationToken) {}
}

// MARK: - InputInterceptor

/// Interception goes through `PointerTapRunLoop` and the per-feature
/// `CGEventTap` installers, which own the tap, its re-enable path and the
/// rules each feature applies inline.
///
/// The capability shape is the interesting part even unwired: macOS needs no
/// helper (`requiresHelper: false`) where Linux always does, and that single
/// difference is what the feature hub has to explain.
final class MacInputInterceptor: InputInterceptor {
    private(set) var isEnabled = false
    private(set) var rules = InputRules()
    private(set) var isRecordingTap = false
    var onInterceptionLost: ((InterceptionLoss) -> Void)?

    let backendName = "cgeventtap"

    var inputCapabilities: InputCapabilities {
        InputCapabilities(canSynthesize: true,
                          synthesizeUnavailableReason: nil,
                          canSwallow: true,
                          canNameTargetApplication: true,
                          canRecordTap: true,
                          eventNodeCount: nil,
                          requiresHelper: false)
    }

    func setEnabled(_ enabled: Bool) throws {
        throw PlatformError.unavailable(
            "the per-feature CGEventTap installers own interception until they are migrated")
    }

    func setRules(_ rules: InputRules) throws {
        throw PlatformError.unavailable(
            "the per-feature CGEventTap installers own interception until they are migrated")
    }

    func devices() throws -> [InputDevice] { [] }

    func startRecordingTap(onEvent: @escaping (RecordedInput) -> Void) throws {
        throw PlatformError.unavailable("ShortcutRecordingTap owns tap mode until it is migrated")
    }

    func stopRecordingTap() { isRecordingTap = false }
}

// MARK: - PackageManager

/// Homebrew is the only package backend macOS has, and `HomebrewSupport` plus
/// its service already parse and run it.
final class MacPackageManager: PackageManager {
    var availableBackends: [PackageBackend] { [] }

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [],
            unavailableReasons: [
                .packagesList: "HomebrewSupport's service owns Homebrew until it is migrated",
                .packagesRefresh: "HomebrewSupport's service owns Homebrew until it is migrated",
                .packagesUpgrade: "HomebrewSupport's service owns Homebrew until it is migrated",
                .packagesUninstall: "HomebrewSupport's service owns Homebrew until it is migrated",
                .packagesCleanUp: "HomebrewSupport's service owns Homebrew until it is migrated",
            ])
    }

    func installed(from backend: PackageBackend) async throws -> [Package] {
        throw PlatformError.unsupported(.packagesList)
    }

    func refresh(_ backend: PackageBackend) async throws {
        throw PlatformError.unsupported(.packagesRefresh)
    }

    func outdated(from backend: PackageBackend) async throws -> [Package] {
        throw PlatformError.unsupported(.packagesList)
    }

    func upgrade(_ id: String, from backend: PackageBackend) async throws {
        throw PlatformError.unsupported(.packagesUpgrade)
    }

    func uninstall(_ id: String, from backend: PackageBackend) async throws {
        throw PlatformError.unsupported(.packagesUninstall)
    }

    func reclaimableBytes(from backend: PackageBackend) async throws -> UInt64 { 0 }

    func cleanUp(_ backend: PackageBackend) async throws {
        throw PlatformError.unsupported(.packagesCleanUp)
    }
}
#endif
