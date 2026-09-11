/* vorssaint-helper: the privileged half of the input relay.
 *
 * It owns the evdev grab and the uinput device, and exposes the narrowest API
 * the unprivileged app needs, on the D-Bus *system* bus:
 *
 *   org.vorssaint.Helper1.SetRules(in s json)    polkit: auth_admin_keep
 *   org.vorssaint.Helper1.Enable(in b enable)    polkit: auth_admin_keep
 *   org.vorssaint.Helper1.GetDevices(out s json) polkit: yes (no prompt)
 *   org.vorssaint.Helper1.Event(t ts, q type, q code, i value)   signal
 *
 * The app never sees a file descriptor for an input device: it sends rules and
 * receives, in tap mode only, the events it needs to record a shortcut.
 * See docs/linux-port/PRIVILEGES.md for the threat model. */
#include "device.h"
#include "polkit_check.h"
#include "rules.h"

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

#define ACTION_SETRULES "org.vorssaint.helper.set-rules"
#define ACTION_ENABLE "org.vorssaint.helper.enable"
#define ACTION_GETDEVICES "org.vorssaint.helper.get-devices"

#define RING_CAP 1024

typedef struct {
    uint64_t ts;
    uint16_t type;
    uint16_t code;
    int32_t value;
} tap_ev;

static struct {
    input_backend *backend;
    bool fake;
    bool tap_mode;
    bool enabled;

    rules_state rules;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_running;
    volatile sig_atomic_t stop;

    tap_ev ring[RING_CAP];
    size_t ring_head, ring_tail;

    char devices_json[4096];
} H;

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* --- authorisation -------------------------------------------------------- */

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
        if (rc == DEV_READ_EOF)
            break;
        if (rc == DEV_READ_ERROR)
            break;

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

/* --- methods -------------------------------------------------------------- */

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

    r = sd_bus_message_read(m, "s", &json);
    if (r < 0)
        return r;

    if (rules_config_from_json(json, &cfg, err, sizeof(err)) < 0)
        return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "bad rules: %s", err);

    pthread_mutex_lock(&H.lock);
    n = rules_reconfigure(&H.rules, &cfg, out, RULES_MAX_OUT);
    /* Reconfiguring while a modifier is held would strand it down. */
    for (int k = 0; k < n && H.enabled; k++)
        H.backend->write(H.backend, &out[k]);
    if (n > 0 && H.enabled)
        H.backend->sync(H.backend);
    pthread_mutex_unlock(&H.lock);

    fprintf(stderr, "helper: SetRules applied: %s\n", json);
    return sd_bus_reply_method_return(m, "");
}

static int method_get_devices(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    char buf[4096];
    int r;

    (void)userdata;
    r = authorize(m, ACTION_GETDEVICES, ret_error);
    if (r < 0)
        return r;

    if (H.enabled && H.backend->describe(H.backend, buf, sizeof(buf)) > 0)
        return sd_bus_reply_method_return(m, "s", buf);
    return sd_bus_reply_method_return(m, "s", H.devices_json[0] ? H.devices_json : "[]");
}

static int method_enable(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    int enable;
    char err[512] = {0};
    int r;

    (void)userdata;
    r = authorize(m, ACTION_ENABLE, ret_error);
    if (r < 0)
        return r;

    r = sd_bus_message_read(m, "b", &enable);
    if (r < 0)
        return r;

    if (enable && !H.enabled) {
        if (H.backend->open(H.backend, !H.tap_mode, err, sizeof(err)) < 0) {
            fprintf(stderr, "helper: Enable(true) refused: %s\n", err);
            return sd_bus_error_setf(ret_error, SD_BUS_ERROR_FAILED,
                                     "cannot claim input devices: %s", err);
        }
        H.backend->describe(H.backend, H.devices_json, sizeof(H.devices_json));
        H.enabled = true;
        H.stop = 0;
        if (pthread_create(&H.thread, NULL, relay_thread, H.backend) == 0)
            H.thread_running = true;
        fprintf(stderr, "helper: relay enabled on backend '%s': %s\n", H.backend->name,
                H.devices_json);
    } else if (!enable && H.enabled) {
        H.stop = 1;
        if (H.thread_running)
            pthread_join(H.thread, NULL);
        H.enabled = false;
        fprintf(stderr, "helper: relay disabled, devices released\n");
    }
    return sd_bus_reply_method_return(m, "");
}

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
    return sd_bus_message_append(reply, "s", H.backend->name);
}

static int prop_authz(sd_bus *bus, const char *path, const char *iface, const char *prop,
                      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    (void)bus; (void)path; (void)iface; (void)prop; (void)userdata; (void)ret_error;
    return sd_bus_message_append(reply, "s", polkit_backend_name());
}

static const sd_bus_vtable helper_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SetRules", "s", "", method_set_rules, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDevices", "", "s", method_get_devices, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Enable", "b", "", method_enable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Event", "tqqi", 0),
    SD_BUS_PROPERTY("Rules", "s", prop_rules, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Backend", "s", prop_backend, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Authorization", "s", prop_authz, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END,
};

/* --- main ----------------------------------------------------------------- */

/* The fake backend has no hardware behind it, so where uinput does not exist
 * the helper stands in for a typist: one Caps Lock tap and one ordinary key,
 * pushed in every time the queue runs dry. That keeps the D-Bus path, the rules
 * engine and the Event signal exercised end to end. */
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

int main(int argc, char **argv)
{
    sd_bus_slot *slot = NULL;
    sd_bus *bus = NULL;
    const char *backend = "evdev";
    bool session_bus = false;
    int r;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--tap"))
            H.tap_mode = true;
        else if (!strcmp(argv[i], "--session-bus"))
            session_bus = true;
        else if (!strcmp(argv[i], "--deny-all"))
            polkit_set_stub_allow(false);
        else {
            fprintf(stderr,
                    "usage: vorssaint-helper [--backend evdev|fake] [--tap] [--session-bus]\n");
            return 1;
        }
    }

    pthread_mutex_init(&H.lock, NULL);
    rules_init(&H.rules, NULL);
    H.fake = strcmp(backend, "fake") == 0;
    H.backend = H.fake ? device_fake_new() : device_evdev_new();
    if (!H.backend)
        return 1;

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
        /* The bus policy in data/org.vorssaint.Helper1.conf allows only root
         * to own this name, so this is where an unprivileged impostor fails. */
        fprintf(stderr, "helper: cannot own %s: %s\n", BUS_NAME, strerror(-r));
        return 1;
    }
    fprintf(stderr, "helper: owning %s on the %s bus, uid=%u, authorization=%s\n", BUS_NAME,
            session_bus ? "session" : "system", (unsigned)geteuid(), polkit_backend_name());
    fprintf(stderr, "helper: backend=%s tap_mode=%s\n", H.backend->name,
            H.tap_mode ? "yes" : "no");
    fflush(stderr);

    while (!g_quit) {
        r = sd_bus_process(bus, NULL);
        if (r < 0)
            break;
        if (r > 0)
            continue;

        /* Drain the tap ring into D-Bus signals from this thread only: sd-bus
         * connections are not shared across threads. */
        pthread_mutex_lock(&H.lock);
        /* Only once the previous burst has been consumed, so the synthetic
         * timestamps stay monotonic. */
        if (H.fake && H.enabled && device_fake_pending(H.backend) == 0)
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

    H.stop = 1;
    if (H.thread_running)
        pthread_join(H.thread, NULL);
    if (H.enabled)
        H.backend->close(H.backend);
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    fprintf(stderr, "helper: exiting, devices released\n");
    return 0;
}
