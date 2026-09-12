// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation
import IOKit.pwr_mgt

/// The macOS `PowerControl`: IOKit power assertions, which is what
/// `KeepAwakeManager` already takes.
///
/// Brightness and fan control are declared *absent* here rather than
/// reimplemented. Both live inside services with their own state — the DDC
/// retry ladder in the brightness service, the XPC handshake and watchdog in
/// `FanControlXPC` — and wrapping them would mean either duplicating that
/// state or rewriting the service, which is the per-feature migration this
/// work package does not do. The capability set says so with a reason, which
/// is the honest form of "not yet": a caller sees `false` and explains itself
/// instead of calling something that silently does nothing.
final class MacPowerControl: PowerControl {
    private var assertions: [PowerAssertion: (SleepInhibition, IOPMAssertionID)] = [:]
    private var nextAssertion: UInt64 = 1

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.powerInhibitSystemSleep, .powerInhibitDisplaySleep],
            unavailableReasons: [
                .powerInhibitLidClose: "macOS has no supported lid-close assertion",
                .powerInternalBrightness: "stays in the brightness service until it is migrated",
                .powerExternalBrightness: "stays in the brightness service until it is migrated",
                .powerExtraDimming: "stays in ExtraBrightnessService until it is migrated",
                .powerFanControl: "stays behind FanControlXPC until it is migrated",
                .powerSleepNow: "not wired",
                .powerLockSession: "not wired",
            ])
    }

    var activeAssertions: [PowerAssertion: SleepInhibition] {
        assertions.mapValues(\.0)
    }

    func inhibit(_ what: SleepInhibition, reason: String) throws -> PowerAssertion {
        guard !what.contains(.lidClose) else {
            throw PlatformError.unsupported(.powerInhibitLidClose)
        }
        let type = what.contains(.displaySleep)
            ? kIOPMAssertionTypeNoDisplaySleep as CFString
            : kIOPMAssertionTypePreventUserIdleSystemSleep as CFString
        var id: IOPMAssertionID = 0
        let status = IOPMAssertionCreateWithName(type,
                                                 IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                                 reason as CFString,
                                                 &id)
        guard status == kIOReturnSuccess else {
            throw PlatformError.backendFailure("IOPMAssertionCreateWithName: \(status)")
        }
        let assertion = PowerAssertion(rawValue: nextAssertion)
        nextAssertion += 1
        assertions[assertion] = (what, id)
        return assertion
    }

    func release(_ assertion: PowerAssertion) {
        guard let (_, id) = assertions.removeValue(forKey: assertion) else { return }
        IOPMAssertionRelease(id)
    }

    func brightnessTargets() -> [BrightnessTarget] { [] }

    func setBrightness(_ level: Double, of display: PlatformDisplayID) -> Bool { false }

    func setFanSpeed(rpm: Int, fanID: String) throws {
        throw PlatformError.unsupported(.powerFanControl)
    }

    func restoreAutomaticFanControl() throws {
        throw PlatformError.unsupported(.powerFanControl)
    }

    func sleepNow() throws { throw PlatformError.unsupported(.powerSleepNow) }

    func lockSession() throws { throw PlatformError.unsupported(.powerLockSession) }
}
#endif
