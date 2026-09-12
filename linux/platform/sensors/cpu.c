/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/proc/stat` deltas, `/proc/loadavg`, `/sys/devices/system/cpu` topology and
 * frequency. The macOS counterpart is `host_statistics(HOST_CPU_LOAD_INFO)` in
 * SystemMonitor.swift, which subtracts the previous tick sample exactly the
 * same way and returns nil when there is no previous sample.
 */

#include "vs_sensors_internal.h"

#include <stdlib.h>
#include <string.h>

static uint64_t ticks_busy(const vs_cpu_ticks *t)
{
    /* macOS's CPU_LOAD_INFO has user, system, idle and nice; irq/softirq/steal
     * have no Mach counterpart and are folded into system, which is where the
     * work actually happens. iowait is reported separately and is not busy. */
    return t->user + t->nice + t->system + t->irq + t->softirq + t->steal;
}

static uint64_t ticks_total(const vs_cpu_ticks *t)
{
    return ticks_busy(t) + t->idle + t->iowait;
}

static bool parse_cpu_line(const char *line, int *index_out, vs_cpu_ticks *out)
{
    memset(out, 0, sizeof *out);
    if (strncmp(line, "cpu", 3) != 0) {
        return false;
    }
    const char *cursor = line + 3;
    if (*cursor == ' ') {
        *index_out = -1;
    } else {
        char *end = NULL;
        long value = strtol(cursor, &end, 10);
        if (end == cursor || value < 0) {
            return false;
        }
        *index_out = (int)value;
        cursor = end;
    }
    unsigned long long v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int parsed = sscanf(cursor, "%llu %llu %llu %llu %llu %llu %llu %llu",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (parsed < 4) {
        return false;
    }
    out->user = v[0];
    out->nice = v[1];
    out->system = v[2];
    out->idle = v[3];
    out->iowait = v[4];
    out->irq = v[5];
    out->softirq = v[6];
    out->steal = v[7];
    out->valid = true;
    return true;
}

static void read_load_average(vs_sensors_impl *impl, double load[3])
{
    load[0] = load[1] = load[2] = 0;
    FILE *file = vs_open_rooted(impl, "/proc/loadavg");
    if (file == NULL) {
        return;
    }
    if (fscanf(file, "%lf %lf %lf", &load[0], &load[1], &load[2]) != 3) {
        load[0] = load[1] = load[2] = 0;
    }
    fclose(file);
}

/* `cpu MHz` lines in /proc/cpuinfo, in processor order, for the machines with
 * no cpufreq tree (a VM, or an ACPI-only laptop). */
static size_t read_cpuinfo_mhz(vs_sensors_impl *impl, double *out, size_t capacity)
{
    FILE *file = vs_open_rooted(impl, "/proc/cpuinfo");
    if (file == NULL) {
        return 0;
    }
    char line[512];
    size_t count = 0;
    while (count < capacity && fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "cpu MHz", 7) != 0) {
            continue;
        }
        const char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        out[count++] = strtod(colon + 1, NULL);
    }
    fclose(file);
    return count;
}

/* Package and core ids are fixed for the life of a boot, so they are read once
 * per instance: without the cache this is two file opens per core on every
 * sampling tick, which on a 128-thread machine is 256 of them a second. */
static void scan_topology(vs_sensors_impl *impl, size_t core_count)
{
    if (impl->topology_scanned && impl->topology_cores >= core_count) {
        return;
    }
    char path[VS_FULLPATH_MAX];
    int32_t seen_packages[VS_SENSORS_MAX_CORES];
    int32_t seen_core_package[VS_SENSORS_MAX_CORES];
    int32_t seen_core_id[VS_SENSORS_MAX_CORES];
    size_t package_count = 0;
    size_t physical_count = 0;

    for (size_t i = 0; i < core_count; i++) {
        int64_t value = 0;
        impl->package_id[i] = -1;
        impl->core_id[i] = -1;
        if (vs_pathf(impl, path, sizeof path,
                     "/sys/devices/system/cpu/cpu%zu/topology/physical_package_id", i) &&
            vs_read_i64(path, &value)) {
            impl->package_id[i] = (int32_t)value;
        }
        if (vs_pathf(impl, path, sizeof path, "/sys/devices/system/cpu/cpu%zu/topology/core_id",
                     i) &&
            vs_read_i64(path, &value)) {
            impl->core_id[i] = (int32_t)value;
        }

        if (impl->package_id[i] >= 0) {
            bool known = false;
            for (size_t p = 0; p < package_count; p++) {
                if (seen_packages[p] == impl->package_id[i]) {
                    known = true;
                    break;
                }
            }
            if (!known && package_count < VS_SENSORS_MAX_CORES) {
                seen_packages[package_count++] = impl->package_id[i];
            }
        }
        /* A physical core is a distinct (package, core_id) pair: core_id is
         * only unique inside its package, so the pair is what separates two
         * sockets' core 0 from one core's two threads. */
        if (impl->core_id[i] >= 0) {
            bool known = false;
            for (size_t c = 0; c < physical_count; c++) {
                if (seen_core_id[c] == impl->core_id[i] &&
                    seen_core_package[c] == impl->package_id[i]) {
                    known = true;
                    break;
                }
            }
            if (!known && physical_count < VS_SENSORS_MAX_CORES) {
                seen_core_package[physical_count] = impl->package_id[i];
                seen_core_id[physical_count] = impl->core_id[i];
                physical_count++;
            }
        }
    }

    impl->topology_scanned = true;
    impl->topology_cores = core_count;
    impl->package_count = package_count;
    impl->physical_core_count = physical_count;
}

static void fill_topology(vs_sensors_impl *impl, vs_cpu_sample *out, const double *cpuinfo_mhz,
                          size_t cpuinfo_count)
{
    char path[VS_FULLPATH_MAX];
    scan_topology(impl, out->core_count);

    for (size_t i = 0; i < out->core_count; i++) {
        vs_cpu_core_sample *core = &out->cores[i];
        core->package_id = impl->package_id[i];
        core->core_id = impl->core_id[i];

        core->frequency_mhz = 0;
        uint64_t khz = 0;
        if (vs_pathf(impl, path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core->index) &&
            vs_read_u64(path, &khz)) {
            core->frequency_mhz = (double)khz / 1000.0;
        } else if ((size_t)core->index < cpuinfo_count) {
            core->frequency_mhz = cpuinfo_mhz[core->index];
        }
    }

    out->package_count = impl->package_count;
    out->physical_core_count = impl->physical_core_count;
}

int vs_sensors_read_cpu(vs_sensors_impl *impl, vs_cpu_sample *out)
{
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    FILE *file = vs_open_rooted(impl, "/proc/stat");
    if (file == NULL) {
        return VS_ERR_BACKEND;
    }

    memset(out, 0, sizeof *out);
    double now = vs_now();
    vs_cpu_ticks aggregate;
    memset(&aggregate, 0, sizeof aggregate);
    vs_cpu_ticks per_core[VS_SENSORS_MAX_CORES];
    memset(per_core, 0, sizeof per_core);

    char line[1024];
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "cpu", 3) != 0) {
            break; /* the cpu lines are first and contiguous */
        }
        int index = 0;
        vs_cpu_ticks ticks;
        if (!parse_cpu_line(line, &index, &ticks)) {
            continue;
        }
        if (index < 0) {
            aggregate = ticks;
        } else if (index < VS_SENSORS_MAX_CORES) {
            per_core[index] = ticks;
            if (out->core_count < (size_t)index + 1) {
                out->core_count = (size_t)index + 1;
            }
        }
    }
    fclose(file);

    if (!aggregate.valid) {
        return VS_ERR_BACKEND;
    }

    for (size_t i = 0; i < out->core_count; i++) {
        out->cores[i].index = (int32_t)i;
    }

    double elapsed = now - impl->prev_cpu_time;
    if (impl->prev_total.valid && elapsed > 0) {
        uint64_t previous_total = ticks_total(&impl->prev_total);
        uint64_t current_total = ticks_total(&aggregate);
        if (current_total > previous_total) {
            double span = (double)(current_total - previous_total);
            out->has_rates = true;
            out->interval_seconds = elapsed;
            out->total_usage = (double)(ticks_busy(&aggregate) - ticks_busy(&impl->prev_total)) / span;
            out->user_usage = (double)((aggregate.user + aggregate.nice) -
                                       (impl->prev_total.user + impl->prev_total.nice)) / span;
            out->system_usage = (double)((aggregate.system + aggregate.irq + aggregate.softirq +
                                          aggregate.steal) -
                                         (impl->prev_total.system + impl->prev_total.irq +
                                          impl->prev_total.softirq + impl->prev_total.steal)) / span;
            out->idle_usage = (double)(aggregate.idle - impl->prev_total.idle) / span;
            out->iowait_usage = (double)(aggregate.iowait - impl->prev_total.iowait) / span;

            for (size_t i = 0; i < out->core_count; i++) {
                const vs_cpu_ticks *previous = &impl->prev_core[i];
                if (!previous->valid || !per_core[i].valid) {
                    continue;
                }
                uint64_t before = ticks_total(previous);
                uint64_t after = ticks_total(&per_core[i]);
                if (after <= before) {
                    continue;
                }
                out->cores[i].usage = (double)(ticks_busy(&per_core[i]) - ticks_busy(previous)) /
                                      (double)(after - before);
            }
        }
    }

    impl->prev_total = aggregate;
    memcpy(impl->prev_core, per_core, sizeof per_core);
    impl->prev_cpu_time = now;

    read_load_average(impl, out->load_average);

    double cpuinfo_mhz[VS_SENSORS_MAX_CORES];
    size_t cpuinfo_count = read_cpuinfo_mhz(impl, cpuinfo_mhz, VS_SENSORS_MAX_CORES);
    fill_topology(impl, out, cpuinfo_mhz, cpuinfo_count);

    return VS_OK;
}
