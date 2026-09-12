// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// What a keep-awake assertion is holding off.
public struct SleepInhibition: OptionSet, Hashable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }

    /// The machine must not suspend. macOS `kIOPMAssertionTypePreventSystemSleep`,
    /// logind `sleep`/`idle` inhibitor, portal `Inhibit` `SUSPEND`.
    public static let systemSleep = SleepInhibition(rawValue: 1 << 0)
    /// The screen must not blank. macOS `kIOPMAssertionTypeNoDisplaySleep`,
    /// portal `Inhibit` `IDLE`, or the screensaver D-Bus inhibitor.
    public static let displaySleep = SleepInhibition(rawValue: 1 << 1)
    /// Closing the lid must not suspend. logind `handle-lid-switch`; macOS has
    /// no supported equivalent, which is what the capability flag records.
    public static let lidClose = SleepInhibition(rawValue: 1 << 2)
}

/// One display's backlight.
public struct BrightnessTarget: Equatable, Identifiable {
    public let id: PlatformDisplayID
    public let name: String
    /// 0...1.
    public let level: Double
    /// `true` when the level is written over DDC/CI to an external monitor
    /// rather than to a built-in backlight — the slow, sometimes-unreliable
    /// path the app already warns about.
    public let isExternal: Bool
    public let isWritable: Bool

    public init(id: PlatformDisplayID, name: String, level: Double,
                isExternal: Bool, isWritable: Bool) {
        self.id = id
        self.name = name
        self.level = level
        self.isExternal = isExternal
        self.isWritable = isWritable
    }
}

/// Sleep inhibition, lid behaviour and backlight.
///
/// macOS is IOKit power assertions plus `CoreDisplay`/DDC; Linux is logind's
/// `Inhibit` (or the portal's, inside a sandbox) plus
/// `/sys/class/backlight` and `ddcutil` for external monitors — the one
/// optional system tool `PLAN.md` § 7 lets a feature degrade without.
///
/// Fan *writing* also lives here rather than in `SystemSensors`, because it is
/// the privileged half: on Linux it goes through `vorssaint-helper`
/// (`docs/linux-port/PRIVILEGES.md`), on macOS through the existing
/// `FanControlHelper` XPC service. Reading fans stays unprivileged in
/// `SystemSensors`.
public protocol PowerControl: PlatformService {
    /// Takes an assertion. Held until `release` — the returned handle is the
    /// only way to drop it, so a crashed feature cannot leak one silently
    /// (both platforms drop assertions when the process exits).
    func inhibit(_ what: SleepInhibition, reason: String) throws -> PowerAssertion

    func release(_ assertion: PowerAssertion)

    /// Everything this process currently holds, for the keep-awake status row.
    var activeAssertions: [PowerAssertion: SleepInhibition] { get }

    func brightnessTargets() -> [BrightnessTarget]

    /// `true` only after reading the level back, because DDC writes report
    /// success and do nothing far more often than anything else in this app.
    @discardableResult func setBrightness(_ level: Double, of display: PlatformDisplayID) -> Bool

    /// Drives a fan. Privileged; `PlatformError.denied` when the helper is not
    /// installed or the user declined the polkit prompt.
    func setFanSpeed(rpm: Int, fanID: String) throws

    /// Hands the fan back to the firmware's own curve.
    func restoreAutomaticFanControl() throws

    func sleepNow() throws
    func lockSession() throws
}

public struct PowerAssertion: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    static let powerInhibitSystemSleep = PlatformCapability("power.inhibitSystemSleep")
    static let powerInhibitDisplaySleep = PlatformCapability("power.inhibitDisplaySleep")
    /// Can keep the machine awake with the lid shut. logind can; macOS
    /// cannot without an unsupported hack, so the feature is Linux-only.
    static let powerInhibitLidClose = PlatformCapability("power.inhibitLidClose")
    static let powerInternalBrightness = PlatformCapability("power.internalBrightness")
    /// External monitor brightness over DDC/CI. Needs `/dev/i2c-*` access on
    /// Linux, which is a udev-rule item.
    static let powerExternalBrightness = PlatformCapability("power.externalBrightness")
    /// Software dimming below the backlight's own minimum.
    static let powerExtraDimming = PlatformCapability("power.extraDimming")
    static let powerFanControl = PlatformCapability("power.fanControl")
    static let powerSleepNow = PlatformCapability("power.sleepNow")
    static let powerLockSession = PlatformCapability("power.lockSession")
}
