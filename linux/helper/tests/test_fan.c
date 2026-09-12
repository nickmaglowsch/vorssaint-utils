/* Fan control against a fake hwmon tree.
 *
 * There is no /sys/class/hwmon in the container this was written in, so the
 * tree below is built in a temporary directory: plain files with the same
 * names and the same read-write behaviour sysfs presents. What that does and
 * does not prove is stated in PRIVILEGES.md § 8 -- the path building, the
 * read-back and the watchdog are real; a driver that clamps, a chip with no
 * pwmN_enable, and the EC on a particular laptop are not.
 *
 * One thing this environment cannot model with file modes: the helper runs as
 * root, and root bypasses the permission bits, so a mode-0444 file is still
 * writable to it. (That is not a gap in the test so much as the truth about
 * the daemon: "writable" in GetCapabilities means writable *by the helper*.)
 * A driver that refuses or ignores a store is therefore modelled with device
 * nodes, which the kernel refuses regardless of uid:
 *
 *   pwm -> /dev/full   the write itself fails (ENOSPC)
 *   pwm -> /dev/zero   the write succeeds and the value does not stick, which
 *                      is exactly what read-back verification exists to catch
 */
#include "fan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static char ROOT[] = "/tmp/vorssaint-fan-test-XXXXXX";

static void ok(const char *name) { printf("  %-52s ok\n", name); }
static void fail(const char *name, const char *why)
{
    printf("  FAIL %s: %s\n", name, why);
    failures++;
}

static void put(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
    if (chmod(path, mode) != 0)
        perror(path);
}

static long get(const char *path)
{
    char buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, sizeof(buf), f))
        buf[0] = '\0';
    fclose(f);
    return strtol(buf, NULL, 10);
}

static void build_tree(void)
{
    char p[512];

    if (!mkdtemp(ROOT)) {
        perror("mkdtemp");
        exit(1);
    }
    /* hwmon0: a normal controllable fan, currently on the chip's curve. */
    snprintf(p, sizeof(p), "%s/hwmon0", ROOT);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/hwmon0/name", ROOT);
    put(p, "nct6798\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon0/pwm1", ROOT);
    put(p, "120\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon0/pwm1_enable", ROOT);
    put(p, "2\n", 0644);

    /* hwmon1: no pwm at all -- a temperature sensor, the common case. */
    snprintf(p, sizeof(p), "%s/hwmon1", ROOT);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/hwmon1/name", ROOT);
    put(p, "coretemp\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon1/temp1_input", ROOT);
    put(p, "42000\n", 0644);

    /* hwmon2: a driver that refuses the store. /dev/full fails every write
     * with ENOSPC for any uid, which no file mode can do to root. */
    snprintf(p, sizeof(p), "%s/hwmon2", ROOT);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/hwmon2/name", ROOT);
    put(p, "acpitz\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon2/pwm1_enable", ROOT);
    put(p, "2\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon2/pwm1", ROOT);
    if (symlink("/dev/full", p) != 0)
        perror(p);

    /* hwmon3: a driver that accepts the store and ignores it -- the quiet
     * failure the read-back is for. Writes to /dev/zero succeed and read back
     * as zeroes, so the value never becomes what was asked for. */
    snprintf(p, sizeof(p), "%s/hwmon3", ROOT);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/hwmon3/name", ROOT);
    put(p, "it87\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon3/pwm1_enable", ROOT);
    put(p, "2\n", 0644);
    snprintf(p, sizeof(p), "%s/hwmon3/pwm1", ROOT);
    if (symlink("/dev/zero", p) != 0)
        perror(p);
}

static void rm_tree(void)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", ROOT);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", ROOT);
}

#define MS(x) ((uint64_t)(x) * 1000000ULL)

