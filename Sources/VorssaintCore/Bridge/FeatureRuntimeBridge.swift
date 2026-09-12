// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// Where "installed" lives.
///
/// The macOS `FeatureRuntime` (`Sources/Vorssaint/App/FeatureRuntime.swift`)
/// is AppKit code that also *runs each feature's binding* when availability
/// flips — it imports AppKit, touches `NSApp` and names thirty service
/// singletons, so it cannot move into the core and this bridge cannot call it
/// directly. What it and the Linux hub share is the persisted answer, and that
/// is this protocol: the same `featureAvailable.<id>` defaults keys
/// (`DefaultsKey.featureAvailable`), read and written the same way.
///
/// WP-15/WP-23 close the gap from the other end: when `FeatureCatalog` moves
/// into the core, `FeatureRuntime` adopts this protocol and the default
/// implementation below goes away. Until then the Linux hub drives the
/// defaults and the shell's own bindings, which is what it would do anyway —
/// `FeatureRuntime.bindings` is a table of macOS singletons.
public protocol FeatureAvailabilityStore: AnyObject {
    /// Every feature id the catalog knows, in the order the hub lists them.
    var featureIDs: [String] { get }
    /// Installed right now.
    func isInstalled(_ id: String) -> Bool
    /// Could be installed on this machine. `FeatureRuntime.mayFlip` refuses an
    /// install the hardware cannot support and never refuses an uninstall;
    /// that asymmetry is preserved here, in `apply`.
    func isInstallable(_ id: String) -> Bool
    /// Persist, and run whatever the platform does on a flip.
    func setInstalled(_ id: String, _ installed: Bool)
}

/// The defaults-backed store: the persisted half of `FeatureRuntime`, with no
/// bindings behind it. It is what the Linux shell uses until WP-15 lands.
public final class DefaultsFeatureAvailabilityStore: FeatureAvailabilityStore {
    public let featureIDs: [String]
    private let defaults: UserDefaults
    /// Features this machine cannot run. Empty on Linux today: nothing probes
    /// hardware until WP-A1, and claiming a feature is uninstallable without
    /// having asked would be the "plausible-looking second implementation"
    /// `PLATFORM.md` § 4 warns about.
    private let unsupported: Set<String>

    public init(featureIDs: [String],
                unsupported: Set<String> = [],
                defaults: UserDefaults = .standard) {
        self.featureIDs = featureIDs
        self.unsupported = unsupported
        self.defaults = defaults
    }

    public func isInstalled(_ id: String) -> Bool {
        defaults.bool(forKey: DefaultsKey.featureAvailable(id))
    }

    public func isInstallable(_ id: String) -> Bool {
        !unsupported.contains(id)
    }

    public func setInstalled(_ id: String, _ installed: Bool) {
        defaults.set(installed, forKey: DefaultsKey.featureAvailable(id))
    }
}

/// The hub's state, as the Features page renders it.
public struct FeatureRuntimeSnapshot: Codable, Equatable {
    public struct Feature: Codable, Equatable {
        public let id: String
        public let installed: Bool
        public let installable: Bool

        public init(id: String, installed: Bool, installable: Bool) {
            self.id = id
            self.installed = installed
            self.installable = installable
        }
    }

    public let features: [Feature]
    public let installedCount: Int
    /// How many this machine can end up with — the catalog minus what the
    /// hardware refuses, plus anything already installed. `FeatureRuntime`
    /// counts it this way so the install-all button cannot sit one short of
    /// its own disabled condition; the tally can never read more installed
    /// than installable.
    public let installableCount: Int
    /// True while something that loaded this session is now uninstalled, so a
    /// restart would actually unload it. The hub's restart banner keys off it.
    public let needsRestartToUnload: Bool
    /// Bumped on every change, so a view that cannot diff can still tell that
    /// something moved.
    public let revision: Int

    public init(features: [Feature], installedCount: Int, installableCount: Int,
                needsRestartToUnload: Bool, revision: Int) {
        self.features = features
        self.installedCount = installedCount
        self.installableCount = installableCount
        self.needsRestartToUnload = needsRestartToUnload
        self.revision = revision
    }
}

