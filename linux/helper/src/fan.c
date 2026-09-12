#include "fan.h"

#include "pathguard.h"

#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fan_init(fan_state *st, const char *root)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->root, sizeof(st->root), "%s", root && *root ? root : "/sys/class/hwmon");
}

bool fan_hwmon_name_ok(const char *name)
{
    size_t digits = 0;

    if (!name || strncmp(name, "hwmon", 5) != 0)
        return false;
    for (const char *p = name + 5; *p; p++) {
        if (!isdigit((unsigned char)*p))
            return false;
        if (++digits > 8)
            return false;
    }
    return digits > 0;
}

/* Resolve <root>/<hwmon> once, through the boundary check in pathguard.c, and
 * build every attribute path from the *resolved* directory. The name has
 * already been validated as hwmonN; this is what stops a `hwmon0` that is a
 * symlink to somewhere else -- the name is not wrong in that case, the tree
 * is. See pathguard.h for why the entry may be a link but the attributes may
 * not. */
static int resolve_hwmon(const fan_state *st, const char *hwmon, char *dir, size_t cap, char *err,
                         size_t err_cap)
{
    return path_guard_dir(st->root, hwmon, dir, cap, err, err_cap);
}

static int read_int_file(const char *dir, const char *attr, long *out)
{
    char buf[64];
    char err[256];
    char *end;
    long v;
    int fd = path_guard_open_leaf(dir, attr, O_RDONLY, err, sizeof(err));
    FILE *f;

    if (fd < 0)
        return fd;
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -errno;
    }
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -EIO;
    }
    fclose(f);
    v = strtol(buf, &end, 10);
    if (end == buf)
        return -EINVAL;
    *out = v;
    return 0;
}

static int write_int_file(const char *dir, const char *attr, long value)
{
    char err[256];
    int fd = path_guard_open_leaf(dir, attr, O_WRONLY | O_TRUNC, err, sizeof(err));
    FILE *f;
    int n;

    if (fd < 0)
        return fd;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -errno;
    }
    n = fprintf(f, "%ld\n", value);
    if (n < 0) {
        fclose(f);
        return -EIO;
    }
    /* fclose is where a deferred sysfs store error surfaces; ignoring it is
     * how a failed write gets reported as a success. */
    if (fclose(f) != 0)
        return -errno;
    return 0;
}

/* Write, then read back. A sysfs store handler can accept the write and
 * clamp, round or silently drop the value; the only way to know what the
 * driver actually holds is to ask it. */
static int write_verified(const char *dir, const char *attr, long value, long *seen, char *err,
                          size_t err_cap)
{
    long got = 0;
    int r;

    r = write_int_file(dir, attr, value);
    if (r < 0) {
        snprintf(err, err_cap, "write %s/%s: %s", dir, attr, strerror(-r));
        return r;
    }
    r = read_int_file(dir, attr, &got);
    if (r < 0) {
        snprintf(err, err_cap, "read back %s/%s: %s", dir, attr, strerror(-r));
        return r;
    }
    if (seen)
        *seen = got;
    if (got != value) {
        snprintf(err, err_cap, "%s/%s did not take the value: wrote %ld, read back %ld", dir, attr,
                 value, got);
        return -EIO;
    }
    return 0;
}

static fan_channel *find_channel(fan_state *st, const char *hwmon, uint32_t channel)
{
    for (size_t i = 0; i < st->n_ch; i++)
        if (st->ch[i].channel == channel && strcmp(st->ch[i].hwmon, hwmon) == 0)
            return &st->ch[i];
    return NULL;
}

