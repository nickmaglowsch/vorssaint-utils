/* vorssaint-helper: the privileged half of the Linux port.
 *
 * One daemon owns everything the app cannot do as the desktop user, and
 * nothing else. On the D-Bus *system* bus, at /org/vorssaint/Helper1:
 *
 *   Enable(b)                     polkit: org.vorssaint.helper.enable
 *   SetRules(s json)              polkit: org.vorssaint.helper.set-rules
 *   GetDevices() -> s             polkit: org.vorssaint.helper.get-devices
 *   GetCapabilities() -> s        polkit: org.vorssaint.helper.get-capabilities
 *   SetFanPwm(s hwmon, u ch, y v) polkit: org.vorssaint.helper.fan-control
 *   SetFanAuto(s hwmon, u ch)     polkit: org.vorssaint.helper.fan-control
 *   FanHeartbeat()                polkit: org.vorssaint.helper.fan-control
 *   DdcWrite(s bus, y vcp, q val) polkit: org.vorssaint.helper.ddc
 *   DdcRead(s bus, y vcp) -> qq   polkit: org.vorssaint.helper.ddc
 *   Event(t ts, q type, q code, i value)                      signal
 *   Rules, Backend, Authorization, Fan, Owner                  properties
 *
 * Three invariants hold across all of it:
 *
 *  1. The app never receives a file descriptor for an input device, an i2c
 *     bus or a sysfs attribute. It sends values and reads state.
 *  2. Enable(true) binds the relay to the caller's logind session; only that
 *     session or uid 0 may disable it or change its rules.
 *  3. Nothing the helper turns on stays on by itself. Grabs are released the
 *     moment the relay is disabled or a source dies; fans put into manual
 *     control go back to the firmware curve when the client stops sending
 *     FanHeartbeat, when the relay is disabled, and at exit.
 *
 * See docs/linux-port/PRIVILEGES.md for the threat model. */
#include "caps.h"
#include "ddc.h"
#include "device.h"
#include "fan.h"
#include "polkit_check.h"
#include "rules.h"
#include "session.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define BUS_NAME "org.vorssaint.Helper1"
#define BUS_PATH "/org/vorssaint/Helper1"
#define IFACE "org.vorssaint.Helper1"

#define ERROR_NOT_OWNER "org.vorssaint.Helper1.Error.NotOwner"

#define ACTION_SETRULES "org.vorssaint.helper.set-rules"
#define ACTION_ENABLE "org.vorssaint.helper.enable"
#define ACTION_GETDEVICES "org.vorssaint.helper.get-devices"
#define ACTION_GETCAPS "org.vorssaint.helper.get-capabilities"
#define ACTION_FAN "org.vorssaint.helper.fan-control"
#define ACTION_DDC "org.vorssaint.helper.ddc"

#define RING_CAP 1024

typedef struct {
    uint64_t ts;
    uint16_t type;
    uint16_t code;
    int32_t value;
} tap_ev;

static struct {
    /* NULL whenever the relay is disabled. A backend exists only while the
     * devices are actually claimed, so "disabled" and "nothing grabbed"
     * cannot drift apart. */
    input_backend *backend;
    bool fake;
    bool tap_mode;
    bool enabled;
    /* Set by the relay thread when its last source dies. The main loop tears
     * the backend down, because the thread must not free an object the D-Bus
     * methods may still be holding the lock on. */
    volatile sig_atomic_t fatal;
    /* Set by the relay thread when the source set changed, so the main loop
     * logs it from the thread that owns stderr ordering. */
    volatile sig_atomic_t hotplugged;

    rules_state rules;
    fan_state fan;
    caps_paths paths;

    session_lookup *sessions;
    session_owner owner;

    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_running;
    volatile sig_atomic_t stop;

    tap_ev ring[RING_CAP];
    size_t ring_head, ring_tail;
} H;

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* --- caller identity ------------------------------------------------------ */

