/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Per-process CPU and memory, with the sampling policy
 * `ProcessUsageService.topCPU` uses on macOS:
 *
 *   - cumulative per-process CPU time is read for every visible process
 *     (`utime + stime` here, `proc_pid_rusage` there);
 *   - the percentage is the delta over the wall-clock interval since this
 *     instance's previous call, divided by the machine's total capacity, which
 *     is exactly `MetricFormat.processCPUPercentage`;
 *   - rows below 0.01 % are dropped, as `topCPU` drops them;
 *   - rows are consolidated under the app responsible for them and the heaviest
 *     `limit` survive (`groupedByApp` + `prefix(limit)`).
 *
 * macOS's "responsible process" has no Linux equivalent, so the grouping key is
 * the application identity instead: the `.desktop` file whose `Exec` matches
 * the process, falling back to `comm`. A browser's twenty renderer processes
 * share one `comm` and therefore one row, which is the behaviour the macOS side
 * gets from the responsible-process link.
 */

#include "vs_sensors_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t ticks_to_ns(uint64_t ticks)
{
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        hz = 100;
    }
    return ticks * (1000000000ull / (uint64_t)hz);
}

static bool all_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

/* `/proc/<pid>/stat` field 14 is utime and 15 is stime, but the command in
 * field 2 is parenthesised and may itself contain spaces and parentheses, so
 * the fields are counted from the LAST ')'. */
static bool read_stat_cpu_time(const char *path, uint64_t *out)
{
    char buffer[4096];
    if (!vs_read_text(path, buffer, sizeof buffer)) {
        return false;
    }
    char *close = strrchr(buffer, ')');
    if (close == NULL || close[1] == '\0') {
        return false;
    }
    /* After ") " comes state (field 3). Counting from there: ppid, pgrp,
     * session, tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime,
     * stime, so utime and stime are the 11th and 12th numbers. */
    const char *cursor = close + 2;
    unsigned long long values[12];
    memset(values, 0, sizeof values);
    int parsed = sscanf(cursor,
                        "%*c %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
                        &values[6], &values[7], &values[8], &values[9], &values[10], &values[11]);
    if (parsed < 12) {
        return false;
    }
    /* values[0] is ppid (field 4), so values[10] is utime (field 14) and
     * values[11] is stime (field 15). */
    *out = ticks_to_ns((uint64_t)values[10] + (uint64_t)values[11]);
    return true;
}

static bool status_kb(const char *line, const char *key, uint64_t *out)
{
    size_t key_length = strlen(key);
    if (strncmp(line, key, key_length) != 0 || line[key_length] != ':') {
        return false;
    }
    *out = strtoull(line + key_length + 1, NULL, 10) * 1024u;
    return true;
}

static void read_status_memory(const char *path, vs_process_sample *out)
{
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, file) != NULL) {
        uint64_t value = 0;
        if (status_kb(line, "VmRSS", &value)) {
            out->rss_bytes = value;
        } else if (status_kb(line, "RssAnon", &value)) {
            out->anon_bytes = value;
        } else if (status_kb(line, "VmSwap", &value)) {
            out->swap_bytes = value;
        }
    }
    fclose(file);
}

/* ------------------------------------------------------- .desktop app names */