int fan_set_pwm(fan_state *st, const char *hwmon, uint32_t channel, uint8_t value, uint64_t now_ns,
                char *err, size_t err_cap)
{
    char dir[PATH_MAX];
    char a_pwm[32], a_en[32];
    char probe[PATH_MAX + 64];
    fan_channel *c;
    long prev_enable = PWM_ENABLE_AUTO;
    int r;

    if (!fan_hwmon_name_ok(hwmon)) {
        snprintf(err, err_cap, "not a hwmon name: '%s' (expected hwmonN)", hwmon ? hwmon : "");
        return -EINVAL;
    }
    if (channel < 1 || channel > 99) {
        snprintf(err, err_cap, "pwm channel %u out of range (1..99)", channel);
        return -EINVAL;
    }
    r = resolve_hwmon(st, hwmon, dir, sizeof(dir), err, err_cap);
    if (r < 0)
        return r;

    snprintf(a_pwm, sizeof(a_pwm), "pwm%u", channel);
    snprintf(a_en, sizeof(a_en), "pwm%u_enable", channel);

    /* Cheap early refusal with a message that names the attribute. The real
     * gate is O_NOFOLLOW inside write_int_file: access(2) as root only tells
     * us the file exists, and there is no TOCTOU window in the open itself. */
    snprintf(probe, sizeof(probe), "%s/%s", dir, a_pwm);
    if (access(probe, W_OK) != 0) {
        snprintf(err, err_cap, "%s is not writable: %s", probe, strerror(errno));
        return -errno;
    }

    c = find_channel(st, hwmon, channel);
    if (!c) {
        if (st->n_ch >= FAN_MAX_TRACKED) {
            snprintf(err, err_cap, "more than %d fan channels under manual control",
                     FAN_MAX_TRACKED);
            return -ENOSPC;
        }
        /* Remember what the machine was doing before we interfered, so the
         * watchdog restores that rather than a guess. A chip with no
         * pwmN_enable at all is manual-only; PWM_ENABLE_AUTO is then the
         * value we would write to restore, and the write below will tell us
         * it cannot be done. */
        if (read_int_file(dir, a_en, &prev_enable) < 0)
            prev_enable = PWM_ENABLE_AUTO;
        c = &st->ch[st->n_ch++];
        memset(c, 0, sizeof(*c));
        snprintf(c->hwmon, sizeof(c->hwmon), "%s", hwmon);
        c->channel = channel;
        c->restore_enable = (int)prev_enable;
    }

    /* Order matters: manual mode first. Writing pwmN while the chip is still
     * on its automatic curve is accepted by most drivers and then overwritten
     * by the firmware at the next update, which looks like a fan that obeys
     * for a second and then drifts back. */
    if (!c->manual) {
        r = write_verified(dir, a_en, PWM_ENABLE_MANUAL, NULL, err, err_cap);
        if (r < 0)
            return r;
        c->manual = true;
    }

    r = write_verified(dir, a_pwm, (long)value, NULL, err, err_cap);
    if (r < 0) {
        /* Do not leave the channel in manual mode at whatever duty cycle it
         * happened to hold: that is the state the watchdog exists to prevent,
         * and here we already know the write path is broken. */
        char ignored[128];
        (void)fan_set_auto(st, hwmon, channel, ignored, sizeof(ignored));
        return r;
    }

    c->applied = value;
    st->stat_writes++;
    st->deadline_ns = now_ns + FAN_WATCHDOG_NS;
    return 0;
}

int fan_set_auto(fan_state *st, const char *hwmon, uint32_t channel, char *err, size_t err_cap)
{
    char dir[PATH_MAX];
    char attr[32];
    fan_channel *c;
    long restore;
    int r;

    if (!fan_hwmon_name_ok(hwmon)) {
        snprintf(err, err_cap, "not a hwmon name: '%s' (expected hwmonN)", hwmon ? hwmon : "");
        return -EINVAL;
    }
    if (channel < 1 || channel > 99) {
        snprintf(err, err_cap, "pwm channel %u out of range (1..99)", channel);
        return -EINVAL;
    }
    r = resolve_hwmon(st, hwmon, dir, sizeof(dir), err, err_cap);
    if (r < 0)
        return r;
    snprintf(attr, sizeof(attr), "pwm%u_enable", channel);

    c = find_channel(st, hwmon, channel);
    /* Restore what was there before, not a fixed 2: a board whose fans were
     * on full (`pwmN_enable` 0) before the app started must go back to full,
     * not to a curve it was not using. */
    restore = c ? c->restore_enable : PWM_ENABLE_AUTO;

    r = write_verified(dir, attr, restore, NULL, err, err_cap);
    if (r < 0)
        return r;

    if (c) {
        c->manual = false;
        /* Drop it from the tracked set: nothing of ours is applied any more. */
        size_t idx = (size_t)(c - st->ch);
        st->ch[idx] = st->ch[st->n_ch - 1];
        st->n_ch--;
    }
    st->stat_restores++;
    if (st->n_ch == 0)
        st->deadline_ns = 0;
    return 0;
}

int fan_heartbeat(fan_state *st, uint64_t now_ns)
{
    if (st->n_ch == 0)
        return -ENOENT;
    st->deadline_ns = now_ns + FAN_WATCHDOG_NS;
    return 0;
}

int fan_watchdog_tick(fan_state *st, uint64_t now_ns)
{
    if (st->deadline_ns == 0 || now_ns < st->deadline_ns)
        return 0;
    st->stat_watchdog_trips++;
    return fan_restore_all(st);
}

int fan_restore_all(fan_state *st)
{
    int restored = 0;

    /* fan_set_auto() compacts the array from the back, so walking it forwards
     * while it shrinks would skip entries. */
    while (st->n_ch > 0) {
        char err[256];
        char hwmon[32];
        uint32_t channel = st->ch[0].channel;
        size_t before = st->n_ch;

        snprintf(hwmon, sizeof(hwmon), "%s", st->ch[0].hwmon);
        if (fan_set_auto(st, hwmon, channel, err, sizeof(err)) == 0) {
            restored++;
        } else {
            fprintf(stderr, "helper: could not restore %s pwm%u to automatic: %s\n", hwmon, channel,
                    err);
        }
        if (st->n_ch == before) {
            /* The restore failed and the channel is still tracked; drop it so
             * this cannot spin, but keep the message above. */
            st->ch[0] = st->ch[st->n_ch - 1];
            st->n_ch--;
        }
    }
    st->deadline_ns = 0;
    return restored;
}