/* Everything the helper is allowed to believe about a caller comes from the
 * bus daemon, never from the message body: the caller states no pid and no
 * uid of its own. */
static int caller_identity(sd_bus_message *m, pid_t *pid, uid_t *uid)
{
    sd_bus_creds *creds = NULL;
    int r;

    *pid = 0;
    *uid = (uid_t)-1;
    r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_PID | SD_BUS_CREDS_EUID, &creds);
    if (r < 0)
        return r;
    r = sd_bus_creds_get_pid(creds, pid);
    if (r >= 0)
        r = sd_bus_creds_get_euid(creds, uid);
    sd_bus_creds_unref(creds);
    return r;
}

static int authorize(sd_bus_message *m, const char *action, sd_bus_error *ret_error)
{
    const char *sender = sd_bus_message_get_sender(m);
    char err[256] = {0};
    authz_result r;

    if (!sender) {
        /* No sender means the message did not come through a bus daemon, so
         * there is no identity to check. Refuse rather than guess. */
        return sd_bus_error_set_const(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
                                      "no sender identity on the message");
    }

    r = polkit_check(action, sender, false, err, sizeof(err));
    switch (r) {
    case AUTHZ_ALLOWED:
        fprintf(stderr, "helper: %s authorized for %s via %s\n", sender, action,
                polkit_backend_name());
        return 0;
    case AUTHZ_CHALLENGE:
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INTERACTIVE_AUTHORIZATION_REQUIRED,
                                 "%s requires an administrator prompt", action);
    case AUTHZ_DENIED:
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_ACCESS_DENIED, "%s denied for %s", action,
                                 sender);
    default:
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED, "authorization check failed: %s",
                                 err);
    }
}

/* The second gate on the relay: polkit says whether this identity may ever do
 * it, this says whether *this* session is the one that turned it on.
 * PLAN.md § 4.4: without it a second caller on another seat, authorised for
 * the same action, can disable a relay it did not enable or point its rules
 * somewhere else. */
static int authorize_owner(sd_bus_message *m, sd_bus_error *ret_error)
{
    char err[512] = {0};
    pid_t pid;
    uid_t uid;
    int r;

    if (caller_identity(m, &pid, &uid) < 0)
        return sd_bus_error_set_const(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
                                      "cannot resolve the caller's credentials");

    r = session_owner_check(&H.owner, H.sessions, pid, uid, err, sizeof(err));
    if (r == 0)
        return 0;
    fprintf(stderr, "helper: refused: %s\n", err);
    return sd_bus_error_setf(ret_error, ERROR_NOT_OWNER, "%s", err);
}

/* --- the relay thread ----------------------------------------------------- */

static void ring_push(uint64_t ts, const rules_event *e)
{
    size_t next = (H.ring_tail + 1) % RING_CAP;
    if (next == H.ring_head)
        return; /* full: drop rather than block the relay */
    H.ring[H.ring_tail].ts = ts;
    H.ring[H.ring_tail].type = e->type;
    H.ring[H.ring_tail].code = e->code;
    H.ring[H.ring_tail].value = e->value;
    H.ring_tail = next;
}

