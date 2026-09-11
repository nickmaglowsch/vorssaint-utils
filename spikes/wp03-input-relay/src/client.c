/* vorssaint-relayctl: the unprivileged half.
 *
 * This is what the app links. It holds no device fd, is not in the input
 * group, and runs as the desktop user; everything it can do is one of the
 * three methods on org.vorssaint.Helper1. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define BUS_NAME "org.vorssaint.Helper1"
#define BUS_PATH "/org/vorssaint/Helper1"
#define IFACE "org.vorssaint.Helper1"

static int on_event(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    uint64_t ts;
    uint16_t type, code;
    int32_t value;
    int r;

    (void)userdata;
    (void)err;
    r = sd_bus_message_read(m, "tqqi", &ts, &type, &code, &value);
    if (r < 0)
        return r;
    printf("Event ts=%llu type=%u code=%u value=%d\n", (unsigned long long)ts, type, code, value);
    fflush(stdout);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: vorssaint-relayctl [--session-bus] <command>\n"
                    "  get-devices          call GetDevices()\n"
                    "  set-rules <json>     call SetRules(s)\n"
                    "  enable | disable     call Enable(b)\n"
                    "  get <property>       read Rules | Backend | Authorization\n"
                    "  listen [seconds]     subscribe to the Event signal\n");
}

int main(int argc, char **argv)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    sd_bus *bus = NULL;
    bool session_bus = false;
    int argi = 1, r;
    const char *cmd;

    if (argi < argc && !strcmp(argv[argi], "--session-bus")) {
        session_bus = true;
        argi++;
    }
    if (argi >= argc) {
        usage();
        return 1;
    }
    cmd = argv[argi++];

    r = session_bus ? sd_bus_open_user(&bus) : sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "relayctl: cannot connect to bus: %s\n", strerror(-r));
        return 1;
    }
    fprintf(stderr, "relayctl: uid=%u connected to the %s bus\n", (unsigned)geteuid(),
            session_bus ? "session" : "system");

    if (!strcmp(cmd, "get-devices")) {
        const char *json;
        r = sd_bus_call_method(bus, BUS_NAME, BUS_PATH, IFACE, "GetDevices", &err, &reply, "");
        if (r < 0)
            goto bus_error;
        sd_bus_message_read(reply, "s", &json);
        printf("GetDevices -> %s\n", json);
    } else if (!strcmp(cmd, "set-rules")) {
        if (argi >= argc) {
            usage();
            return 1;
        }
        r = sd_bus_call_method(bus, BUS_NAME, BUS_PATH, IFACE, "SetRules", &err, &reply, "s",
                               argv[argi]);
        if (r < 0)
            goto bus_error;
        printf("SetRules -> ok\n");
    } else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        r = sd_bus_call_method(bus, BUS_NAME, BUS_PATH, IFACE, "Enable", &err, &reply, "b",
                               !strcmp(cmd, "enable"));
        if (r < 0)
            goto bus_error;
        printf("Enable(%s) -> ok\n", !strcmp(cmd, "enable") ? "true" : "false");
    } else if (!strcmp(cmd, "get")) {
        char *val = NULL;
        if (argi >= argc) {
            usage();
            return 1;
        }
        r = sd_bus_get_property_string(bus, BUS_NAME, BUS_PATH, IFACE, argv[argi], &err, &val);
        if (r < 0)
            goto bus_error;
        printf("%s = %s\n", argv[argi], val);
        free(val);
    } else if (!strcmp(cmd, "listen")) {
        int seconds = argi < argc ? atoi(argv[argi]) : 3;
        uint64_t end;
        r = sd_bus_match_signal(bus, NULL, BUS_NAME, BUS_PATH, IFACE, "Event", on_event, NULL);
        if (r < 0) {
            fprintf(stderr, "relayctl: cannot match Event: %s\n", strerror(-r));
            return 1;
        }
        printf("listening for %s.Event for %d s\n", IFACE, seconds);
        fflush(stdout);
        sd_bus_get_timeout(bus, &end);
        for (int i = 0; i < seconds * 20; i++) {
            r = sd_bus_process(bus, NULL);
            if (r < 0)
                break;
            if (r == 0)
                sd_bus_wait(bus, 50000);
        }
    } else {
        usage();
        return 1;
    }

    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return 0;

bus_error:
    fprintf(stderr, "relayctl: call failed: %s: %s\n", err.name ?: "(no name)",
            err.message ?: strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    return 2;
}
