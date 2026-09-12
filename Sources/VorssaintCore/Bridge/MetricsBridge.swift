// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// One metric family, as the panel's sparkline renders it.
///
/// The keys are `cpu` and `history` because that is what the WP-01 spike's
/// `Panel.qml` already binds (`metrics.state.cpu`,
/// `metrics.state.history`) against the C stub. Keeping them means the QML
/// that was measured under Xvfb and headless sway renders the real core with
/// no edit at all, which is the cheapest possible proof that the ABI swap
/// worked.
///
/// `history` is oldest-first and at most `MetricsBridgeService.historyLength`
/// (60) samples, which is the shape `spikes/wp01-toolkit/corebridge-stub/
/// corebridge.c` produces and the sparkline's x-axis assumes.
public struct MetricsSnapshot: Codable, Equatable {
    /// Most recent sample, 0…100. `0` before the first sample, not `nil`:
    /// the sparkline has to draw something on the first frame.
    public let cpu: Double
    /// Up to 60 samples, oldest first.
    public let history: [Double]
    /// How many samples the ring buffer holds when it is full, so a view can
    /// scale its x-axis without hard-coding 60.
    public let capacity: Int
    /// Where the numbers come from. `"placeholder"` until WP-A1 wires
    /// `SystemSensors`; the panel shows a badge for anything but `"sensors"`,
    /// which is how a probe series never gets mistaken for a measurement.
    public let source: String

    public init(cpu: Double, history: [Double], capacity: Int, source: String) {
        self.cpu = cpu
        self.history = history
        self.capacity = capacity
        self.source = source
    }
}

/// What the panel's monitor card calls.
public enum MetricsCommand: Equatable {
    /// Throw the history away — the panel's "reset" affordance, and what the
    /// shell does when it resumes from sleep and the series is meaningless.
    case reset
    /// Push one sample. It exists so the shell and the tests have a way to
    /// drive the series before WP-A1 owns the sampling; WP-A1 removes it.
    case sample(Double)
}

/// The metrics placeholder WP-A1 replaces.
///
/// It owns the ring buffer and the snapshot shape, and nothing else: there is
/// no sampler here and no `/proc` read, because `SystemSensors`
/// (`PLATFORM.md` § 2) is where that belongs and inventing a second reader of
/// the same hardware is exactly what `PLATFORM.md` § 4 says not to do. WP-A1
/// keeps this type and its snapshot, deletes `MetricsCommand.sample`, and
/// calls `record(_:)` from the sensor's cadence.
public final class MetricsBridgeService: BridgeService {
    public static let bridgeID = "metrics"
    /// 60 samples. The stub's `VS_HISTORY`, the sparkline's x-axis, and — at
    /// the monitor panel's 500 ms cadence — thirty seconds of history.
    public static let historyLength = 60

    private let bridge: CoreBridge
    private let capacity: Int
    private let source: String
    private var history: [Double] = []

    public init(bridge: CoreBridge = .shared,
                capacity: Int = MetricsBridgeService.historyLength,
                source: String = "placeholder") {
        self.bridge = bridge
        self.capacity = max(1, capacity)
        self.source = source
    }

    /// Append one sample and publish. Clamped to 0…100 here rather than at the
    /// call site, because a sparkline that has to defend itself against a
    /// sensor's out-of-range reading is a sparkline in every view.
    public func record(_ sample: Double) {
        let clamped = sample.isFinite ? min(max(sample, 0), 100) : 0
        if history.count == capacity {
            history.removeFirst()
        }
        history.append(clamped)
        publishToBridge(bridge)
    }

    public func bridgeSnapshot() -> MetricsSnapshot {
        MetricsSnapshot(cpu: history.last ?? 0,
                        history: history,
                        capacity: capacity,
                        source: source)
    }

    public func apply(_ command: MetricsCommand) throws {
        switch command {
        case .reset:
            guard !history.isEmpty else { return }
            history.removeAll(keepingCapacity: true)
            publishToBridge(bridge)
        case .sample(let value):
            guard value.isFinite else {
                throw BridgeError.rejected(service: Self.bridgeID,
                                           reason: "sample is not a finite number")
            }
            record(value)
        }
    }
}

// MARK: - Wire format

extension MetricsCommand: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: BridgeCommandKey.self)
        let key = try BridgeCommandCoding.singleKey(container, in: decoder)
        switch key.stringValue {
        case "reset": _ = try container.decode(Bool.self, forKey: key); self = .reset
        case "sample": self = .sample(try container.decode(Double.self, forKey: key))
        default: throw BridgeCommandCoding.unknownCase(key, decoder)
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: BridgeCommandKey.self)
        switch self {
        case .reset: try container.encode(true, forKey: BridgeCommandKey("reset"))
        case .sample(let value): try container.encode(value, forKey: BridgeCommandKey("sample"))
        }
    }
}