static void *relay_thread(void *arg)
{
    input_backend *b = arg;

    while (!H.stop) {
        rules_event in, out[RULES_MAX_OUT];
        uint64_t ts, deadline;
        int rc, m;

        pthread_mutex_lock(&H.lock);
        deadline = rules_deadline_ns(&H.rules);
        pthread_mutex_unlock(&H.lock);

        rc = b->read(b, &in, &ts, deadline ? deadline : now_monotonic_ns() + 200000000ULL);
        if (rc == DEV_READ_HOTPLUG) {
            /* A device appeared or went away while the relay was running. It
             * is already claimed on the same terms as the rest; say so from
             * the main loop, which owns the log. */
            H.hotplugged = 1;
            continue;
        }
        if (rc == DEV_READ_EOF || rc == DEV_READ_ERROR) {
            /* The last source died. The others are already gone, but any that
             * remain are still grabbed and therefore invisible to the
             * session; flag it and let the main loop close the backend rather
             * than limping on. */
            fprintf(stderr, "helper: source %s, releasing all devices\n",
                    rc == DEV_READ_EOF ? "disconnected" : "read error");
            H.fatal = 1;
            break;
        }

        pthread_mutex_lock(&H.lock);
        if (rc == DEV_READ_TIMEOUT) {
            m = rules_timer(&H.rules, now_monotonic_ns(), out, RULES_MAX_OUT);
        } else {
            int t = rules_timer(&H.rules, ts, out, RULES_MAX_OUT);
            for (int k = 0; k < t; k++)
                b->write(b, &out[k]);
            if (H.tap_mode)
                ring_push(ts, &in);
            m = rules_process(&H.rules, &in, ts, out, RULES_MAX_OUT);
        }
        for (int k = 0; k < m; k++)
            b->write(b, &out[k]);
        if (m > 0)
            b->sync(b);
        pthread_mutex_unlock(&H.lock);
    }
    H.thread_running = false;
    return NULL;
}

/* --- backend lifecycle ---------------------------------------------------- */

static void seed_fake_source(input_backend *b);

static input_backend *make_backend(void)
{
    input_backend *b = H.fake ? device_fake_new() : device_evdev_new();
    if (b && H.fake)
        seed_fake_source(b);
    return b;
}

/* Stop the relay and hand every device back to the session.
 *
 * This is the only place the relay stops, so there is no path on which the
 * helper reports itself disabled while still holding an EVIOCGRAB. A grabbed
 * device is invisible to the compositor, so a leaked grab is not a tidiness
 * problem: it is a keyboard the user cannot type on until the daemon exits. */
static void release_devices(void)
{
    if (!H.enabled)
        return;

    H.stop = 1;
    if (H.thread_running) {
        pthread_join(H.thread, NULL);
        H.thread_running = false;
    }
    if (H.backend) {
        H.backend->close(H.backend); /* ungrabs every source, destroys uinput */
        H.backend = NULL;
    }
    H.enabled = false;
    H.fatal = 0;
    session_owner_clear(&H.owner);
    fprintf(stderr, "helper: relay disabled, devices released\n");
}

/* --- methods: the relay --------------------------------------------------- */

static int method_set_rules(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *json;
    rules_config cfg;
    rules_event out[RULES_MAX_OUT];
    char err[256];
    int r, n;

    (void)userdata;
    r = authorize(m, ACTION_SETRULES, ret_error);
    if (r < 0)
        return r;
    r = authorize_owner(m, ret_error);
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "s", &json);
    if (r < 0)
        return r;

    /* Checked here as well as in the parser so the refusal names the size the
     * caller actually sent, and so nothing downstream ever sees the string. */
    if (strnlen(json, RULES_JSON_MAX + 1) > RULES_JSON_MAX) {
        fprintf(stderr, "helper: SetRules refused: document over %d bytes\n", RULES_JSON_MAX);
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS,
                                 "bad rules: document is longer than the %d byte limit",
                                 RULES_JSON_MAX);
    }

    if (rules_config_from_json(json, &cfg, err, sizeof(err)) < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "bad rules: %s", err);

    pthread_mutex_lock(&H.lock);
    n = rules_reconfigure(&H.rules, &cfg, out, RULES_MAX_OUT);
    /* Reconfiguring while a modifier is held would strand it down. */
    for (int k = 0; k < n && H.backend; k++)
        H.backend->write(H.backend, &out[k]);
    if (n > 0 && H.backend)
        H.backend->sync(H.backend);
    pthread_mutex_unlock(&H.lock);

    fprintf(stderr, "helper: SetRules applied: %s\n", json);
    return sd_bus_reply_method_return(m, "");
}

