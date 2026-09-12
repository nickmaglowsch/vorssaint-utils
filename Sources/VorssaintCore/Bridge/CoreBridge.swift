// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint

import Foundation

/// The registry behind the three C entry points of `PLAN.md` § 4.2:
/// `subscribe(service, callback)`, `command(service, json)` and
/// `snapshot(service) -> json`.
///
/// One instance is shared by the whole process (`CoreBridge.shared`); a test
/// makes its own so it never sees another test's services.
///
/// ## Threading contract
///
/// This is the *Swift half* of what
/// `linux/shell/include/corebridge.h` states for the C half, and it is written
/// to match what `spikes/wp01-toolkit/qt-quick/CoreModel.cpp` already assumes.
///
/// 1. **`subscribe`, `command`, `snapshot` and `unsubscribe` are called on the
///    thread that runs the core** — the shell's GUI thread, which is also the
///    core's "main thread only" thread. `CoreModel::setService` and
///    `CoreModel::invoke` are both reached from QML, so this already holds; the
///    shell must never call them from a worker.
/// 2. **A snapshot callback may arrive on any thread** — whichever thread
///    called `publish`. Today every publisher is on the main thread, but
///    WP-A1's sensors will sample on their own, and a contract that promised
///    the main thread would have to be broken then rather than now.
///    `CoreModel::snapshotTrampoline` copies the string and posts a
///    `Qt::QueuedConnection` to the GUI thread, which is exactly the right
///    thing for both cases.
/// 3. **The JSON handed to a callback is owned by the bridge and dies with the
///    call.** The C layer passes a pointer into a Swift `String`'s temporary
///    UTF-8 buffer; after the callback returns it is gone. Copy, then hop.
/// 4. **Callbacks run with the bridge's lock held**, so two publishes can
///    never deliver out of order and a service cannot be unregistered
///    mid-delivery. A callback must therefore not block and must not call back
///    into the bridge from *another* thread; re-entering from the *same*
///    thread is safe (the lock is recursive), which is what lets `apply` end
///    with `publishToBridge()`.
///
/// ## Swift 6 concurrency
///
/// The package is still on the Swift 5 language mode (`swift-tools-version:
/// 5.9`), so this type carries its own `NSRecursiveLock` rather than an actor.
/// That is deliberate and survives the move to Swift 6: the C entry points are
/// `@_cdecl` functions, which cannot be `async` and cannot inherit an actor's
/// isolation, so *something* has to be a synchronous, lock-guarded boundary.
/// When the package moves to the Swift 6 mode, the services stay
/// `@MainActor` and this class stays `@unchecked Sendable` with the lock as
/// its stated reason — the boundary does not move, only the annotation.
public final class CoreBridge: @unchecked Sendable {
    /// The process-wide bridge the `@_cdecl` exports talk to.
    public static let shared = CoreBridge()

    /// A subscription token. `>= 1`, like the C contract says.
    public typealias Token = Int32

    /// Where the bridge reports what the C boundary has to swallow: a snapshot
    /// that would not encode, a command that would not decode. Defaults to
    /// stderr. Set by the shell in WP-20 so it lands in the app's log.
    public var diagnostic: (String) -> Void = { message in
        FileHandle.standardError.write(Data(("CoreBridge: " + message + "\n").utf8))
    }

    /// Deliveries actually made, and deliveries the diff suppressed. Read by
    /// the diffing test and by `--selftest` (WP-20).
    public private(set) var deliveryCount = 0
    public private(set) var suppressedCount = 0
    public private(set) var deliveredBytes = 0

    private let lock = NSRecursiveLock()
    private var registry: [String: Registration] = [:]
    private var nextToken: Token = 1
    /// Which service each live token belongs to, so `unsubscribe` needs only
    /// the token — the Qt side holds one token per `CoreModel` and no id.
    private var tokenOwners: [Token: String] = [:]

    public init() {}

    // MARK: Registration

    private final class Registration {
        let id: String
        /// Encode the service's current state. Type erased here so the
        /// registry can be heterogeneous; the generic `register` below is the
        /// only thing that builds it.
        let encodeSnapshot: () throws -> Data
        let applyCommand: (Data) throws -> Void
        var subscribers: [Token: (String) -> Void] = [:]
        /// The exact bytes every subscriber last received. `nil` until the
        /// first delivery. This is the diff.
        var lastDelivered: Data?

        init(id: String,
             encodeSnapshot: @escaping () throws -> Data,
             applyCommand: @escaping (Data) throws -> Void) {
            self.id = id
            self.encodeSnapshot = encodeSnapshot
            self.applyCommand = applyCommand
        }
    }