/// What the Features page calls.
public enum FeatureRuntimeCommand: Equatable {
    case install(String)
    case uninstall(String)
    case installAll
    case uninstallAll
}

public final class FeatureRuntimeBridgeService: BridgeService {
    public static let bridgeID = "featureRuntime"

    private let store: FeatureAvailabilityStore
    private let bridge: CoreBridge
    private var revision = 0
    /// Everything that came to life in *this* process: installed at launch or
    /// installed later in the session. A feature uninstalled mid-session stops
    /// working at once, but its inert singleton only leaves memory on the next
    /// launch — which is what the restart banner is about, including the
    /// install-then-uninstall-again case.
    private var loadedThisSession: Set<String>

    public init(store: FeatureAvailabilityStore, bridge: CoreBridge = .shared) {
        self.store = store
        self.bridge = bridge
        loadedThisSession = Set(store.featureIDs.filter(store.isInstalled))
    }

    public func bridgeSnapshot() -> FeatureRuntimeSnapshot {
        let features = store.featureIDs.map {
            FeatureRuntimeSnapshot.Feature(id: $0,
                                           installed: store.isInstalled($0),
                                           installable: store.isInstallable($0))
        }
        return FeatureRuntimeSnapshot(
            features: features,
            installedCount: features.filter(\.installed).count,
            installableCount: features.filter { $0.installable || $0.installed }.count,
            needsRestartToUnload: loadedThisSession.contains { !store.isInstalled($0) },
            revision: revision)
    }

    public func apply(_ command: FeatureRuntimeCommand) throws {
        switch command {
        case .install(let id):
            try setInstalled(id, true)
        case .uninstall(let id):
            try setInstalled(id, false)
        case .installAll:
            for id in store.featureIDs where mayFlip(id, to: true) { flip(id, true) }
        case .uninstallAll:
            for id in store.featureIDs where mayFlip(id, to: false) { flip(id, false) }
        }
        revision += 1
        publishToBridge(bridge)
    }

    private func setInstalled(_ id: String, _ installed: Bool) throws {
        guard store.featureIDs.contains(id) else {
            throw BridgeError.rejected(service: Self.bridgeID, reason: "no feature \"\(id)\"")
        }
        // An install the machine cannot support is refused out loud rather
        // than silently ignored: the hub row is what has to explain itself.
        guard installed == false || store.isInstallable(id) else {
            throw BridgeError.rejected(service: Self.bridgeID,
                                       reason: "\"\(id)\" is not supported here")
        }
        guard mayFlip(id, to: installed) else { return }
        flip(id, installed)
    }

    /// The one gate every install passes, whichever surface asks. Uninstalls
    /// are never refused and an existing install is never revoked: the
    /// hardware check can be wrong, and a wrong answer that strands someone's
    /// settings costs far more than one that leaves a feature calling itself
    /// unsupported.
    private func mayFlip(_ id: String, to installed: Bool) -> Bool {
        guard store.isInstalled(id) != installed else { return false }
        return !installed || store.isInstallable(id)
    }

    private func flip(_ id: String, _ installed: Bool) {
        store.setInstalled(id, installed)
        if installed { loadedThisSession.insert(id) }
    }
}

// MARK: - Wire format

extension FeatureRuntimeCommand: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: BridgeCommandKey.self)
        let key = try BridgeCommandCoding.singleKey(container, in: decoder)
        switch key.stringValue {
        case "install": self = .install(try container.decode(String.self, forKey: key))
        case "uninstall": self = .uninstall(try container.decode(String.self, forKey: key))
        case "installAll": _ = try container.decode(Bool.self, forKey: key); self = .installAll
        case "uninstallAll": _ = try container.decode(Bool.self, forKey: key); self = .uninstallAll
        default: throw BridgeCommandCoding.unknownCase(key, decoder)
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: BridgeCommandKey.self)
        switch self {
        case .install(let id): try container.encode(id, forKey: BridgeCommandKey("install"))
        case .uninstall(let id): try container.encode(id, forKey: BridgeCommandKey("uninstall"))
        case .installAll: try container.encode(true, forKey: BridgeCommandKey("installAll"))
        case .uninstallAll: try container.encode(true, forKey: BridgeCommandKey("uninstallAll"))
        }
    }
}