static void desktop_add(vs_sensors_impl *impl, const char *directory, const char *filename)
{
    size_t length = strlen(filename);
    if (length <= 8 || strcmp(filename + length - 8, ".desktop") != 0) {
        return;
    }
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", directory, filename);
    if (written < 0 || (size_t)written >= sizeof path) {
        return;
    }
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return;
    }

    char name[VS_SENSORS_LABEL_MAX];
    char exec[VS_SENSORS_NAME_MAX];
    name[0] = '\0';
    exec[0] = '\0';
    bool in_desktop_entry = false;
    char line[1024];
    while (fgets(line, sizeof line, file) != NULL) {
        vs_trim(line);
        if (line[0] == '[') {
            in_desktop_entry = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_desktop_entry) {
            continue;
        }
        if (name[0] == '\0' && strncmp(line, "Name=", 5) == 0) {
            vs_copy(name, sizeof name, line + 5);
        } else if (exec[0] == '\0' && strncmp(line, "Exec=", 5) == 0) {
            /* "Exec=/usr/bin/foo --bar %U" -> "foo". */
            char *space = strchr(line + 5, ' ');
            if (space != NULL) {
                *space = '\0';
            }
            const char *slash = strrchr(line + 5, '/');
            vs_copy(exec, sizeof exec, slash != NULL ? slash + 1 : line + 5);
        }
    }
    fclose(file);

    if (exec[0] == '\0' || name[0] == '\0') {
        return;
    }
    vs_desktop_entry *grown = realloc(impl->desktop, (impl->desktop_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return;
    }
    impl->desktop = grown;
    vs_desktop_entry *entry = &impl->desktop[impl->desktop_count];
    memset(entry, 0, sizeof *entry);
    vs_copy(entry->id, sizeof entry->id, filename);
    entry->id[length - 8] = '\0';
    vs_copy(entry->exec, sizeof entry->exec, exec);
    vs_copy(entry->name, sizeof entry->name, name);
    impl->desktop_count++;
}

/* XDG_DATA_DIRS plus XDG_DATA_HOME, the search path the spec defines. A fixture
 * root gets its own applications directory instead, so the index is
 * deterministic in a test. */
static void scan_desktop_files(vs_sensors_impl *impl)
{
    if (impl->desktop_scanned) {
        return;
    }
    impl->desktop_scanned = true;

    char roots[8][VS_FULLPATH_MAX];
    size_t root_count = 0;

    if (impl->rooted) {
        if (vs_path(impl, roots[root_count], sizeof roots[0], "/usr/share/applications")) {
            root_count++;
        }
    } else {
        const char *home = getenv("XDG_DATA_HOME");
        const char *user_home = getenv("HOME");
        if (home != NULL && home[0] != '\0') {
            snprintf(roots[root_count++], sizeof roots[0], "%s/applications", home);
        } else if (user_home != NULL && user_home[0] != '\0') {
            snprintf(roots[root_count++], sizeof roots[0], "%s/.local/share/applications",
                     user_home);
        }
        const char *dirs = getenv("XDG_DATA_DIRS");
        if (dirs == NULL || dirs[0] == '\0') {
            dirs = "/usr/local/share:/usr/share";
        }
        char copy[1024];
        vs_copy(copy, sizeof copy, dirs);
        char *saveptr = NULL;
        for (char *token = strtok_r(copy, ":", &saveptr);
             token != NULL && root_count < sizeof roots / sizeof roots[0];
             token = strtok_r(NULL, ":", &saveptr)) {
            snprintf(roots[root_count++], sizeof roots[0], "%s/applications", token);
        }
    }

    for (size_t i = 0; i < root_count; i++) {
        DIR *dir = opendir(roots[i]);
        if (dir == NULL) {
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            desktop_add(impl, roots[i], entry->d_name);
        }
        closedir(dir);
    }
}

static const vs_desktop_entry *match_desktop(vs_sensors_impl *impl, const char *comm,
                                             const char *argv0)
{
    scan_desktop_files(impl);
    for (size_t i = 0; i < impl->desktop_count; i++) {
        if (argv0 != NULL && argv0[0] != '\0' && strcmp(impl->desktop[i].exec, argv0) == 0) {
            return &impl->desktop[i];
        }
    }
    for (size_t i = 0; i < impl->desktop_count; i++) {
        /* comm is truncated to 15 characters by the kernel, so a prefix match
         * is the best that can be done for a long binary name. */
        if (strncmp(impl->desktop[i].exec, comm, 15) == 0) {
            return &impl->desktop[i];
        }
    }
    return NULL;
}

static void read_argv0(const char *path, char *out, size_t out_len)
{
    out[0] = '\0';
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return;
    }
    char buffer[512];
    size_t read = fread(buffer, 1, sizeof buffer - 1, file);
    fclose(file);
    buffer[read] = '\0';
    if (read == 0) {
        return;
    }
    const char *slash = strrchr(buffer, '/');
    vs_copy(out, out_len, slash != NULL ? slash + 1 : buffer);
}

/* ------------------------------------------------------------------ sorting */

