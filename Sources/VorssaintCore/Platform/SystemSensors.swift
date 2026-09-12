// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// CPU load, as the menu-bar metric and the monitor panel read it.
public struct CPUSample: Equatable {
    /// 0...1 over all cores.
    public let totalUsage: Double
    public let perCoreUsage: [Double]
    public let userUsage: Double
    public let systemUsage: Double
    /// Seconds since the reference date.
    public let sampledAt: TimeInterval

    public init(totalUsage: Double, perCoreUsage: [Double], userUsage: Double,
                systemUsage: Double, sampledAt: TimeInterval) {
        self.totalUsage = totalUsage
        self.perCoreUsage = perCoreUsage
        self.userUsage = userUsage
        self.systemUsage = systemUsage
        self.sampledAt = sampledAt
    }
}

/// Memory, in the breakdown `MonitorMemoryMetric` already renders.
public struct MemorySample: Equatable {
    public let totalBytes: UInt64
    public let usedBytes: UInt64
    /// macOS's "app memory"; `/proc/meminfo`'s `AnonPages` on Linux — the
    /// memory processes have actually allocated, as against `usedBytes`,
    /// which is `MemTotal - MemAvailable` and includes what the kernel would
    /// have to reclaim. Named for what it means rather than for either source.
    public let activeBytes: UInt64
    public let cachedBytes: UInt64
    public let swapUsedBytes: UInt64
    /// macOS memory pressure, 0...1. On Linux this is the PSI stall fraction
    /// (`/proc/pressure/memory`, `some avg10`) where the kernel reports one,
    /// and a fill level derived from `MemAvailable` where it does not. The
    /// two are different quantities, not two estimates of one: the capability
    /// flag says which arrived, and a panel must not label a fill level as
    /// pressure.
    public let pressure: Double?

    public init(totalBytes: UInt64, usedBytes: UInt64, activeBytes: UInt64,
                cachedBytes: UInt64, swapUsedBytes: UInt64, pressure: Double?) {
        self.totalBytes = totalBytes
        self.usedBytes = usedBytes
        self.activeBytes = activeBytes
        self.cachedBytes = cachedBytes
        self.swapUsedBytes = swapUsedBytes
        self.pressure = pressure
    }
}

/// One temperature reading, named the way the sensor names itself (an SMC key
/// on macOS, an hwmon label on Linux) so `TemperatureSensorSelector`'s
/// preference survives.
public struct TemperatureSample: Equatable, Identifiable {
    public let id: String
    public let label: String
    public let celsius: Double
    /// True for the reading the platform considers the package/die
    /// temperature, which is what the badge shows by default.
    public let isPrimary: Bool

    public init(id: String, label: String, celsius: Double, isPrimary: Bool) {
        self.id = id
        self.label = label
        self.celsius = celsius
        self.isPrimary = isPrimary
    }
}

public struct FanSample: Equatable, Identifiable {
    public let id: String
    public let label: String
    public let rpm: Int
    public let minimumRPM: Int?
    public let maximumRPM: Int?
    /// Whether this fan can be driven, not merely read. Fan *control* is a
    /// privileged path (`docs/linux-port/PRIVILEGES.md`); reading is not.
    public let isControllable: Bool

    public init(id: String, label: String, rpm: Int, minimumRPM: Int?,
                maximumRPM: Int?, isControllable: Bool) {
        self.id = id
        self.label = label
        self.rpm = rpm
        self.minimumRPM = minimumRPM
        self.maximumRPM = maximumRPM
        self.isControllable = isControllable
    }
}

public struct BatterySample: Equatable {
    public let percentage: Double
    public let isCharging: Bool
    public let isPluggedIn: Bool
    /// Seconds, when the platform estimates one.
    public let timeToEmpty: TimeInterval?
    public let timeToFull: TimeInterval?
    public let cycleCount: Int?
    /// Present capacity over design capacity, 0...1.
    public let health: Double?
    public let temperatureCelsius: Double?

    public init(percentage: Double, isCharging: Bool, isPluggedIn: Bool,
                timeToEmpty: TimeInterval?, timeToFull: TimeInterval?,
                cycleCount: Int?, health: Double?, temperatureCelsius: Double?) {
        self.percentage = percentage
        self.isCharging = isCharging
        self.isPluggedIn = isPluggedIn
        self.timeToEmpty = timeToEmpty
        self.timeToFull = timeToFull
        self.cycleCount = cycleCount
        self.health = health
        self.temperatureCelsius = temperatureCelsius
    }
}