int fan_state_to_json(const fan_state *st, uint64_t now_ns, char *buf, size_t cap)
{
    size_t used = 0;
    int n;

    n = snprintf(buf, cap, "{\"root\":\"%s\",\"watchdog_ms\":%llu,\"seconds_left\":%.1f,"
                           "\"writes\":%llu,\"restores\":%llu,\"watchdog_trips\":%llu,"
                           "\"manual\":[",
                 st->root, (unsigned long long)(FAN_WATCHDOG_NS / 1000000ULL),
                 st->deadline_ns > now_ns ? (double)(st->deadline_ns - now_ns) / 1e9 : 0.0,
                 (unsigned long long)st->stat_writes, (unsigned long long)st->stat_restores,
                 (unsigned long long)st->stat_watchdog_trips);
    if (n < 0)
        return n;
    used = (size_t)n;
    for (size_t i = 0; i < st->n_ch && used < cap; i++) {
        n = snprintf(buf + used, cap - used,
                     "%s{\"hwmon\":\"%s\",\"channel\":%u,\"value\":%u,\"restore_enable\":%d}",
                     i ? "," : "", st->ch[i].hwmon, st->ch[i].channel, st->ch[i].applied,
                     st->ch[i].restore_enable);
        if (n < 0)
            return n;
        used += (size_t)n;
    }
    if (used < cap)
        used += (size_t)snprintf(buf + used, cap - used, "]}");
    return (int)used;
}

/* --- enumeration for GetCapabilities -------------------------------------- */

static void read_str_file(const char *dir, const char *attr, char *out, size_t cap)
{
    char err[256];
    int fd = path_guard_open_leaf(dir, attr, O_RDONLY, err, sizeof(err));
    FILE *f;

    out[0] = '\0';
    if (fd < 0)
        return;
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return;
    }
    if (fgets(out, (int)cap, f)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
        /* The name goes into a JSON string; the helper writes JSON by hand,
         * so anything that would need escaping is dropped rather than
         * escaped. Driver names are [a-z0-9_] in practice. */
        for (char *p = out; *p; p++)
            if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
                *p = '_';
    }
    fclose(f);
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int fan_enumerate_json(const char *root, char *buf, size_t cap)
{
    char *names[64];
    size_t n_names = 0;
    size_t used = 0;
    struct dirent *de;
    DIR *d;
    int n;

    n = snprintf(buf, cap, "[");
    used = n > 0 ? (size_t)n : 0;

    d = opendir(root);
    if (d) {
        while ((de = readdir(d)) != NULL && n_names < 64) {
            if (!fan_hwmon_name_ok(de->d_name))
                continue;
            names[n_names++] = strdup(de->d_name);
        }
        closedir(d);
    }
    /* readdir order is the filesystem's, which is not stable; the
     * capabilities page and its tests want hwmon0, hwmon1, hwmon2. */
    qsort(names, n_names, sizeof(names[0]), cmp_name);

    for (size_t i = 0; i < n_names; i++) {
        char dir[PATH_MAX], label[128], gerr[256];
        bool first_pwm = true;

        /* A hwmon entry that resolves outside the boundary is skipped rather
         * than listed: offering it in the capabilities page would be offering
         * the user a control the write path will then refuse. */
        if (path_guard_dir(root, names[i], dir, sizeof(dir), gerr, sizeof(gerr)) < 0) {
            free(names[i]);
            continue;
        }
        read_str_file(dir, "name", label, sizeof(label));

        if (used < cap)
            used += (size_t)snprintf(buf + used, cap - used,
                                     "%s{\"hwmon\":\"%s\",\"name\":\"%s\",\"pwm\":[", i ? "," : "",
                                     names[i], label);

        for (unsigned ch = 1; ch <= 9 && used < cap; ch++) {
            char a_pwm[32], a_en[32], p_pwm[PATH_MAX + 64], p_en[PATH_MAX + 64];
            long cur = -1, en = -1;

            snprintf(a_pwm, sizeof(a_pwm), "pwm%u", ch);
            snprintf(a_en, sizeof(a_en), "pwm%u_enable", ch);
            snprintf(p_pwm, sizeof(p_pwm), "%s/%s", dir, a_pwm);
            if (access(p_pwm, F_OK) != 0)
                continue;
            snprintf(p_en, sizeof(p_en), "%s/%s", dir, a_en);
            read_int_file(dir, a_pwm, &cur);
            read_int_file(dir, a_en, &en);
            used += (size_t)snprintf(buf + used, cap - used,
                                     "%s{\"channel\":%u,\"writable\":%s,\"value\":%ld,"
                                     "\"enable\":%ld,\"enable_writable\":%s}",
                                     first_pwm ? "" : ",", ch,
                                     access(p_pwm, W_OK) == 0 ? "true" : "false", cur, en,
                                     access(p_en, W_OK) == 0 ? "true" : "false");
            first_pwm = false;
        }
        if (used < cap)
            used += (size_t)snprintf(buf + used, cap - used, "]}");
        free(names[i]);
    }
    if (used < cap)
        used += (size_t)snprintf(buf + used, cap - used, "]");
    return (int)used;
}