    /// Register a service under its `bridgeID`. Registering twice replaces the
    /// first — the shell's `--selftest` re-registers — and drops its
    /// subscribers, because their tokens belonged to the service that left.
    public func register<S: BridgeService>(_ service: S) {
        lock.lock()
        defer { lock.unlock() }
        let id = S.bridgeID
        if let existing = registry[id] {
            for token in existing.subscribers.keys { tokenOwners[token] = nil }
        }
        registry[id] = Registration(
            id: id,
            // The service is captured strongly. Every one of them is a
            // process-lifetime singleton, and a weak capture would make a
            // registered service that nothing else happened to hold read back
            // as "unknown" — a failure mode that would show up first in a
            // squad's own test and cost an afternoon. `unregister` is the way
            // out, and the shell never needs it.
            encodeSnapshot: {
                do {
                    return try BridgeJSON.encode(service.bridgeSnapshot())
                } catch {
                    throw BridgeError.snapshotEncodingFailed(
                        service: id, reason: String(describing: error))
                }
            },
            applyCommand: { data in
                let command: S.Command
                do {
                    command = try BridgeJSON.decode(S.Command.self, from: data)
                } catch {
                    throw BridgeError.malformedCommand(
                        service: id, reason: String(describing: error))
                }
                do {
                    try service.apply(command)
                } catch let error as BridgeError {
                    throw error
                } catch {
                    throw BridgeError.rejected(service: id, reason: String(describing: error))
                }
            })
    }

    /// Remove a service. Its subscribers stop hearing from it; their tokens
    /// become unknown, which is what `unsubscribe` on a dead token must see.
    public func unregister(_ id: String) {
        lock.lock()
        defer { lock.unlock() }
        if let existing = registry.removeValue(forKey: id) {
            for token in existing.subscribers.keys { tokenOwners[token] = nil }
        }
    }

    public var registeredServiceIDs: [String] {
        lock.lock()
        defer { lock.unlock() }
        return registry.keys.sorted()
    }

    // MARK: The three calls

    /// Subscribe to a service's snapshot stream. The sink fires **once
    /// immediately** with the current snapshot — the C header promises that and
    /// `CoreModel` relies on it — and then on every change.
    @discardableResult
    public func subscribe(to id: String,
                          sink: @escaping (String) -> Void) throws -> Token {
        lock.lock()
        defer { lock.unlock() }
        guard let registration = registry[id] else { throw BridgeError.unknownService(id) }
        // Encode before handing out a token: a service whose snapshot will not
        // encode must fail the subscribe rather than register a subscriber that
        // can never be fed.
        let payload = try registration.encodeSnapshot()
        let token = nextToken
        nextToken += 1
        registration.subscribers[token] = sink
        tokenOwners[token] = id
        registration.lastDelivered = payload
        deliver(payload, to: sink)
        return token
    }

    /// Drop one subscription. Unknown and already-dropped tokens are a no-op:
    /// the shell tears a `CoreModel` down after the service it named is gone
    /// often enough that making that an error would only produce noise.
    public func unsubscribe(_ token: Token) {
        lock.lock()
        defer { lock.unlock() }
        guard let id = tokenOwners.removeValue(forKey: token) else { return }
        registry[id]?.subscribers[token] = nil
    }

    /// Deliver a command, as the JSON the views send.
    public func command(_ id: String, json: String) throws {
        lock.lock()
        defer { lock.unlock() }
        guard let registration = registry[id] else { throw BridgeError.unknownService(id) }
        try registration.applyCommand(Data(json.utf8))
    }

    /// The service's current snapshot as JSON, for a reader that has no
    /// subscription yet (`CoreModel::setService` makes this call first so the
    /// view has state before its first frame).
    public func snapshotJSON(_ id: String) throws -> String {
        lock.lock()
        defer { lock.unlock() }
        guard let registration = registry[id] else { throw BridgeError.unknownService(id) }
        let payload = try registration.encodeSnapshot()
        return String(decoding: payload, as: UTF8.self)
    }

    // MARK: Publishing, and the diff

    /// Re-read a service's state and deliver it, unless the encoded bytes are
    /// byte-for-byte the ones the subscribers already hold.
    ///
    /// Returns `true` if anything was delivered. A service with no subscribers
    /// still records the snapshot as "last delivered", so the first subscriber
    /// after it is fed by `subscribe`, not by a stale comparison.
    @discardableResult
    public func publish(_ id: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard let registration = registry[id] else { return false }
        let payload: Data
        do {
            payload = try registration.encodeSnapshot()
        } catch {
            diagnostic("\(id): snapshot not delivered: \(error)")
            return false
        }
        guard payload != registration.lastDelivered else {
            suppressedCount += 1
            return false
        }
        registration.lastDelivered = payload
        guard !registration.subscribers.isEmpty else { return false }
        // Sorted by token so delivery order is the subscription order rather
        // than a dictionary's.
        for token in registration.subscribers.keys.sorted() {
            guard let sink = registration.subscribers[token] else { continue }
            deliver(payload, to: sink)
        }
        return true
    }

    /// Forget what a service last delivered, so the next publish is
    /// unconditional. The shell calls this after a language change, where the
    /// snapshot is identical but every *other* service's strings moved.
    public func invalidate(_ id: String) {
        lock.lock()
        defer { lock.unlock() }
        registry[id]?.lastDelivered = nil
    }

    private func deliver(_ payload: Data, to sink: (String) -> Void) {
        deliveryCount += 1
        deliveredBytes += payload.count
        sink(String(decoding: payload, as: UTF8.self))
    }

    /// Reset the counters. Tests and `--selftest`; never the running shell.
    public func resetCounters() {
        lock.lock()
        defer { lock.unlock() }
        deliveryCount = 0
        suppressedCount = 0
        deliveredBytes = 0
    }
}
