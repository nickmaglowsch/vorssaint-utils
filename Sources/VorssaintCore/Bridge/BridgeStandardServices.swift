// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

public extension CoreBridge {
    /// The three services WP-18 adopts end to end, registered under the ids
    /// the Qt shell names.
    ///
    /// Called once from `BridgeCSurface` on the first C call, so a C program
    /// that links the static library and calls `vs_snapshot("metrics")` with
    /// no Swift `main` behind it still gets an answer. Calling it again
    /// replaces the services, which is what `--selftest` wants and what a
    /// second call must therefore not be an error.
    ///
    /// Feature squads add their own here, next to these three, and the
    /// checklist in `docs/linux-port/BRIDGE.md` is the thing to follow.
    @discardableResult
    func registerStandardServices() -> StandardServices {
        let features = FeatureRuntimeBridgeService(
            store: DefaultsFeatureAvailabilityStore(featureIDs: LinuxFeatureProbeSet.ids),
            bridge: self)
        let l10n = L10nBridgeService(bridge: self)
        let metrics = MetricsBridgeService(bridge: self)

        register(features)
        register(l10n)
        register(metrics)
        l10n.startObserving()

        return StandardServices(featureRuntime: features, l10n: l10n, metrics: metrics)
    }

    /// Held by whoever registered them, so a caller that wants to drive a
    /// service (the shell's sampler, a test) does not have to reach back
    /// through the registry for it.
    struct StandardServices {
        public let featureRuntime: FeatureRuntimeBridgeService
        public let l10n: L10nBridgeService
        public let metrics: MetricsBridgeService
    }
}

/// The feature ids the Linux hub offers today.
///
/// **This list is a placeholder and is meant to be deleted.** It names only
/// features whose Linux backend has actually landed or is in flight, because
/// a hub row for a feature with no backend is a row that lies.
///
/// WP-15 did not move `FeatureCatalog` into the core — `CORE_MOVES.md`
/// appendix A proves it cannot go — but it did put the ids and their platform
/// support there: `FeatureSupportCatalog.featureIDs(on: .linux)` is the real
/// 48-id Linux catalog, and it is what `registerStandardServices` passes once
/// every backend has landed. Until then
/// `FeatureSupportTests.testTheBridgeProbeSetIsASubsetOfTheLinuxCatalog`
/// checks every entry here is one of those 48, which is stronger than the
/// `linux-port-ci` grep it replaces the need for (that grep only proved the id
/// was an `AppFeature` case at all).
public enum LinuxFeatureProbeSet {
    public static let ids = [
        "switcher",          // WP-C1 window backend landed
        "windowLayout",      // WP-C1
        "clipboardHistory",  // WP-A8, data-control backend
        "keepAwake",         // WP-A6, logind inhibitors
        "mixer",             // WP-A5 backend in progress
        "micMute",           // WP-A5
        "soundOutputSwitcher", // WP-A5
        "screenshot",        // WP-B1 capture engine in progress
        "screenRecorder",    // WP-B1
        "commandBar",        // WP-B10
        "superKey",          // WP-S1 helper daemon merged
        "monitorCPU"         // WP-A1, the series `metrics` will carry
    ]
}
