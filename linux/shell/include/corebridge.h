/*
 * corebridge.h - the C ABI that VorssaintCore exports from Swift via @_cdecl.
 *
 * WP-18. This is the real header; the WP-01 spike's identical stub lives at
 * spikes/wp01-toolkit/corebridge-stub/corebridge.h and the two declaration
 * regions are held byte-for-byte identical by the `bridge-abi` job in
 * .github/workflows/linux-port-ci.yml. docs/linux-port/BRIDGE.md lists every
 * difference between them and why.
 *
 * The symbols come from Sources/VorssaintLinux/CoreBridgeC.swift:
 *
 *   public typealias vs_snapshot_callback = BridgeCSurface.SnapshotCallback
 *   // @convention(c) (UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void
 *
 *   @_cdecl("vs_subscribe")
 *   public func vs_subscribe(_ service: UnsafePointer<CChar>?,
 *                            _ callback: vs_snapshot_callback?,
 *                            _ ctx: UnsafeMutableRawPointer?) -> Int32
 *
 *   @_cdecl("vs_command")
 *   public func vs_command(_ service: UnsafePointer<CChar>?,
 *                          _ json: UnsafePointer<CChar>?) -> Int32
 *
 *   @_cdecl("vs_snapshot")
 *   public func vs_snapshot(_ service: UnsafePointer<CChar>?) -> UnsafeMutablePointer<CChar>?
 *
 *   @_cdecl("vs_free")
 *   public func vs_free(_ s: UnsafeMutablePointer<CChar>?)
 *
 * Swift's @_cdecl emits plain C functions with no name mangling, so an
 * `extern "C"` block is all the C++ side needs. Ownership rules match what a
 * Swift `strdup`-style return implies: the caller frees the vs_snapshot
 * result with vs_free().
 *
 * The Swift parameters are Optional pointers where the spike's header showed
 * them non-optional. That is an ABI no-op -- an Optional pointer imports as
 * `char *` either way -- and it is what lets the Swift side answer a NULL
 * service name with -1 instead of trapping.
 *
 * ------------------------------------------------------------------------
 * The contract
 * ------------------------------------------------------------------------
 *
 * Rules every caller follows, and every reviewer should check. They are the
 * bridge's half of what linux/platform/README.md states for the platform
 * vtable, and they are what spikes/wp01-toolkit/qt-quick/CoreModel.cpp
 * already assumes.
 *
 * - Every call returns int or a pointer that says why. vs_subscribe returns
 *   a token >= 1 or -1; vs_command returns 0 or a negative code naming the
 *   reason; vs_snapshot returns NULL only for an unknown service. No call
 *   fails silently.
 *
 * - One thread calls in. vs_subscribe, vs_command and vs_snapshot must all be
 *   called from the thread that runs the core -- in the shell, the GUI
 *   thread, which is also the thread the Swift services already require
 *   ("main thread only"). CoreModel::setService and CoreModel::invoke are
 *   both reached from QML, so this already holds; do not call them from a
 *   worker.
 *
 * - The callback may arrive on any thread. It fires on whichever thread
 *   published the snapshot. Today that is the main thread, but WP-A1's
 *   sensors will sample on their own and a contract promising the main thread
 *   would have to be broken then rather than now. Copy the string and hop:
 *   CoreModel::snapshotTrampoline does exactly that, with
 *   QString::fromUtf8() followed by a Qt::QueuedConnection.
 *
 * - The callback's json pointer dies with the call. It points into a
 *   temporary UTF-8 buffer the bridge owns. Copy before returning; do not
 *   store it, and do not free it.
 *
 * - The callback must not block. It runs with the bridge's lock held, which
 *   is what keeps two publishes from delivering out of order and what stops a
 *   service being unregistered mid-delivery. Re-entering the bridge from the
 *   same thread inside the callback is safe (the lock is recursive); blocking
 *   on another thread that is about to call in is not.
 *
 * - Snapshots are diffed before delivery. The bridge compares the encoded
 *   bytes with what the subscribers last received and delivers nothing when
 *   they match, so a callback firing means the state really moved. Keys are
 *   emitted in sorted order for exactly this reason. A full snapshot is sent,
 *   never a patch: see docs/linux-port/BRIDGE.md for the measurement behind
 *   that choice.
 *
 * - Ownership is explicit. Every char * from vs_snapshot is released by
 *   vs_free and by nothing else.
 */
#ifndef VORSSAINT_COREBRIDGE_H
#define VORSSAINT_COREBRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the bridge's own thread with a NUL-terminated JSON snapshot.
 * The pointer is owned by the bridge and is only valid for the duration of
 * the call; the shell must copy or marshal it to its UI thread. */
typedef void (*vs_snapshot_callback)(const char *json, void *ctx);

/* Subscribe to a service's snapshot stream. Returns a subscription token
 * (>= 1) on success, or -1 if the service is unknown. The callback fires
 * once immediately with the current snapshot, then on every change. */
int vs_subscribe(const char *service, vs_snapshot_callback callback, void *ctx);

/* Deliver a command to a service. `json` is the Codable command enum encoded
 * as JSON, for example {"set":{"key":"toggle","value":true}} or
 * {"recordShortcut":"Ctrl+Alt+K"}. Returns 0 on success, -1 on an unknown
 * service, -2 on a command the service rejected, -3 on JSON the service's
 * command enum could not decode. */
int vs_command(const char *service, const char *json);

/* Current snapshot of a service as a newly allocated NUL-terminated JSON
 * string, or NULL if the service is unknown. Free with vs_free(). */
char *vs_snapshot(const char *service);

/* Free a string returned by vs_snapshot(). */
void vs_free(char *s);

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_COREBRIDGE_H */
