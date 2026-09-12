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
 *
 * A driver that refuses the store is therefore modelled with a read-only
 * tmpfs, which refuses the write regardless of uid while leaving the
 * attribute a *regular file* -- which it must be, because the symlink guard
 * added for the WP-S1 review requires sysfs attributes to be regular files.
 * An earlier revision modelled this with symlinks to /dev/full and /dev/zero;
 * the guard now refuses those, correctly, and the fixture moved rather than
 * the guard. The cost of that trade is noted where it falls, below.
 *
 * If the mount is unavailable (an unprivileged CI runner), that one block
 * skips and says so rather than silently passing.
 */
#include "fan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
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

static bool have_ro_mount;

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

    /* hwmon2: a driver that refuses the store. A read-only tmpfs refuses the
     * write for any uid -- which no file mode can do to root -- while keeping
     * the attribute a regular file, as sysfs attributes are. */
    snprintf(p, sizeof(p), "%s/hwmon2", ROOT);
    mkdir(p, 0755);
    if (mount("tmpfs", p, "tmpfs", 0, "size=64k") == 0) {
        snprintf(p, sizeof(p), "%s/hwmon2/name", ROOT);
        put(p, "acpitz\n", 0644);
        snprintf(p, sizeof(p), "%s/hwmon2/pwm1_enable", ROOT);
        put(p, "2\n", 0644);
        snprintf(p, sizeof(p), "%s/hwmon2/pwm1", ROOT);
        put(p, "255\n", 0644);
        snprintf(p, sizeof(p), "%s/hwmon2", ROOT);
        if (mount(NULL, p, NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
            have_ro_mount = true;
        else
            perror("remount ro");
    }
}

static void rm_tree(void)
{
    char cmd[600];

    if (have_ro_mount) {
        snprintf(cmd, sizeof(cmd), "%s/hwmon2", ROOT);
        if (umount(cmd) != 0)
            perror("umount");
    }
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

    printf("\nsymlinked hwmon entries (QA: the name was valid, the tree was not)\n");
    {
        /* The name `hwmon7` passes fan_hwmon_name_ok, because nothing is wrong
         * with the name. What is wrong is that the entry is a link out of the
         * root, and before the pathguard check this wrote through it and
         * returned 0. See pathguard.h for why the entry may be a symlink in
         * general (sysfs makes it one) but not one that leaves the tree. */
        char outside[512], link[600], victim[700];
        long before_val;

        snprintf(outside, sizeof(outside), "%s.outside", ROOT);
        mkdir(outside, 0755);
        snprintf(victim, sizeof(victim), "%s/pwm1", outside);
        put(victim, "17\n", 0644);
        snprintf(victim, sizeof(victim), "%s/pwm1_enable", outside);
        put(victim, "2\n", 0644);
        snprintf(link, sizeof(link), "%s/hwmon7", ROOT);
        if (symlink(outside, link) != 0)
            perror(link);

        snprintf(victim, sizeof(victim), "%s/pwm1", outside);
        before_val = get(victim);

        fan_init(&st, ROOT);
        if (fan_set_pwm(&st, "hwmon7", 1, 200, T, err, sizeof(err)) == -ELOOP)
            ok("a hwmon entry linked out of the root is refused with ELOOP");
        else
            fail("a hwmon entry linked out of the root is refused with ELOOP", err);
        if (strstr(err, "outside"))
            ok("and the refusal says where it would have landed");
        else
            fail("and the refusal says where it would have landed", err);
        if (get(victim) == before_val)
            ok("and nothing was written through it");
        else
            fail("and nothing was written through it", "the value changed");
        if (st.n_ch == 0)
            ok("and no channel was recorded as under manual control");
        else
            fail("and no channel was recorded as under manual control", "channel tracked");

        /* SetFanAuto is the restore path and must refuse identically, or a
         * refused set could be "restored" through the link. */
        if (fan_set_auto(&st, "hwmon7", 1, err, sizeof(err)) == -ELOOP)
            ok("SetFanAuto refuses the same entry");
        else
            fail("SetFanAuto refuses the same entry", err);

        /* A symlinked *attribute* is the other half. sysfs attributes are
         * regular files, never links, so O_NOFOLLOW refuses this outright. */
        snprintf(link, sizeof(link), "%s/hwmon8", ROOT);
        mkdir(link, 0755);
        snprintf(victim, sizeof(victim), "%s/hwmon8/pwm1_enable", ROOT);
        put(victim, "2\n", 0644);
        snprintf(victim, sizeof(victim), "%s/hwmon8/pwm1", ROOT);
        snprintf(outside, sizeof(outside), "%s.outside/pwm1", ROOT);
        if (symlink(outside, victim) != 0)
            perror(victim);
        before_val = get(outside);
        if (fan_set_pwm(&st, "hwmon8", 1, 200, T, err, sizeof(err)) == -ELOOP &&
            strstr(err, "symbolic link"))
            ok("a pwm attribute that is a symlink is refused with ELOOP");
        else
            fail("a pwm attribute that is a symlink is refused with ELOOP", err);
        if (get(outside) == before_val)
            ok("and nothing was written through that either");
        else
            fail("and nothing was written through that either", "the value changed");

        /* The enumeration must not advertise what the write path will refuse. */
        {
            char json[4096];
            fan_enumerate_json(ROOT, json, sizeof(json));
            if (!strstr(json, "hwmon7"))
                ok("GetCapabilities does not list an out-of-boundary entry");
            else
                fail("GetCapabilities does not list an out-of-boundary entry", json);
        }

        /* Clean up so the later enumeration assertions see the original tree. */
        snprintf(link, sizeof(link), "%s/hwmon7", ROOT);
        unlink(link);
        snprintf(link, sizeof(link), "rm -rf '%s/hwmon8' '%s.outside'", ROOT, ROOT);
        if (system(link) != 0)
            fprintf(stderr, "cleanup failed\n");
    }

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
    if (have_ro_mount) {
        size_t before = st.n_ch;
        if (fan_set_pwm(&st, "hwmon2", 1, 100, T, err, sizeof(err)) < 0 &&
            strstr(err, "Read-only file system"))
            ok("a driver that refuses the write is reported, not swallowed");
        else
            fail("a driver that refuses the write is reported, not swallowed", err);
        if (st.n_ch == before)
            ok("and the channel is not left recorded as under manual control");
        else
            fail("and the channel is not left recorded as under manual control", "tracked");
    } else {
        printf("  SKIP a driver that refuses the write: no mount privilege here\n");
    }

    /* The branch this fixture no longer reaches, stated rather than implied.
     *
     * write_verified() rejects two shapes of a store that looked like it
     * worked: the attribute reads back a *different* number, and the
     * attribute cannot be read back at all. Both need an attribute that is
     * not an ordinary file, and the symlink guard added in the WP-S1 review
     * requires sysfs attributes to be ordinary files -- correctly, because
     * that is what they are. So neither shape has a model here any more.
     *
     * That is the right trade: the guard closes a hole QA demonstrated live,
     * and the branch it costs is still exercised on every successful write,
     * where the read-back is what the assertion below actually reads. It is
     * recorded in PRIVILEGES.md section 8 as untested rather than quietly
     * dropped. */
    printf("  NOTE the read-back mismatch branch has no model in this container;\n");
    printf("       see PRIVILEGES.md section 8. The read-back itself runs on\n");
    printf("       every successful write and is asserted above.\n");

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
        if (!have_ro_mount || strstr(json, "\"hwmon\":\"hwmon2\""))
            ok("every chip with a pwm attribute is listed");
        else
            fail("every chip with a pwm attribute is listed", json);
        /* `writable` is access(2) as the helper, which answers the question the
         * capabilities page is actually asking: can the *helper* write this.
         * As root no file mode makes it false, but a read-only filesystem
         * does, so the hwmon2 fixture exercises it for real. */
        if (have_ro_mount) {
            if (strstr(json, "\"writable\":false"))
                ok("a pwm the helper cannot write is reported as not writable");
            else
                fail("a pwm the helper cannot write is reported as not writable", json);
        } else {
            printf("  SKIP \"writable\":false needs the read-only mount\n");
        }
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