static int compare_cpu(const void *a, const void *b)
{
    const vs_process_sample *left = a;
    const vs_process_sample *right = b;
    if (left->cpu_percent < right->cpu_percent) {
        return 1;
    }
    if (left->cpu_percent > right->cpu_percent) {
        return -1;
    }
    return left->pid - right->pid;
}

static int compare_memory(const void *a, const void *b)
{
    const vs_process_sample *left = a;
    const vs_process_sample *right = b;
    if (left->rss_bytes < right->rss_bytes) {
        return 1;
    }
    if (left->rss_bytes > right->rss_bytes) {
        return -1;
    }
    return left->pid - right->pid;
}

static uint64_t previous_cpu_time(const vs_sensors_impl *impl, int32_t pid, bool *found)
{
    for (size_t i = 0; i < impl->prev_proc_count; i++) {
        if (impl->prev_proc[i].pid == pid) {
            *found = true;
            return impl->prev_proc[i].cpu_time_ns;
        }
    }
    *found = false;
    return 0;
}

static size_t group_rows(vs_process_sample *rows, size_t count)
{
    size_t groups = 0;
    for (size_t i = 0; i < count; i++) {
        size_t target = groups;
        for (size_t g = 0; g < groups; g++) {
            if (strcmp(rows[g].group_key, rows[i].group_key) == 0) {
                target = g;
                break;
            }
        }
        if (target == groups) {
            if (i != groups) {
                rows[groups] = rows[i];
            }
            groups++;
            continue;
        }
        rows[target].cpu_percent += rows[i].cpu_percent;
        rows[target].cpu_time_ns += rows[i].cpu_time_ns;
        rows[target].rss_bytes += rows[i].rss_bytes;
        rows[target].anon_bytes += rows[i].anon_bytes;
        rows[target].swap_bytes += rows[i].swap_bytes;
        rows[target].member_count++;
        rows[target].has_rate = rows[target].has_rate || rows[i].has_rate;
    }
    return groups;
}