static int method_get_devices(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    char buf[4096] = "[]";
    char err[256] = {0};
    int r;

    (void)userdata;
    r = authorize(m, ACTION_GETDEVICES, ret_error);
    if (r < 0)
        return r;

    if (H.backend) {
        H.backend->describe(H.backend, buf, sizeof(buf));
        return sd_bus_reply_method_return(m, "s", buf);
    }

    /* Disabled. Enumerate for real rather than replaying the last answer: a
     * cached list would report devices as claimed after they were released,
     * which is the same lie as a leaked grab, only quieter. Listen-only
     * discovery takes no EVIOCGRAB and needs no uinput, so answering this way
     * costs the caller nothing. */
    {
        input_backend *probe = make_backend();
        if (probe) {
            if (probe->open(probe, false, err, sizeof(err)) < 0)
                fprintf(stderr, "helper: GetDevices found nothing: %s\n", err);
            else
                probe->describe(probe, buf, sizeof(buf));
            probe->close(probe);
        }
    }
    return sd_bus_reply_method_return(m, "s", buf);
}

static int method_get_capabilities(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    char buf[8192];
    int r;

    (void)userdata;
    r = authorize(m, ACTION_GETCAPS, ret_error);
    if (r < 0)
        return r;

    caps_to_json(&H.paths, buf, sizeof(buf));
    return sd_bus_reply_method_return(m, "s", buf);
}

static int method_enable(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    char err[512] = {0};
    char session[SESSION_ID_MAX];
    pid_t pid = 0;
    uid_t uid = (uid_t)-1;
    int enable;
    int r;

    (void)userdata;
    r = authorize(m, ACTION_ENABLE, ret_error);
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "b", &enable);
    if (r < 0)
        return r;

    /* Enable(true) on an already-enabled relay is not a no-op to be waved
     * through: it is the case where a second session would otherwise take
     * over a relay it did not start. Check ownership on both directions. */
    r = authorize_owner(m, ret_error);
    if (r < 0)
        return r;

    if (caller_identity(m, &pid, &uid) < 0)
        return sd_bus_error_set_const(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
                                      "cannot resolve the caller's credentials");

    if (enable && !H.enabled) {
        H.backend = make_backend();
        if (!H.backend)
            return sd_bus_error_set_const(ret_error, SD_BUS_ERROR_NO_MEMORY, "out of memory");

        if (H.backend->open(H.backend, !H.tap_mode, err, sizeof(err)) < 0) {
            fprintf(stderr, "helper: Enable(true) refused: %s\n", err);
            /* open() unwinds its own partial work, but the object itself is
             * ours to free, and leaving it around would make the next
             * Enable(true) reopen an already-open backend. */
            H.backend->close(H.backend);
            H.backend = NULL;
            return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED,
                                     "cannot claim input devices: %s", err);
        }
        H.enabled = true;
        H.stop = 0;
        H.fatal = 0;

        session_identify(H.sessions, pid, session, sizeof(session));
        session_owner_bind(&H.owner, session, uid, pid);

        if (pthread_create(&H.thread, NULL, relay_thread, H.backend) == 0)
            H.thread_running = true;
        {
            char devs[4096];
            H.backend->describe(H.backend, devs, sizeof(devs));
            /* The audit line PRIVILEGES.md § 7 asked for: the only way a user
             * can later discover the capability was on, and for whom. */
            fprintf(stderr,
                    "helper: relay ENABLED by uid=%u pid=%ld session=%s on backend '%s': %s\n",
                    (unsigned)uid, (long)pid, session[0] ? session : "(none)", H.backend->name,
                    devs);
        }
    } else if (!enable && H.enabled) {
        fprintf(stderr, "helper: relay DISABLE requested by uid=%u pid=%ld\n", (unsigned)uid,
                (long)pid);
        release_devices();
        /* Fans follow the relay: a client that has just given up the relay is
         * usually a client that has exited, and a fan left in manual mode by
         * a dead client is the failure the watchdog exists for. */
        if (H.fan.n_ch > 0)
            fprintf(stderr, "helper: restored %d fan channel(s) to automatic\n",
                    fan_restore_all(&H.fan));
    }
    return sd_bus_reply_method_return(m, "");
}

