// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

// The C ABI the Qt shell binds to (WP-18).
//
// `linux/shell/include/corebridge.h` is the contract; this file is what emits
// the symbols it declares. The two must be read together, and the CI leg
// `bridge-abi` in `.github/workflows/linux-port-ci.yml` is what proves they
// agree: it builds this target as a static library, compiles a C client
// against that header alone, and calls all four functions.
//
// Deliberately four one-line wrappers. Everything they do lives in
// `BridgeCSurface` inside `VorssaintCore`, because an `@_cdecl` function in an
// executable target cannot be imported by `VorssaintCoreTests` — the pointer
// handling, the `strdup`/`free` pairing and the status codes would otherwise
// have no test but a CI job. What is left here is exactly the thing that
// cannot be tested any other way: the symbol names and their signatures.
//
// Not guarded by `#if os(Linux)`, unlike `main.swift`: there is nothing
// Linux-specific in a C ABI, and leaving it unguarded means `swift build` on a
// Mac type-checks it too. `build.sh` never compiles this directory, so the
// macOS product is untouched either way.

import VorssaintCore

/// `typedef void (*vs_snapshot_callback)(const char *json, void *ctx);`
public typealias vs_snapshot_callback = BridgeCSurface.SnapshotCallback

/// `int vs_subscribe(const char *service, vs_snapshot_callback callback, void *ctx);`
@_cdecl("vs_subscribe")
public func vs_subscribe(_ service: UnsafePointer<CChar>?,
                         _ callback: vs_snapshot_callback?,
                         _ ctx: UnsafeMutableRawPointer?) -> Int32 {
    BridgeCSurface.subscribe(service, callback, ctx)
}

/// `int vs_command(const char *service, const char *json);`
@_cdecl("vs_command")
public func vs_command(_ service: UnsafePointer<CChar>?,
                       _ json: UnsafePointer<CChar>?) -> Int32 {
    BridgeCSurface.command(service, json)
}

/// `char *vs_snapshot(const char *service);`
@_cdecl("vs_snapshot")
public func vs_snapshot(_ service: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>? {
    BridgeCSurface.snapshot(service)
}

/// `void vs_free(char *s);`
@_cdecl("vs_free")
public func vs_free(_ s: UnsafeMutablePointer<CChar>?) {
    BridgeCSurface.free(s)
}