int vs_sensors_read_processes(vs_sensors_impl *impl, const vs_process_query *query,
                              vs_process_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    vs_process_query effective;
    if (query != NULL) {
        effective = *query;
    } else {
        memset(&effective, 0, sizeof effective);
        effective.sort = VS_PROCESS_SORT_CPU;
        effective.limit = 5;
        effective.group_by_app = true;
        effective.minimum_cpu_percent = 0.01;
    }

    char proc_root[VS_FULLPATH_MAX];
    if (!vs_path(impl, proc_root, sizeof proc_root, "/proc")) {
        return VS_ERR_BACKEND;
    }
    DIR *dir = opendir(proc_root);
    if (dir == NULL) {
        return VS_ERR_BACKEND;
    }

    double now = vs_now();
    double elapsed = now - impl->prev_proc_time;
    bool have_previous = impl->prev_proc != NULL && impl->prev_proc_count > 0 && elapsed > 0;
    /* A gap longer than this means sampling was paused; a delta across it would
     * be a misleading spike, so the interval is treated as a fresh baseline.
     * NetworkSampler.maxGap makes the same call at 10 seconds. */
    if (elapsed > 30.0) {
        have_previous = false;
    }
    long processors = sysconf(_SC_NPROCESSORS_ONLN);
    if (processors < 1) {
        processors = 1;
    }

    vs_process_sample *rows = NULL;
    size_t count = 0;
    size_t capacity = 0;
    vs_proc_prev *next_prev = NULL;
    size_t next_prev_count = 0;
    size_t next_prev_capacity = 0;
    int result = VS_OK;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!all_digits(entry->d_name)) {
            continue;
        }
        int32_t pid = (int32_t)strtol(entry->d_name, NULL, 10);
        char path[VS_FULLPATH_MAX];
        uint64_t cpu_time = 0;
        int written = snprintf(path, sizeof path, "%s/%s/stat", proc_root, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof path ||
            !read_stat_cpu_time(path, &cpu_time)) {
            continue;
        }

        if (next_prev_count == next_prev_capacity) {
            size_t next = next_prev_capacity == 0 ? 256 : next_prev_capacity * 2;
            vs_proc_prev *grown = realloc(next_prev, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            next_prev = grown;
            next_prev_capacity = next;
        }
        next_prev[next_prev_count].pid = pid;
        next_prev[next_prev_count].cpu_time_ns = cpu_time;
        next_prev_count++;

        double percent = 0;
        bool has_rate = false;
        if (have_previous) {
            bool found = false;
            uint64_t before = previous_cpu_time(impl, pid, &found);
            if (found && cpu_time >= before) {
                double capacity_ns = elapsed * 1e9 * (double)processors;
                if (capacity_ns > 0) {
                    percent = (double)(cpu_time - before) / capacity_ns * 100.0;
                    if (percent > 100.0) {
                        percent = 100.0;
                    }
                    has_rate = true;
                }
            }
        }

        if (count == capacity) {
            size_t next = capacity == 0 ? 256 : capacity * 2;
            vs_process_sample *grown = realloc(rows, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            rows = grown;
            capacity = next;
        }
        vs_process_sample *row = &rows[count];
        memset(row, 0, sizeof *row);
        row->pid = pid;
        row->cpu_time_ns = cpu_time;
        row->cpu_percent = percent;
        row->has_rate = has_rate;
        row->member_count = 1;

        written = snprintf(path, sizeof path, "%s/%s/comm", proc_root, entry->d_name);
        if (written > 0 && (size_t)written < sizeof path) {
            char comm[VS_SENSORS_NAME_MAX];
            if (vs_read_text(path, comm, sizeof comm)) {
                vs_copy(row->comm, sizeof row->comm, vs_trim(comm));
            }
        }
        char argv0[VS_SENSORS_NAME_MAX];
        argv0[0] = '\0';
        written = snprintf(path, sizeof path, "%s/%s/cmdline", proc_root, entry->d_name);
        if (written > 0 && (size_t)written < sizeof path) {
            read_argv0(path, argv0, sizeof argv0);
        }
        written = snprintf(path, sizeof path, "%s/%s/status", proc_root, entry->d_name);
        if (written > 0 && (size_t)written < sizeof path) {
            read_status_memory(path, row);
        }

        const vs_desktop_entry *desktop = match_desktop(impl, row->comm, argv0);
        if (desktop != NULL) {
            vs_copy(row->name, sizeof row->name, desktop->name);
            vs_copy(row->group_key, sizeof row->group_key, desktop->id);
        } else if (argv0[0] != '\0') {
            vs_copy(row->name, sizeof row->name, argv0);
            vs_copy(row->group_key, sizeof row->group_key, argv0);
        } else {
            vs_copy(row->name, sizeof row->name, row->comm);
            vs_copy(row->group_key, sizeof row->group_key, row->comm);
        }
        count++;
    }
    closedir(dir);

    if (result != VS_OK) {
        free(rows);
        free(next_prev);
        return result;
    }

    free(impl->prev_proc);
    impl->prev_proc = next_prev;
    impl->prev_proc_count = next_prev_count;
    impl->prev_proc_time = now;

    /* Filter before grouping, which is the order `ProcessUsageService.topCPU`
     * uses: it drops rows under 0.01 % as it builds them and only then calls
     * groupedByApp. Grouping first would be defensible -- thirty renderers at
     * 0.009 % each are a visible 0.27 % together -- but it is a different
     * answer from the Mac's for the same machine, and the point of this layer
     * is that the two agree. */
    if (effective.sort == VS_PROCESS_SORT_CPU && effective.minimum_cpu_percent > 0) {
        size_t kept = 0;
        for (size_t i = 0; i < count; i++) {
            if (rows[i].cpu_percent >= effective.minimum_cpu_percent) {
                if (kept != i) {
                    rows[kept] = rows[i];
                }
                kept++;
            }
        }
        count = kept;
    }

    if (effective.group_by_app) {
        count = group_rows(rows, count);
    }

    if (count > 1) {
        qsort(rows, count, sizeof *rows,
              effective.sort == VS_PROCESS_SORT_MEMORY ? compare_memory : compare_cpu);
    }
    if (effective.limit > 0 && count > effective.limit) {
        count = effective.limit;
    }

    *out = rows;
    *count_out = count;
    return VS_OK;
}