/* --- methods: fans -------------------------------------------------------- */

static int method_set_fan_pwm(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *hwmon;
    uint32_t channel;
    uint8_t value;
    char err[512] = {0};
    int r;

    (void)userdata;
    r = authorize(m, ACTION_FAN, ret_error);
    if (r < 0)
        return r;
    r = sd_bus_message_read(m, "suy", &hwmon, &channel, &value);
    if (r < 0)
        return r;

    pthread_mutex_lock(&H.lock);
    r = fan_set_pwm(&H.fan, hwmon, channel, value, now_monotonic_ns(), err, sizeof(err));
    pthread_mutex_unlock(&H.lock);
    if (r < 0) {
        fprintf(stderr, "helper: SetFanPwm(%s,%u,%u) refused: %s\n", hwmon, channel, value, err);
        return sd_bus_error_setf(ret_error,
                                 r == -EINVAL ? SD_BUS_ERROR_INVALID_ARGS : SD_BUS_ERROR_FAILED,
                                 "%s", err);
    }
    fprintf(stderr, "helper: SetFanPwm %s pwm%u = %u (verified; watchdog armed for %llu ms)\n",
            hwmon, channel, value, (unsigned long long)(FAN_WATCHDOG_NS / 1000000ULL));
    return sd_bus_reply_method_return(m, "");
}

static int method_set_fan_auto(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *hwmon;
    uint32_t channel;
    char err[512] = {0};
    int r;

    (void)userdata;
    r = authorize(m, ACTION_FAN, ret_error);
    if (r < 0)
        return r;
    r = sd_bus_message_read(m, "su", &hwmon, &channel);
    if (r < 0)
        return r;

    pthread_mutex_lock(&H.lock);
    r = fan_set_auto(&H.fan, hwmon, channel, err, sizeof(err));
    pthread_mutex_unlock(&H.lock);
    if (r < 0)
        return sd_bus_error_setf(ret_error,
                                 r == -EINVAL ? SD_BUS_ERROR_INVALID_ARGS : SD_BUS_ERROR_FAILED,
                                 "%s", err);
    fprintf(stderr, "helper: SetFanAuto %s pwm%u (verified)\n", hwmon, channel);
    return sd_bus_reply_method_return(m, "");
}

static int method_fan_heartbeat(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    int r;

    (void)userdata;
    r = authorize(m, ACTION_FAN, ret_error);
    if (r < 0)
        return r;

    pthread_mutex_lock(&H.lock);
    r = fan_heartbeat(&H.fan, now_monotonic_ns());
    pthread_mutex_unlock(&H.lock);
    if (r == -ENOENT)
        return sd_bus_error_set_const(ret_error, SD_BUS_ERROR_FAILED,
                                      "no fan channel is under manual control; re-apply the curve");
    return sd_bus_reply_method_return(m, "");
}

/* --- methods: DDC/CI ------------------------------------------------------ */

static int method_ddc_write(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *bus;
    uint8_t vcp;
    uint16_t value;
    char err[512] = {0};
    ddc_transport *t;
    int r;

    (void)userdata;
    r = authorize(m, ACTION_DDC, ret_error);
    if (r < 0)
        return r;
    r = sd_bus_message_read(m, "syq", &bus, &vcp, &value);
    if (r < 0)
        return r;

    t = ddc_transport_i2c(bus, err, sizeof(err));
    if (!t)
        return sd_bus_error_setf(ret_error,
                                 ddc_bus_name_ok(bus) ? SD_BUS_ERROR_FAILED
                                                      : SD_BUS_ERROR_INVALID_ARGS,
                                 "%s", err);
    r = ddc_write_vcp(t, vcp, value, err, sizeof(err));
    t->close(t);
    if (r < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_IO_ERROR, "%s", err);
    fprintf(stderr, "helper: DdcWrite %s vcp 0x%02x = %u\n", bus, vcp, value);
    return sd_bus_reply_method_return(m, "");
}

