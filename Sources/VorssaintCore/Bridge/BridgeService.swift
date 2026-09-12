// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// What a core service must provide to be rendered by the Linux shell
/// (`PLAN.md` § 4.2, `docs/linux-port/BRIDGE.md`).
///
/// The whole point of the bridge is that a feature squad writes *no*
/// view-specific binding: it declares the state its SwiftUI views already
/// observe as a `Snapshot`, the calls those views already make as a `Command`,
/// and the generic `CoreModel` on the Qt side renders both. Nothing here knows
/// that Qt exists.
///
/// **Main thread only, like the services it wraps.** `bridgeSnapshot()` and
/// `apply(_:)` run on the thread that called into `CoreBridge`, which for the
/// shell is the GUI thread — the same thread the core's services already
/// require. Only the *delivery* of a snapshot may happen elsewhere; see
/// `CoreBridge`'s threading contract.
public protocol BridgeService: AnyObject {
    /// The service's published state. `Equatable` is not what the diff uses —
    /// the diff is over the encoded bytes, which is the thing actually
    /// delivered — but requiring it keeps a snapshot an honest value type and
    /// lets a test say what it means.
    associatedtype Snapshot: Codable & Equatable
    /// What the views call, as a closed enum. See `BRIDGE.md` § "The command
    /// wire format" for the single-key JSON shape every one of these uses.
    associatedtype Command: Codable

    /// Stable id. It is the string `vs_subscribe("metrics", …)` names and the
    /// one `CoreModel { service: "metrics" }` writes in QML, so it is part of
    /// the shell's source and is renamed only together with it.
    static var bridgeID: String { get }

    /// The service's state right now. Called on every publish, so it must be
    /// cheap: read the `@Published` properties, never sample hardware.
    func bridgeSnapshot() -> Snapshot

    /// Apply one command, or throw to refuse it. A refusal reaches the shell
    /// as `vs_command() == -2`; it is not something the shell can repair, so a
    /// service that refuses should also publish a snapshot that explains why.
    func apply(_ command: Command) throws
}

public extension BridgeService {
    /// Instance-side spelling of `Self.bridgeID`, so a service can publish
    /// itself without naming its own type.
    var bridgeID: String { Self.bridgeID }

    /// Re-read this service's state and deliver it to every subscriber, unless
    /// the encoded bytes are the ones they already have. Call it at the end of
    /// anything that mutates.
    func publishToBridge(_ bridge: CoreBridge = .shared) {
        bridge.publish(Self.bridgeID)
    }
}

/// Everything the bridge can refuse, and the `vs_command`/`vs_subscribe`
/// status it becomes at the C boundary.
///
/// The mapping *is* the ABI: `linux/shell/include/corebridge.h` documents
/// these numbers and Qt's `CoreModel::invoke` returns them straight to QML.
public enum BridgeError: Error, Equatable {
    /// No service is registered under that id. C: `-1`.
    case unknownService(String)
    /// The service rejected the command. C: `-2`.
    case rejected(service: String, reason: String)
    /// The JSON did not decode into the service's `Command`. C: `-3`.
    /// A shell bug, not a refusal anyone can act on.
    case malformedCommand(service: String, reason: String)
    /// The service's own `Snapshot` failed to encode. C: `-2` from
    /// `vs_command`, `NULL` from `vs_snapshot`. Only reachable from a snapshot
    /// that is `Codable` in name only — a non-string-keyed dictionary, a bare
    /// `Double.nan`.
    case snapshotEncodingFailed(service: String, reason: String)

    /// The `int` this error is at the C boundary.
    public var cStatus: Int32 {
        switch self {
        case .unknownService: return -1
        case .rejected: return -2
        case .malformedCommand: return -3
        case .snapshotEncodingFailed: return -2
        }
    }
}
