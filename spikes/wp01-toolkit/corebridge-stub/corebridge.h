/*
 * corebridge.h - the C ABI that VorssaintCore exports from Swift via @_cdecl.
 *
 * This header is written to be byte-for-byte compatible with what the Swift
 * declarations below would produce, so the stub can be swapped for the real
 * Swift library without touching a single line of shell code:
 *
 *   @_cdecl("vs_subscribe")
 *   public func vs_subscribe(_ service: UnsafePointer<CChar>,
 *                            _ callback: @convention(c) (UnsafePointer<CChar>?,
 *                                                        UnsafeMutableRawPointer?) -> Void,
 *                            _ ctx: UnsafeMutableRawPointer?) -> Int32
 *
 *   @_cdecl("vs_command")
 *   public func vs_command(_ service: UnsafePointer<CChar>,
 *                          _ json: UnsafePointer<CChar>) -> Int32
 *
 *   @_cdecl("vs_snapshot")
 *   public func vs_snapshot(_ service: UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>?
 *
 * Swift's @_cdecl emits plain C functions with no name mangling, so an
 * `extern "C"` block is all the C++ side needs. Ownership rules match what a
 * Swift `strdup`-style return implies: the caller frees the vs_snapshot
 * result with vs_free().
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
 * service, -2 on a command the service rejected. */
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