static int method_ddc_read(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    const char *bus;
    uint8_t vcp;
    uint16_t cur = 0, max = 0;
    char err[512] = {0};
    ddc_transport *t;
    int r;

    (void)userdata;
    r = authorize(m, ACTION_DDC, ret_error);
    if (r < 0)
        return r;
    r = sd_bus_message_read(m, "sy", &bus, &vcp);
    if (r < 0)
        return r;

    t = ddc_transport_i2c(bus, err, sizeof(err));
    if (!t)
        return sd_bus_error_setf(ret_error,
                                 ddc_bus_name_ok(bus) ? SD_BUS_ERROR_FAILED
                                                      : SD_BUS_ERROR_INVALID_ARGS,
                                 "%s", err);
    r = ddc_read_vcp(t, vcp, &cur, &max, err, sizeof(err));
    t->close(t);
    if (r < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_IO_ERROR, "%s", err);
    return sd_bus_reply_method_return(m, "qq", cur, max);
}

/* --- properties ----------------------------------------------------------- */

static int prop_rules(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    char json[512];
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    pthread_mutex_lock(&H.lock);
    rules_state_to_json(&H.rules, json, sizeof(json));
    pthread_mutex_unlock(&H.lock);
    return sd_bus_message_append(reply, "s", json);
}

static int prop_backend(sd_bus *bus, const char *path, const char *iface, const char *prop,
                        sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    /* Reportable while disabled, when no backend object exists. */
    return sd_bus_message_append(reply, "s", H.fake ? "fake" : "evdev");
}

static int prop_authz(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    return sd_bus_message_append(reply, "s", polkit_backend_name());
}

static int prop_fan(sd_bus *bus, const char *path, const char *iface, const char *prop,
                    sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    char json[2048];
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    pthread_mutex_lock(&H.lock);
    fan_state_to_json(&H.fan, now_monotonic_ns(), json, sizeof(json));
    pthread_mutex_unlock(&H.lock);
    return sd_bus_message_append(reply, "s", json);
}

/* Who holds the relay, so the capabilities page can say "another session has
 * this on" instead of only failing when the user tries to turn it off. */
static int prop_owner(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    char json[256];
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    snprintf(json, sizeof(json),
             "{\"bound\":%s,\"session\":\"%s\",\"uid\":%d,\"lookup\":\"%s\"}",
             H.owner.bound ? "true" : "false", H.owner.session,
             H.owner.bound ? (int)H.owner.uid : -1, H.sessions->name);
    return sd_bus_message_append(reply, "s", json);
}

