#include "vorssaint_helper_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

struct vh_client {
    sd_bus *bus;
    vh_event_fn on_event;
    void *on_event_user;
};

static void err_clear(vh_error *err)
{
    if (err) {
        err->name[0] = '\0';
        err->message[0] = '\0';
    }
}

static void err_local(vh_error *err, int rc, const char *what)
{
    if (!err)
        return;
    err->name[0] = '\0';
    snprintf(err->message, sizeof(err->message), "%s: %s", what, strerror(rc < 0 ? -rc : rc));
}

static int err_from_bus(vh_error *err, sd_bus_error *be, int rc, const char *what)
{
    if (err) {
        snprintf(err->name, sizeof(err->name), "%s", be->name ? be->name : "");
        if (be->message)
            snprintf(err->message, sizeof(err->message), "%s", be->message);
        else
            snprintf(err->message, sizeof(err->message), "%s: %s", what, strerror(rc < 0 ? -rc : rc));
    }
    sd_bus_error_free(be);
    return rc < 0 ? rc : -EIO;
}

bool vh_error_is(const vh_error *err, const char *dbus_error_name)
{
    return err && dbus_error_name && strcmp(err->name, dbus_error_name) == 0;
}

int vh_connect(vh_client **out, bool session_bus, vh_error *err)
{
    vh_client *c;
    int r;

    err_clear(err);
    *out = NULL;
    c = calloc(1, sizeof(*c));
    if (!c) {
        err_local(err, ENOMEM, "allocate client");
        return -ENOMEM;
    }
    r = session_bus ? sd_bus_open_user(&c->bus) : sd_bus_open_system(&c->bus);
    if (r < 0) {
        err_local(err, r, session_bus ? "connect to the session bus" : "connect to the system bus");
        free(c);
        return r;
    }
    *out = c;
    return 0;
}

void vh_disconnect(vh_client *c)
{
    if (!c)
        return;
    if (c->bus)
        sd_bus_unref(c->bus);
    free(c);
}

int vh_is_available(vh_client *c, bool *available, vh_error *err)
{
    sd_bus_error be = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int has = 0, r;

    err_clear(err);
    *available = false;
    r = sd_bus_call_method(c->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "NameHasOwner", &be, &reply, "s", VH_BUS_NAME);
    if (r < 0)
        return err_from_bus(err, &be, r, "NameHasOwner");
    sd_bus_message_read(reply, "b", &has);
    sd_bus_message_unref(reply);
    *available = has != 0;
    return 0;
}

/* --- one-string-out calls -------------------------------------------------- */

static int call_string_out(vh_client *c, const char *method, char *json, size_t cap, vh_error *err)
{
    sd_bus_error be = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *s = NULL;
    int r;

    err_clear(err);
    r = sd_bus_call_method(c->bus, VH_BUS_NAME, VH_BUS_PATH, VH_IFACE, method, &be, &reply, "");
    if (r < 0)
        return err_from_bus(err, &be, r, method);
    r = sd_bus_message_read(reply, "s", &s);
    if (r >= 0)
        snprintf(json, cap, "%s", s ? s : "");
    sd_bus_message_unref(reply);
    return r < 0 ? r : 0;
}

int vh_get_devices(vh_client *c, char *json, size_t cap, vh_error *err)
{
    return call_string_out(c, "GetDevices", json, cap, err);
}

int vh_get_capabilities(vh_client *c, char *json, size_t cap, vh_error *err)
{
    return call_string_out(c, "GetCapabilities", json, cap, err);
}

/* --- no-reply calls -------------------------------------------------------- */

static int call_void(vh_client *c, const char *method, vh_error *err, const char *types, ...)
{
    sd_bus_error be = SD_BUS_ERROR_NULL;
    sd_bus_message *req = NULL, *reply = NULL;
    va_list ap;
    int r;

    err_clear(err);
    r = sd_bus_message_new_method_call(c->bus, &req, VH_BUS_NAME, VH_BUS_PATH, VH_IFACE, method);
    if (r < 0) {
        err_local(err, r, method);
        return r;
    }
    if (types && *types) {
        va_start(ap, types);
        r = sd_bus_message_appendv(req, types, ap);
        va_end(ap);
        if (r < 0) {
            sd_bus_message_unref(req);
            err_local(err, r, method);
            return r;
        }
    }
    r = sd_bus_call(c->bus, req, 0, &be, &reply);
    sd_bus_message_unref(req);
    if (r < 0)
        return err_from_bus(err, &be, r, method);
    sd_bus_message_unref(reply);
    return 0;
}

