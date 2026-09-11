// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// Combine re-export shim (target wiring: WP-10, contents: WP-13).
//
// Darwin gets the system Combine; Linux gets OpenCombine 0.14. The switch is
// keyed on `canImport(Darwin)`, never on `canImport(Combine)`: the WP-00
// spike hit `error: circular dependency between modules 'VorssaintCombine'
// and 'Combine'` when `canImport(Combine)` resolved a sibling target, and no
// target in this package may be named `Combine` for the same reason
// (docs/linux-port/spikes/00-swift-core.md § 6).
//
// WP-13 owns what this module actually exposes (the `@Published`/
// `ObservableObject`/`AnyCancellable` surface the ported services need, and
// whether `OpenCombineFoundation`/`OpenCombineDispatch` are re-exported or
// wrapped). This file exists so the target, and therefore the OpenCombine
// dependency, resolves and builds on both platforms today.

#if canImport(Darwin)
@_exported import Combine
#else
@_exported import OpenCombine
@_exported import OpenCombineDispatch
@_exported import OpenCombineFoundation
#endif

/// Which Combine implementation this build re-exports. Lets a test assert the
/// switch resolved the way the platform expects.
public enum VorssaintCombineBackend {
    public static var name: String {
        #if canImport(Darwin)
        return "Combine"
        #else
        return "OpenCombine"
        #endif
    }
}
