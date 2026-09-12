/* vorssaint-helperctl: the harness for the client library.
 *
 * Every subcommand is one call on vorssaint_helper_client.h and nothing else,
 * so a harness run is a test of the same code path the app takes. It prints
 * the D-Bus error *name* on failure, because that is what the capabilities
 * hub branches on and what the private-bus scenario asserts.
 */
#include "vorssaint_helper_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const vh_error *err, const char *what)
{
    fprintf(stderr, "helperctl: %s failed: %s: %s\n", what, err->name[0] ? err->name : "(local)",
            err->message);
    return 2;
}

static void on_event(uint64_t ts, uint16_t type, uint16_t code, int32_t value, void *user)
{
    (void)user;
    printf("Event ts=%llu type=%u code=%u value=%d\n", (unsigned long long)ts, type, code, value);
    fflush(stdout);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vorssaint-helperctl [--session-bus] <command>\n"
            "  available               is the helper on the bus\n"
            "  get-devices             GetDevices()\n"
            "  get-capabilities        GetCapabilities()\n"
            "  enable | disable        Enable(b)\n"
            "  set-rules <json>        SetRules(s)\n"
            "  get <property>          Rules | Backend | Authorization | Fan | Owner\n"
            "  fan-pwm <hwmon> <ch> <v>  SetFanPwm(s,u,y)\n"
            "  fan-auto <hwmon> <ch>     SetFanAuto(s,u)\n"
            "  fan-heartbeat             FanHeartbeat()\n"
            "  ddc-write <bus> <vcp> <v> DdcWrite(s,y,q), vcp in hex, e.g. 10\n"
            "  ddc-read <bus> <vcp>      DdcRead(s,y) -> current, max\n"
            "  listen [seconds]          subscribe to Event\n");
}

int main(int argc, char **argv)
{
    vh_error err = {{0}, {0}};
    vh_client *c = NULL;
    bool session_bus = false;
    int argi = 1;
    const char *cmd;
    int rc = 0;

    if (argi < argc && !strcmp(argv[argi], "--session-bus")) {
        session_bus = true;
        argi++;
    }
    if (argi >= argc) {
        usage();
        return 1;
    }
    cmd = argv[argi++];

    if (vh_connect(&c, session_bus, &err) < 0)
        return fail(&err, "connect");
    fprintf(stderr, "helperctl: uid=%u connected to the %s bus\n", (unsigned)geteuid(),
            session_bus ? "session" : "system");

#define NEED(n)                                                                                    \
    do {                                                                                           \
        if (argc - argi < (n)) {                                                                   \
            usage();                                                                               \
            vh_disconnect(c);                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

    if (!strcmp(cmd, "available")) {
        bool avail = false;
        if (vh_is_available(c, &avail, &err) < 0)
            rc = fail(&err, "available");
        else
            printf("available = %s\n", avail ? "yes" : "no");
    } else if (!strcmp(cmd, "get-devices")) {
        char json[8192];
        if (vh_get_devices(c, json, sizeof(json), &err) < 0)
            rc = fail(&err, "GetDevices");
        else
            printf("GetDevices -> %s\n", json);
    } else if (!strcmp(cmd, "get-capabilities")) {
        char json[8192];
        if (vh_get_capabilities(c, json, sizeof(json), &err) < 0)
            rc = fail(&err, "GetCapabilities");
        else
            printf("GetCapabilities -> %s\n", json);
    } else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        bool on = !strcmp(cmd, "enable");
        if (vh_enable(c, on, &err) < 0)
            rc = fail(&err, "Enable");
        else
            printf("Enable(%s) -> ok\n", on ? "true" : "false");
    } else if (!strcmp(cmd, "set-rules")) {
        NEED(1);
        if (vh_set_rules(c, argv[argi], &err) < 0)
            rc = fail(&err, "SetRules");
        else
            printf("SetRules -> ok\n");
    } else if (!strcmp(cmd, "get")) {
        char val[4096];
        NEED(1);
        if (vh_get_property(c, argv[argi], val, sizeof(val), &err) < 0)
            rc = fail(&err, argv[argi]);
        else
            printf("%s = %s\n", argv[argi], val);
    } else if (!strcmp(cmd, "fan-pwm")) {
        NEED(3);
        if (vh_set_fan_pwm(c, argv[argi], (uint32_t)strtoul(argv[argi + 1], NULL, 10),
                           (uint8_t)strtoul(argv[argi + 2], NULL, 10), &err) < 0)
            rc = fail(&err, "SetFanPwm");
        else
            printf("SetFanPwm(%s,%s,%s) -> ok\n", argv[argi], argv[argi + 1], argv[argi + 2]);
    } else if (!strcmp(cmd, "fan-auto")) {
        NEED(2);
        if (vh_set_fan_auto(c, argv[argi], (uint32_t)strtoul(argv[argi + 1], NULL, 10), &err) < 0)
            rc = fail(&err, "SetFanAuto");
        else
            printf("SetFanAuto(%s,%s) -> ok\n", argv[argi], argv[argi + 1]);
    } else if (!strcmp(cmd, "fan-heartbeat")) {
        if (vh_fan_heartbeat(c, &err) < 0)
            rc = fail(&err, "FanHeartbeat");
        else
            printf("FanHeartbeat -> ok\n");
    } else if (!strcmp(cmd, "ddc-write")) {
        NEED(3);
        if (vh_ddc_write(c, argv[argi], (uint8_t)strtoul(argv[argi + 1], NULL, 16),
                         (uint16_t)strtoul(argv[argi + 2], NULL, 10), &err) < 0)
            rc = fail(&err, "DdcWrite");
        else
            printf("DdcWrite(%s,0x%s,%s) -> ok\n", argv[argi], argv[argi + 1], argv[argi + 2]);
    } else if (!strcmp(cmd, "ddc-read")) {
        uint16_t cur = 0, max = 0;
        NEED(2);
        if (vh_ddc_read(c, argv[argi], (uint8_t)strtoul(argv[argi + 1], NULL, 16), &cur, &max,
                        &err) < 0)
            rc = fail(&err, "DdcRead");
        else
            printf("DdcRead(%s,0x%s) -> current=%u max=%u\n", argv[argi], argv[argi + 1], cur, max);
    } else if (!strcmp(cmd, "listen")) {
        int seconds = argi < argc ? atoi(argv[argi]) : 3;
        if (vh_subscribe_events(c, on_event, NULL, &err) < 0) {
            rc = fail(&err, "subscribe");
        } else {
            printf("listening for %s.Event for %d s\n", VH_IFACE, seconds);
            fflush(stdout);
            for (int i = 0; i < seconds * 20; i++)
                if (vh_process(c, 50000, &err) < 0)
                    break;
        }
    } else {
        usage();
        rc = 1;
    }

    vh_disconnect(c);
    return rc;
}
