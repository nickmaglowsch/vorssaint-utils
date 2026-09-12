// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// The pointer-level implementation of the four C entry points.
///
/// `Sources/VorssaintLinux/CoreBridgeC.swift` is nothing but four `@_cdecl`
/// lines over this enum. The split exists so the pointer handling — the
/// `strdup`, the NUL termination, the `free`, the status codes — is inside
/// `VorssaintCore` where `VorssaintCoreTests` can reach it. An `@_cdecl`
/// function in an executable target cannot be imported by a test target, so
/// the alternative would be a C ABI whose only test is a CI job.
///
/// Everything here matches `linux/shell/include/corebridge.h` exactly; that
/// header is the contract and this is the implementation of it.
public enum BridgeCSurface {
    /// The C callback type. Byte-for-byte the header's
    /// `typedef void (*vs_snapshot_callback)(const char *json, void *ctx);`
    public typealias SnapshotCallback =
        @convention(c) (UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void

    /// Which bridge the C surface talks to. `CoreBridge.shared` in the shell;
    /// a test points it at its own instance and puts it back afterwards.
    public static var bridge: CoreBridge = .shared

    // MARK: vs_subscribe

    /// `int vs_subscribe(const char *service, vs_snapshot_callback callback,
    ///                   void *ctx)`
    ///
    /// Returns the subscription token (`>= 1`) or `-1` for an unknown service,
    /// a NULL name or a NULL callback.
    public static func subscribe(_ service: UnsafePointer<CChar>?,
                                 _ callback: SnapshotCallback?,
                                 _ ctx: UnsafeMutableRawPointer?) -> Int32 {
        bootstrap()
        guard let service, let callback else { return -1 }
        let name = String(cString: service)
        do {
            return try bridge.subscribe(to: name) { json in
                // `withCString` hands out a NUL-terminated buffer that lives
                // exactly as long as this closure — which is what the header
                // promises the shell and what `CoreModel::snapshotTrampoline`
                // copies out of before it hops to the GUI thread.
                json.withCString { callback($0, ctx) }
            }
        } catch {
            bridge.diagnostic("vs_subscribe(\"\(name)\"): \(error)")
            return -1
        }
    }

    // MARK: vs_command

    /// `int vs_command(const char *service, const char *json)`
    ///
    /// `0` on success, `-1` unknown service, `-2` refused by the service,
    /// `-3` the JSON did not decode.
    public static func command(_ service: UnsafePointer<CChar>?,
                               _ json: UnsafePointer<CChar>?) -> Int32 {
        bootstrap()
        guard let service, let json else { return -1 }
        let name = String(cString: service)
        do {
            try bridge.command(name, json: String(cString: json))
            return 0
        } catch let error as BridgeError {
            bridge.diagnostic("vs_command(\"\(name)\") = \(error.cStatus): \(error)")
            return error.cStatus
        } catch {
            bridge.diagnostic("vs_command(\"\(name)\") = -2: \(error)")
            return -2
        }
    }

    // MARK: vs_snapshot / vs_free

    /// `char *vs_snapshot(const char *service)`
    ///
    /// A newly allocated NUL-terminated copy the caller frees with
    /// `vs_free`, or NULL for an unknown service.
    public static func snapshot(_ service: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>? {
        bootstrap()
        guard let service else { return nil }
        let name = String(cString: service)
        do {
            return allocate(try bridge.snapshotJSON(name))
        } catch {
            bridge.diagnostic("vs_snapshot(\"\(name)\") = NULL: \(error)")
            return nil
        }
    }

    /// `void vs_free(char *s)`
    public static func free(_ pointer: UnsafeMutablePointer<CChar>?) {
        guard let pointer else { return }
        accounting.lock()
        liveStrings -= 1
        accounting.unlock()
        #if canImport(Glibc)
        Glibc.free(UnsafeMutableRawPointer(pointer))
        #else
        Darwin.free(UnsafeMutableRawPointer(pointer))
        #endif
    }

    // MARK: Allocation accounting

    private static let accounting = NSLock()
    private static var liveStrings = 0

    /// How many `vs_snapshot` results have not been given back to `vs_free`.
    ///
    /// It exists for one test — "every `vs_snapshot` is matched by a
    /// `vs_free`" — and for `--selftest`. It is not a leak detector for the
    /// shell: a `free` of a pointer this surface never returned would push it
    /// negative, which is itself the bug worth seeing.
    public static var liveSnapshotStrings: Int {
        accounting.lock()
        defer { accounting.unlock() }
        return liveStrings
    }

    /// `strdup` of a Swift string, counted. `strdup` rather than a Swift
    /// allocation because `vs_free` is documented as the counterpart of
    /// `malloc`, and a shell that called plain `free()` on it — every C
    /// programmer's reflex — would then be right.
    private static func allocate(_ string: String) -> UnsafeMutablePointer<CChar>? {
        let copy: UnsafeMutablePointer<CChar>? = string.withCString { strdup($0) }
        guard let copy else { return nil }
        accounting.lock()
        liveStrings += 1
        accounting.unlock()
        return copy
    }

    // MARK: Bootstrap

    /// The C client has no Swift `main` to run first: a static library linked
    /// into a C program calls `vs_snapshot("metrics")` with nothing having
    /// initialised anything. So every entry point registers the standard
    /// services on first use, once.
    private static let bootstrapOnce: Void = {
        CoreBridge.shared.registerStandardServices()
    }()

    private static func bootstrap() { _ = bootstrapOnce }
}
