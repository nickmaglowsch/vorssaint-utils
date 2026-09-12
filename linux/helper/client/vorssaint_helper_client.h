/* Client for org.vorssaint.Helper1.
 *
 * This is the unprivileged half, and it is what `linux/platform/helper-client`
 * links: the platform layer (and through it `Sources/VorssaintLinux`) never
 * builds a D-Bus message itself. Everything the app can do to the privileged
 * daemon is one of the calls below; there is no escape hatch for a raw method
 * name, because the point of the helper is that its API is enumerable.
 *
 * Error handling: every call returns 0 or a negative errno, and fills a
 * vh_error with the D-Bus error *name* as well as its message. The name is
 * what the capabilities hub branches on, and the four it must understand are:
 *
 *   org.freedesktop.DBus.Error.ServiceUnknown        helper not installed
 *   org.freedesktop.DBus.Error.AccessDenied          polkit refused
 *   org.freedesktop.DBus.Error.InteractiveAuthorizationRequired
 *                                                    needs an admin prompt
 *   org.vorssaint.Helper1.Error.NotOwner             another session owns it
 *
 * The client is not thread-safe: one vh_client per thread, as sd-bus requires.
 */
#ifndef VORSSAINT_HELPER_CLIENT_H
#define VORSSAINT_HELPER_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VH_BUS_NAME "org.vorssaint.Helper1"
#define VH_BUS_PATH "/org/vorssaint/Helper1"
#define VH_IFACE "org.vorssaint.Helper1"
#define VH_ERROR_NOT_OWNER "org.vorssaint.Helper1.Error.NotOwner"

typedef struct vh_client vh_client;

typedef struct {
    char name[128];    /* D-Bus error name, "" if the failure was local */
    char message[512]; /* human-readable */
} vh_error;

/* True if err names the given D-Bus error. */
bool vh_error_is(const vh_error *err, const char *dbus_error_name);

/* session_bus is for development against scripts/private-bus.sh; the shipped
 * app always passes false. */
int vh_connect(vh_client **out, bool session_bus, vh_error *err);
void vh_disconnect(vh_client *c);

/* True if the name has an owner on the bus — that is, the helper is installed
 * and activatable. Distinguishes "not installed" from "installed and
 * refusing", which are different sentences in the hub. */
int vh_is_available(vh_client *c, bool *available, vh_error *err);

/* --- input relay ---------------------------------------------------------- */

int vh_get_devices(vh_client *c, char *json, size_t cap, vh_error *err);
int vh_get_capabilities(vh_client *c, char *json, size_t cap, vh_error *err);
int vh_enable(vh_client *c, bool enable, vh_error *err);
int vh_set_rules(vh_client *c, const char *rules_json, vh_error *err);

/* The focused application, which the relay cannot see for itself: a grabbed
 * input device says nothing about which window has focus. Cheap and called
 * often, which is exactly why it is not part of the rules document -- see
 * docs/linux-port/PRIVILEGES.md § 4.1. */
int vh_set_context(vh_client *c, const char *context_json, vh_error *err);

/* --- fans ----------------------------------------------------------------- */

/* The caller must keep calling vh_fan_heartbeat() at least every
 * VH_FAN_HEARTBEAT_ADVISED_MS or the helper hands the fans back to the
 * firmware. That is the contract, not a detail: it is what makes a crashed
 * app safe. */
#define VH_FAN_HEARTBEAT_ADVISED_MS 3000

int vh_set_fan_pwm(vh_client *c, const char *hwmon, uint32_t channel, uint8_t value,
                   vh_error *err);
int vh_set_fan_auto(vh_client *c, const char *hwmon, uint32_t channel, vh_error *err);
int vh_fan_heartbeat(vh_client *c, vh_error *err);

/* --- DDC/CI --------------------------------------------------------------- */

int vh_ddc_write(vh_client *c, const char *bus, uint8_t vcp, uint16_t value, vh_error *err);
int vh_ddc_read(vh_client *c, const char *bus, uint8_t vcp, uint16_t *current, uint16_t *max,
                vh_error *err);

/* --- properties and events ------------------------------------------------ */

/* name is one of "Rules", "Backend", "Authorization", "Fan", "Owner". */
int vh_get_property(vh_client *c, const char *name, char *value, size_t cap, vh_error *err);

/* One Event signal. kind is "input", "rule" or "hotplug"; see
 * docs/linux-port/PRIVILEGES.md § 4.1 for the payload schema.
 *
 * For "input", device is the source index, the evdev triple is filled in and
 * detail is empty. For "rule", detail is a JSON object naming the rule and
 * what it decided, and the triple is zero. For "hotplug", detail is the
 * human-readable device line. kind and detail point into the message and are
 * valid only for the duration of the callback. */
typedef void (*vh_event_fn)(uint64_t ts_ns, const char *kind, uint32_t device, uint16_t type,
                            uint16_t code, int32_t value, const char *detail, void *user);

int vh_subscribe_events(vh_client *c, vh_event_fn fn, void *user, vh_error *err);

/* Pump the connection. Returns >0 if something was processed, 0 on timeout,
 * <0 on error. */
int vh_process(vh_client *c, uint64_t timeout_us, vh_error *err);

#ifdef __cplusplus
}
#endif

#endif /* VORSSAINT_HELPER_CLIENT_H */