static const sd_bus_vtable helper_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SetRules", "s", "", method_set_rules, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDevices", "", "s", method_get_devices, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCapabilities", "", "s", method_get_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Enable", "b", "", method_enable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetFanPwm", "suy", "", method_set_fan_pwm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetFanAuto", "su", "", method_set_fan_auto, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("FanHeartbeat", "", "", method_fan_heartbeat, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DdcWrite", "syq", "", method_ddc_write, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DdcRead", "sy", "qq", method_ddc_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Event", "tqqi", 0),
    SD_BUS_PROPERTY("Rules", "s", prop_rules, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Backend", "s", prop_backend, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Authorization", "s", prop_authz, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Fan", "s", prop_fan, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Owner", "s", prop_owner, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END,
};

/* --- main ----------------------------------------------------------------- */

/* The fake backend has no hardware behind it, so where uinput does not exist
 * the helper stands in for a typist: one Caps Lock tap and one ordinary key,
 * pushed in every time the queue runs dry. That keeps the D-Bus path, the
 * rules engine and the Event signal exercised end to end. */
static void seed_fake_source(input_backend *b)
{
    /* A cursor rather than the clock, so successive bursts never overlap and
     * the timestamps the Event signal carries stay monotonic. */
    static uint64_t cursor;
    uint64_t now = now_monotonic_ns();
    uint64_t t;

    if (cursor < now)
        cursor = now;
    t = cursor + 5000000ULL;
    cursor = t + 400000000ULL;

    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 1, t);
    device_fake_push(b, EV_KEY, KEY_CAPSLOCK, 0, t + 50000000ULL);
    device_fake_push(b, EV_KEY, KEY_H, 1, t + 300000000ULL);
    device_fake_push(b, EV_KEY, KEY_H, 0, t + 340000000ULL);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vorssaint-helper [options]\n"
            "  --backend evdev|fake   device backend (default evdev)\n"
            "  --tap                  listen-only relay: no grab, no uinput\n"
            "  --session-bus          connect to the session bus (development)\n"
            "  --hwmon-root PATH      sysfs hwmon root (default /sys/class/hwmon)\n"
            "  --fake-sessions SPEC   test session lookup: 'PID=name' or 'uid:UID=name',\n"
            "                         comma-separated\n"
            "  --deny-all             stub authorization denies everything\n"
            "                         (only in a -DWITH_POLKIT=OFF build)\n");
}

/* --fake-sessions exists because logind cannot be run in a container: with no
 * systemd as pid 1, sd_pid_get_session() has no session to return for any
 * pid, so the binding would be untestable on the bus. It is refused in a
 * build with real polkit, which is the build that is installed. */
static int parse_fake_sessions(const char *spec)
{
    char *copy = strdup(spec);
    char *save = NULL, *tok;
    int n = 0;

    if (!copy)
        return -1;
    H.sessions = session_lookup_fake();
    if (!H.sessions) {
        free(copy);
        return -1;
    }
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strncmp(tok, "uid:", 4) == 0)
            session_fake_add_uid(H.sessions, (uid_t)strtoul(tok + 4, NULL, 10), eq + 1);
        else
            session_fake_add(H.sessions, (pid_t)strtol(tok, NULL, 10), eq + 1);
        n++;
    }
    free(copy);
    return n;
}