int main(void)
{
    fan_state st;
    char err[512];
    char p_pwm[512], p_en[512];
    uint64_t T = MS(1000);

    build_tree();
    snprintf(p_pwm, sizeof(p_pwm), "%s/hwmon0/pwm1", ROOT);
    snprintf(p_en, sizeof(p_en), "%s/hwmon0/pwm1_enable", ROOT);

    printf("hwmon name validation (the path is built from untrusted input)\n");
    {
        struct {
            const char *name;
            bool want;
        } cases[] = {
            {"hwmon0", true},   {"hwmon12", true},   {"hwmon", false},
            {"hwmon0/..", false}, {"../../etc", false}, {"hwmonX", false},
            {"hwmon0 ", false}, {"", false},
        };
        bool all = true;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (fan_hwmon_name_ok(cases[i].name) != cases[i].want) {
                char why[128];
                snprintf(why, sizeof(why), "'%s' judged %s", cases[i].name,
                         cases[i].want ? "bad" : "good");
                fail("only hwmonN is accepted", why);
                all = false;
            }
        }
        if (all)
            ok("only hwmonN is accepted; traversal is rejected");
    }
    fan_init(&st, ROOT);
    if (fan_set_pwm(&st, "hwmon0/../hwmon2", 1, 200, T, err, sizeof(err)) == -EINVAL)
        ok("fan_set_pwm rejects a traversal before touching the filesystem");
    else
        fail("fan_set_pwm rejects a traversal before touching the filesystem", err);

    printf("\nsetting a duty cycle\n");
    fan_init(&st, ROOT);
    if (fan_set_pwm(&st, "hwmon0", 1, 200, T, err, sizeof(err)) == 0)
        ok("SetFanPwm writes the value");
    else
        fail("SetFanPwm writes the value", err);

    if (get(p_pwm) == 200)
        ok("the value is in pwm1, read back from the tree");
    else
        fail("the value is in pwm1", "wrong value");

    /* Order matters: manual mode must be set before the duty cycle, or the
     * chip's curve overwrites it at the next update. */
    if (get(p_en) == PWM_ENABLE_MANUAL)
        ok("pwm1_enable was put into manual mode (1)");
    else
        fail("pwm1_enable was put into manual mode (1)", "still on the chip's curve");

    printf("\nread-back verification\n");
    if (fan_set_pwm(&st, "hwmon2", 1, 100, T, err, sizeof(err)) < 0 && strstr(err, "write"))
        ok("a driver that refuses the write is reported, not swallowed");
    else
        fail("a driver that refuses the write is reported, not swallowed", err);

    /* The one that matters. A store that succeeds and does not stick looks
     * exactly like success from the write's return value alone; only reading
     * the attribute back tells the difference, and telling the user their fan
     * is at 30 %% when the driver ignored them is how hardware cooks.
     *
     * There are two shapes of this and write_verified() rejects both: the
     * attribute reads back a *different* number, and the attribute cannot be
     * read back at all. /dev/zero gives the second (its bytes are not a
     * number), so that is the one exercised here; the first is the branch one
     * line below it in fan.c and has no model in a container. What is
     * asserted is the invariant they share -- the helper refuses to report a
     * duty cycle it has not read back. */
    if (fan_set_pwm(&st, "hwmon3", 1, 100, T, err, sizeof(err)) < 0 &&
        strstr(err, "read back"))
        ok("a write that succeeds but does not stick is caught by the read-back");
    else
        fail("a write that succeeds but does not stick is caught by the read-back", err);

    /* And it must not leave that channel in manual mode at an unknown duty
     * cycle: we have just proved the write path is broken for it. */
    {
        char p[512];
        long en;
        snprintf(p, sizeof(p), "%s/hwmon3/pwm1_enable", ROOT);
        en = get(p);
        if (en == 2 && st.n_ch == 1)
            ok("and the channel is handed straight back to automatic control");
        else
            fail("and the channel is handed straight back to automatic control", "left manual");
    }

    if (fan_set_pwm(&st, "hwmon1", 1, 100, T, err, sizeof(err)) < 0)
        ok("a hwmon with no pwm channel is reported");
    else
        fail("a hwmon with no pwm channel is reported", "accepted");

    printf("\nthe watchdog restores automatic control\n");
    fan_init(&st, ROOT);
    put(p_en, "2\n", 0644);
    if (fan_set_pwm(&st, "hwmon0", 1, 64, T, err, sizeof(err)) != 0)
        fail("arm the watchdog", err);

    if (fan_watchdog_tick(&st, T + FAN_WATCHDOG_NS - 1) == 0 && get(p_en) == PWM_ENABLE_MANUAL)
        ok("one nanosecond before the deadline nothing is restored");
    else
        fail("one nanosecond before the deadline nothing is restored", "restored early");

    /* A heartbeat inside the window must push the deadline out, or a client
     * that is alive and well loses control of its own fans every 10 s. */
    if (fan_heartbeat(&st, T + MS(9000)) == 0 &&
        fan_watchdog_tick(&st, T + FAN_WATCHDOG_NS + MS(500)) == 0)
        ok("a heartbeat inside the window defers the restore");
    else
        fail("a heartbeat inside the window defers the restore", "restored anyway");

    if (fan_watchdog_tick(&st, T + MS(9000) + FAN_WATCHDOG_NS) == 1)
        ok("10 s after the last heartbeat the channel is restored");
    else
        fail("10 s after the last heartbeat the channel is restored", "not restored");

    /* Restored to what the machine was doing before, not to a fixed 2. */
    if (get(p_en) == 2)
        ok("pwm1_enable is back to the value it had before (2)");
    else
        fail("pwm1_enable is back to the value it had before (2)", "wrong value");

    if (st.n_ch == 0 && st.deadline_ns == 0)
        ok("nothing is left under manual control");
    else
        fail("nothing is left under manual control", "channels still tracked");

    if (fan_heartbeat(&st, T) == -ENOENT)
        ok("heartbeating with nothing armed tells the client to re-apply");
    else
        fail("heartbeating with nothing armed tells the client to re-apply", "accepted");

    printf("\nthe pre-existing mode is what gets restored\n");
    fan_init(&st, ROOT);
    put(p_en, "0\n", 0644); /* this board ran its fans at full speed */
    if (fan_set_pwm(&st, "hwmon0", 1, 90, T, err, sizeof(err)) != 0)
        fail("set pwm on a full-speed board", err);
    if (fan_set_auto(&st, "hwmon0", 1, err, sizeof(err)) == 0 && get(p_en) == 0)
        ok("a board that was on full speed goes back to full speed, not to a curve");
    else
        fail("a board that was on full speed goes back to full speed", err);

    printf("\nexplicit restore-all (Enable(false), SIGTERM, exit)\n");
    fan_init(&st, ROOT);
    put(p_en, "2\n", 0644);
    fan_set_pwm(&st, "hwmon0", 1, 30, T, err, sizeof(err));
    if (fan_restore_all(&st) == 1 && get(p_en) == 2 && st.n_ch == 0)
        ok("fan_restore_all hands every channel back");
    else
        fail("fan_restore_all hands every channel back", "channel left in manual mode");

    printf("\nenumeration for GetCapabilities\n");
    {
        char json[4096];
        fan_enumerate_json(ROOT, json, sizeof(json));
        printf("  %s\n", json);
        if (strstr(json, "\"hwmon\":\"hwmon0\"") && strstr(json, "\"name\":\"nct6798\""))
            ok("the controllable chip is listed by name");
        else
            fail("the controllable chip is listed by name", json);
        if (strstr(json, "\"hwmon\":\"hwmon1\",\"name\":\"coretemp\",\"pwm\":[]"))
            ok("a chip with no pwm is listed with an empty channel list");
        else
            fail("a chip with no pwm is listed with an empty channel list", json);
        if (strstr(json, "\"hwmon\":\"hwmon2\"") && strstr(json, "\"hwmon\":\"hwmon3\""))
            ok("every chip with a pwm attribute is listed");
        else
            fail("every chip with a pwm attribute is listed", json);
        /* `writable` is access(2) as the helper, and the helper is root, so on
         * this machine nothing comes back false. The field is still the right
         * one for the capabilities page -- it answers "can the helper write
         * this", which is the question -- but it cannot be exercised here. */
        if (geteuid() == 0)
            printf("  (running as root: access(W_OK) cannot return false here, "
                   "so \"writable\":false is unexercised)\n");
    }
    {
        char json[256];
        fan_enumerate_json("/sys/class/hwmon", json, sizeof(json));
        printf("  real /sys/class/hwmon on this machine: %s\n", json);
    }

    rm_tree();
    printf("\n%s\n", failures ? "FAILURES" : "all fan tests passed");
    return failures ? 1 : 0;
}
