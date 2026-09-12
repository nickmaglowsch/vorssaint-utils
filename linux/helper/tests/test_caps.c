/* GetCapabilities, and what it says about *this* machine.
 *
 * The assertions here are about shape and honesty rather than about values:
 * the whole point of the method is that the values differ per machine. What
 * must hold everywhere is that every field is present, that an absent
 * facility is reported as absent with the errno that established it, and
 * that nothing is claimed available because a name exists.
 *
 * It also prints the real answer for this container, which is the evidence
 * PRIVILEGES.md § 8 cites.
 */
#include "caps.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void ok(const char *name) { printf("  %-52s ok\n", name); }
static void fail(const char *name, const char *why)
{
    printf("  FAIL %s: %s\n", name, why);
    failures++;
}

static void need(const char *json, const char *needle, const char *what)
{
    if (strstr(json, needle))
        ok(what);
    else
        fail(what, needle);
}

int main(void)
{
    caps_paths p;
    char json[8192];

    caps_paths_defaults(&p);
    caps_to_json(&p, json, sizeof(json));

    printf("GetCapabilities on this machine\n");
    printf("  %s\n\n", json);

    need(json, "\"uinput\":", "the uinput section is present");
    need(json, "\"hwmon\":", "the hwmon section is present");
    need(json, "\"i2c\":", "the i2c section is present");
    need(json, "\"input\":", "the input section is present");
    need(json, "\"hardware_tested\":false",
         "DDC is marked as never having been tested on hardware");

    /* A path that does not exist must come back unavailable with the errno,
     * never as a bare false: "No such file or directory" and "No such device"
     * are different problems with different fixes, and the second is what a
     * kernel built without CONFIG_INPUT_UINPUT gives for a node created by
     * hand. */
    {
        caps_paths q = p;
        char j2[8192];
        q.uinput_path = "/dev/vorssaint-no-such-node";
        caps_to_json(&q, j2, sizeof(j2));
        if (strstr(j2, "\"available\":false") && strstr(j2, "No such file or directory") &&
            strstr(j2, "errno 2"))
            ok("an absent uinput reports the errno that established it");
        else
            fail("an absent uinput reports the errno that established it", j2);
    }

    {
        caps_paths q = p;
        char j2[8192];
        q.hwmon_root = "/vorssaint-no-such-root";
        q.i2c_dev_root = "/vorssaint-no-such-root";
        caps_to_json(&q, j2, sizeof(j2));
        if (strstr(j2, "\"devices\":[]") && strstr(j2, "\"buses\":[]"))
            ok("absent hwmon and i2c roots are empty lists, not errors");
        else
            fail("absent hwmon and i2c roots are empty lists, not errors", j2);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all capability tests passed");
    return failures ? 1 : 0;
}
