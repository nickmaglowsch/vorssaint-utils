// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

#if os(macOS)
import Foundation

/// The macOS `SystemSensors`.
///
/// The part that matters for the port is `thermalPressure`: this is where
/// `ProcessInfo.thermalState` lives now. That property is Darwin-only —
/// the third of the four Foundation gaps WP-00 § 8 condition 3 named — and
/// `FanControlSupport` is its only reader. Behind this protocol,
/// `FanControlPolicy` never learns which OS it is on: Linux derives the same
/// four levels from hwmon `temp*_crit` / `temp*_max` trip points.
///
/// The sampling members are declared absent with reasons rather than
/// reimplemented. `SystemMonitor`, `SMCClient`, `VMStatisticsDecoder`,
/// `PowerSampler` and `NetworkProcessSupport` each own a sampler with its own
/// cadence, smoothing and cached state (`MonitorSamplingPolicy` exists
/// precisely to hold that), and a second reader of the same hardware would
/// either duplicate that state or disturb it. Moving them is per-feature work;
/// an honest `false` here is what lets a caller say so.
final class MacSystemSensors: SystemSensors {
    private var observers: [SensorObservationToken: (ThermalPressure) -> Void] = [:]
    private var nextToken: UInt64 = 1
    private var thermalObserver: NSObjectProtocol?

    deinit {
        if let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
        }
    }

    var platformCapabilities: PlatformCapabilitySet {
        PlatformCapabilitySet(
            supported: [.sensorsThermalPressure],
            unavailableReasons: [
                .sensorsCPU: "SystemMonitor owns the CPU sampler until it is migrated",
                .sensorsPerCoreCPU: "SystemMonitor owns the CPU sampler until it is migrated",
                .sensorsMemory: "VMStatisticsDecoder owns the memory sampler until it is migrated",
                .sensorsMemoryPressure: "VMStatisticsDecoder owns the memory sampler until it is migrated",
                .sensorsTemperature: "SMCClient owns the SMC session until it is migrated",
                .sensorsFanSpeed: "SMCClient owns the SMC session until it is migrated",
                .sensorsBattery: "PowerSampler owns the battery sampler until it is migrated",
                .sensorsBatteryHealth: "PowerSampler owns the battery sampler until it is migrated",
                .sensorsPeripheralBatteries: "PeripheralBatterySupport's service owns this",
                .sensorsNetwork: "NetworkProcessSupport's service owns this",
                .sensorsDiskActivity: "the disk sampler owns this",
            ])
    }

    var thermalPressure: ThermalPressure {
        Self.pressure(from: ProcessInfo.processInfo.thermalState)
    }

    /// The one mapping. `ProcessInfo.ThermalState` has exactly these four
    /// cases and `ThermalPressure` mirrors them in the same order, so this is
    /// a rename and not a judgement.
    static func pressure(from state: ProcessInfo.ThermalState) -> ThermalPressure {
        switch state {
        case .nominal: return .nominal
        case .fair: return .fair
        case .serious: return .serious
        case .critical: return .critical
        @unknown default: return .nominal
        }
    }

    func cpu() -> CPUSample? { nil }
    func memory() -> MemorySample? { nil }
    func temperatures() -> [TemperatureSample] { [] }
    func fans() -> [FanSample] { [] }
    func battery() -> BatterySample? { nil }
    func peripheralBatteries() -> [BatterySample] { [] }
    func network() -> [NetworkSample] { [] }
    func disks() -> [DiskSample] { [] }

    func observeThermalPressure(
        _ onChange: @escaping (ThermalPressure) -> Void) -> SensorObservationToken? {
        let token = SensorObservationToken(rawValue: nextToken)
        nextToken += 1
        observers[token] = onChange
        if thermalObserver == nil {
            thermalObserver = NotificationCenter.default.addObserver(
                forName: ProcessInfo.thermalStateDidChangeNotification,
                object: nil, queue: .main) { [weak self] _ in
                guard let self else { return }
                let pressure = self.thermalPressure
                for observer in self.observers.values { observer(pressure) }
            }
        }
        return token
    }

    func stopObserving(_ token: SensorObservationToken) {
        observers[token] = nil
        if observers.isEmpty, let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
            self.thermalObserver = nil
        }
    }
}
#endif