int vh_enable(vh_client *c, bool enable, vh_error *err)
{
    return call_void(c, "Enable", err, "b", (int)enable);
}

int vh_set_context(vh_client *c, const char *context_json, vh_error *err)
{
    return call_void(c, "SetContext", err, "s", context_json);
}

int vh_set_rules(vh_client *c, const char *rules_json, vh_error *err)
{
    return call_void(c, "SetRules", err, "s", rules_json);
}

int vh_set_fan_pwm(vh_client *c, const char *hwmon, uint32_t channel, uint8_t value, vh_error *err)
{
    return call_void(c, "SetFanPwm", err, "suy", hwmon, channel, value);
}

int vh_set_fan_auto(vh_client *c, const char *hwmon, uint32_t channel, vh_error *err)
{
    return call_void(c, "SetFanAuto", err, "su", hwmon, channel);
}

int vh_fan_heartbeat(vh_client *c, vh_error *err)
{
    return call_void(c, "FanHeartbeat", err, "");
}

int vh_ddc_write(vh_client *c, const char *bus, uint8_t vcp, uint16_t value, vh_error *err)
{
    return call_void(c, "DdcWrite", err, "syq", bus, vcp, value);
}

int vh_ddc_read(vh_client *c, const char *bus, uint8_t vcp, uint16_t *current, uint16_t *max,
                vh_error *err)
{
    sd_bus_error be = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    uint16_t cur = 0, mx = 0;
    int r;

    err_clear(err);
    r = sd_bus_call_method(c->bus, VH_BUS_NAME, VH_BUS_PATH, VH_IFACE, "DdcRead", &be, &reply, "sy",
                           bus, vcp);
    if (r < 0)
        return err_from_bus(err, &be, r, "DdcRead");
    r = sd_bus_message_read(reply, "qq", &cur, &mx);
    sd_bus_message_unref(reply);
    if (r < 0)
        return r;
    if (current)
        *current = cur;
    if (max)
        *max = mx;
    return 0;
}

int vh_get_property(vh_client *c, const char *name, char *value, size_t cap, vh_error *err)
{
    sd_bus_error be = SD_BUS_ERROR_NULL;
    char *v = NULL;
    int r;

    err_clear(err);
    r = sd_bus_get_property_string(c->bus, VH_BUS_NAME, VH_BUS_PATH, VH_IFACE, name, &be, &v);
    if (r < 0)
        return err_from_bus(err, &be, r, name);
    snprintf(value, cap, "%s", v ? v : "");
    free(v);
    return 0;
}

static int on_event_message(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    vh_client *c = userdata;
    uint64_t ts;
    const char *kind = NULL, *detail = NULL;
    uint32_t device;
    uint16_t type, code;
    int32_t value;
    int r;

    (void)ret_error;
    r = sd_bus_message_read(m, "tsuqqis", &ts, &kind, &device, &type, &code, &value, &detail);
    if (r < 0)
        return r;
    if (c->on_event)
        c->on_event(ts, kind ? kind : "", device, type, code, value, detail ? detail : "",
                    c->on_event_user);
    return 0;
}

int vh_subscribe_events(vh_client *c, vh_event_fn fn, void *user, vh_error *err)
{
    int r;

    err_clear(err);
    c->on_event = fn;
    c->on_event_user = user;
    r = sd_bus_match_signal(c->bus, NULL, VH_BUS_NAME, VH_BUS_PATH, VH_IFACE, "Event",
                            on_event_message, c);
    if (r < 0) {
        err_local(err, r, "subscribe to Event");
        return r;
    }
    return 0;
}

int vh_process(vh_client *c, uint64_t timeout_us, vh_error *err)
{
    int r;

    err_clear(err);
    r = sd_bus_process(c->bus, NULL);
    if (r < 0) {
        err_local(err, r, "sd_bus_process");
        return r;
    }
    if (r > 0)
        return r;
    r = sd_bus_wait(c->bus, timeout_us);
    if (r < 0) {
        err_local(err, r, "sd_bus_wait");
        return r;
    }
    return 0;
}