int main(int argc, char **argv)
{
    sd_bus_slot *slot = NULL;
    sd_bus *bus = NULL;
    const char *backend = "evdev";
    const char *hwmon_root = NULL;
    const char *fake_sessions = NULL;
    bool session_bus = false;
    int r;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--tap"))
            H.tap_mode = true;
        else if (!strcmp(argv[i], "--session-bus"))
            session_bus = true;
        else if (!strcmp(argv[i], "--hwmon-root") && i + 1 < argc)
            hwmon_root = argv[++i];
        else if (!strcmp(argv[i], "--fake-sessions") && i + 1 < argc)
            fake_sessions = argv[++i];
        else if (!strcmp(argv[i], "--deny-all"))
            polkit_set_stub_allow(false);
        else {
            usage();
            return 1;
        }
    }

    pthread_mutex_init(&H.lock, NULL);
    rules_init(&H.rules, NULL);
    fan_init(&H.fan, hwmon_root);
    caps_paths_defaults(&H.paths);
    if (hwmon_root)
        H.paths.hwmon_root = hwmon_root;
    session_owner_clear(&H.owner);
    H.sessions = session_lookup_logind();

    if (fake_sessions) {
        if (polkit_is_real()) {
            fprintf(stderr, "helper: --fake-sessions is refused in a build with real polkit\n");
            return 1;
        }
        if (parse_fake_sessions(fake_sessions) < 0) {
            fprintf(stderr, "helper: cannot parse --fake-sessions\n");
            return 1;
        }
    }

    /* No backend is created here: devices are claimed by Enable(true) and
     * released by Enable(false), so a helper sitting idle on the bus holds
     * nothing at all. */
    H.fake = strcmp(backend, "fake") == 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* sd_bus_open_system honours DBUS_SYSTEM_BUS_ADDRESS, which is how the
     * private-bus harness in scripts/ points the helper at a test bus without
     * changing a line of this code. */
    r = session_bus ? sd_bus_open_user(&bus) : sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "helper: cannot connect to bus: %s\n", strerror(-r));
        return 1;
    }

    r = sd_bus_add_object_vtable(bus, &slot, BUS_PATH, IFACE, helper_vtable, NULL);
    if (r < 0) {
        fprintf(stderr, "helper: cannot export %s: %s\n", IFACE, strerror(-r));
        return 1;
    }

    r = sd_bus_request_name(bus, BUS_NAME, 0);
    if (r < 0) {
        /* The bus policy in dist/org.vorssaint.Helper1.conf allows only root
         * to own this name, so this is where an unprivileged impostor fails. */
        fprintf(stderr, "helper: cannot own %s: %s\n", BUS_NAME, strerror(-r));
        return 1;
    }
    fprintf(stderr, "helper: owning %s on the %s bus, uid=%u, authorization=%s\n", BUS_NAME,
            session_bus ? "session" : "system", (unsigned)geteuid(), polkit_backend_name());
    fprintf(stderr,
            "helper: backend=%s tap_mode=%s sessions=%s hwmon_root=%s "
            "(nothing claimed until Enable(true))\n",
            H.fake ? "fake" : "evdev", H.tap_mode ? "yes" : "no", H.sessions->name, H.fan.root);
    fflush(stderr);

    while (!g_quit) {
        r = sd_bus_process(bus, NULL);
        if (r < 0)
            break;
        if (r > 0)
            continue;

        if (H.fatal)
            release_devices();
        if (H.hotplugged) {
            H.hotplugged = 0;
            pthread_mutex_lock(&H.lock);
            if (H.backend) {
                char devs[4096];
                H.backend->describe(H.backend, devs, sizeof(devs));
                fprintf(stderr, "helper: hot-plug: %s; sources now %s\n",
                        H.backend->last_hotplug(H.backend), devs);
            }
            pthread_mutex_unlock(&H.lock);
        }

        /* The fan watchdog runs here rather than on a timer thread: sd_bus_wait
         * below has a 100 ms ceiling, so the deadline is honoured within that,
         * and the restore happens on the thread that owns the fan state. */
        pthread_mutex_lock(&H.lock);
        {
            int restored = fan_watchdog_tick(&H.fan, now_monotonic_ns());
            if (restored > 0)
                fprintf(stderr,
                        "helper: FAN WATCHDOG: no FanHeartbeat for %llu ms, restored %d channel(s) "
                        "to automatic control\n",
                        (unsigned long long)(FAN_WATCHDOG_NS / 1000000ULL), restored);
        }
        /* Drain the tap ring into D-Bus signals from this thread only: sd-bus
         * connections are not shared across threads. */
        /* Only once the previous burst has been consumed, so the synthetic
         * timestamps stay monotonic. */
        if (H.fake && H.enabled && H.backend && device_fake_pending(H.backend) == 0)
            seed_fake_source(H.backend);
        while (H.ring_head != H.ring_tail) {
            tap_ev *e = &H.ring[H.ring_head];
            sd_bus_emit_signal(bus, BUS_PATH, IFACE, "Event", "tqqi", e->ts, e->type, e->code,
                               e->value);
            H.ring_head = (H.ring_head + 1) % RING_CAP;
        }
        pthread_mutex_unlock(&H.lock);

        r = sd_bus_wait(bus, 100000); /* 100 ms, so the ring drains promptly */
        if (r < 0)
            break;
    }

    release_devices();
    if (H.fan.n_ch > 0)
        fprintf(stderr, "helper: exit: restored %d fan channel(s) to automatic\n",
                fan_restore_all(&H.fan));
    session_lookup_free(H.sessions);
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    fprintf(stderr, "helper: exiting, devices released\n");
    return 0;
}