public struct NetworkSample: Equatable {
    public let interfaceName: String
    public let bytesInPerSecond: Double
    public let bytesOutPerSecond: Double
    public let totalBytesIn: UInt64
    public let totalBytesOut: UInt64

    public init(interfaceName: String, bytesInPerSecond: Double,
                bytesOutPerSecond: Double, totalBytesIn: UInt64, totalBytesOut: UInt64) {
        self.interfaceName = interfaceName
        self.bytesInPerSecond = bytesInPerSecond
        self.bytesOutPerSecond = bytesOutPerSecond
        self.totalBytesIn = totalBytesIn
        self.totalBytesOut = totalBytesOut
    }
}

public struct DiskSample: Equatable, Identifiable {
    public let id: String
    public let mountPoint: String
    public let totalBytes: UInt64
    public let freeBytes: UInt64
    public let readBytesPerSecond: Double
    public let writeBytesPerSecond: Double

    public init(id: String, mountPoint: String, totalBytes: UInt64, freeBytes: UInt64,
                readBytesPerSecond: Double, writeBytesPerSecond: Double) {
        self.id = id
        self.mountPoint = mountPoint
        self.totalBytes = totalBytes
        self.freeBytes = freeBytes
        self.readBytesPerSecond = readBytesPerSecond
        self.writeBytesPerSecond = writeBytesPerSecond
    }
}

/// How hot the machine says it is.
///
/// This is `ProcessInfo.ThermalState`'s home in the port: that property is
/// Darwin-only (`docs/linux-port/PLAN.md` § 4.1, WP-00 condition 3), and
/// `FanControlSupport` is its only reader. Linux derives the same four levels
/// from hwmon `temp*_crit`/`temp*_max` trip points, so the fan-control policy
/// logic never learns which OS it is on.
public enum ThermalPressure: Int, Equatable, Codable, CaseIterable {
    case nominal
    case fair
    case serious
    case critical
}

/// Everything the monitor, fan-control and battery features read.
///
/// macOS is IOKit, `host_statistics64`, SMC and `sysctl`; Linux is `/proc`,
/// `/sys/class/hwmon`, `/sys/class/power_supply` and UPower over D-Bus. All of
/// it is readable unprivileged except fan *writing*, which is why only that
/// lives in `PowerControl` behind the helper.
public protocol SystemSensors: PlatformService {
    func cpu() -> CPUSample?
    func memory() -> MemorySample?
    func temperatures() -> [TemperatureSample]
    func fans() -> [FanSample]
    func battery() -> BatterySample?
    /// Every battery that is not the machine's own: a mouse, a keyboard, a
    /// headset. `PeripheralBatterySupport` already models these.
    func peripheralBatteries() -> [BatterySample]
    func network() -> [NetworkSample]
    func disks() -> [DiskSample]

    /// The `ProcessInfo.ThermalState` replacement.
    var thermalPressure: ThermalPressure { get }

    /// Fires when `thermalPressure` changes, so `FanControlPolicy` need not
    /// poll. `nil` where the platform only polls.
    func observeThermalPressure(_ onChange: @escaping (ThermalPressure) -> Void) -> SensorObservationToken?
    func stopObserving(_ token: SensorObservationToken)
}

public struct SensorObservationToken: Hashable {
    public let rawValue: UInt64
    public init(rawValue: UInt64) { self.rawValue = rawValue }
}

public extension PlatformCapability {
    static let sensorsCPU = PlatformCapability("sensors.cpu")
    static let sensorsPerCoreCPU = PlatformCapability("sensors.perCoreCPU")
    static let sensorsMemory = PlatformCapability("sensors.memory")
    static let sensorsMemoryPressure = PlatformCapability("sensors.memoryPressure")
    static let sensorsTemperature = PlatformCapability("sensors.temperature")
    static let sensorsFanSpeed = PlatformCapability("sensors.fanSpeed")
    static let sensorsBattery = PlatformCapability("sensors.battery")
    static let sensorsBatteryHealth = PlatformCapability("sensors.batteryHealth")
    static let sensorsPeripheralBatteries = PlatformCapability("sensors.peripheralBatteries")
    static let sensorsNetwork = PlatformCapability("sensors.network")
    static let sensorsDiskActivity = PlatformCapability("sensors.diskActivity")
    static let sensorsThermalPressure = PlatformCapability("sensors.thermalPressure")
}
