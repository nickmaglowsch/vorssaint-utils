/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/proc/meminfo` and `/proc/pressure/memory`.
 *
 * The mapping onto what the macOS panel shows (SystemMonitor.swift's
 * `SystemSnapshot`, filled by `MetricFormat.memoryUsed` / `appMemory` /
 * `cachedFiles`) is the interesting part, and it is spelled out in
 * linux/platform/sensors/README.md. In short:
 *
 *   Memory Used  = app + wired + compressed   ->  MemTotal - MemAvailable
 *   App Memory   = internal - purgeable       ->  AnonPages
 *   Cached Files = external page cache        ->  Buffers + Cached + SReclaimable - Shmem
 *   Compressed   = compressor pages           ->  Zswapped, or absent
 *   Swap Used    = swapins - swapouts region  ->  SwapTotal - SwapFree
 *   Pressure     = kern.memorystatus_..._level -> /proc/pressure/memory some avg10
 */

#include "vs_sensors_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct meminfo {
    uint64_t total;
    uint64_t free;
    uint64_t available;
    bool has_available;
    uint64_t buffers;
    uint64_t cached;
    uint64_t sreclaimable;
    uint64_t shmem;
    uint64_t anon;
    uint64_t zswapped;
    bool has_zswapped;
    uint64_t swap_total;
    uint64_t swap_free;
} meminfo;

static bool take(const char *line, const char *key, uint64_t *out)
{
    size_t key_length = strlen(key);
    if (strncmp(line, key, key_length) != 0 || line[key_length] != ':') {
        return false;
    }
    char *end = NULL;
    unsigned long long value = strtoull(line + key_length + 1, &end, 10);
    if (end == line + key_length + 1) {
        return false;
    }
    /* Every meminfo quantity but HugePages_* is in kB, and we ask for none of
     * those. */
    *out = (uint64_t)value * 1024u;
    return true;
}

static uint64_t saturating_sub(uint64_t a, uint64_t b)
{
    return a > b ? a - b : 0;
}

static bool read_psi(vs_sensors_impl *impl, double *some_out, double *full_out)
{
    FILE *file = vs_open_rooted(impl, "/proc/pressure/memory");
    if (file == NULL) {
        return false;
    }
    char line[256];
    bool found = false;
    while (fgets(line, sizeof line, file) != NULL) {
        double avg10 = 0;
        if (sscanf(line, "some avg10=%lf", &avg10) == 1) {
            *some_out = avg10 / 100.0;
            found = true;
        } else if (sscanf(line, "full avg10=%lf", &avg10) == 1) {
            *full_out = avg10 / 100.0;
        }
    }
    fclose(file);
    return found;
}

int vs_sensors_read_memory(vs_sensors_impl *impl, vs_memory_sample *out)
{
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    FILE *file = vs_open_rooted(impl, "/proc/meminfo");
    if (file == NULL) {
        return VS_ERR_BACKEND;
    }

    meminfo info;
    memset(&info, 0, sizeof info);
    char line[256];
    while (fgets(line, sizeof line, file) != NULL) {
        uint64_t value = 0;
        if (take(line, "MemTotal", &value)) {
            info.total = value;
        } else if (take(line, "MemFree", &value)) {
            info.free = value;
        } else if (take(line, "MemAvailable", &value)) {
            info.available = value;
            info.has_available = true;
        } else if (take(line, "Buffers", &value)) {
            info.buffers = value;
        } else if (take(line, "Cached", &value)) {
            info.cached = value;
        } else if (take(line, "SReclaimable", &value)) {
            info.sreclaimable = value;
        } else if (take(line, "Shmem", &value)) {
            info.shmem = value;
        } else if (take(line, "AnonPages", &value)) {
            info.anon = value;
        } else if (take(line, "Zswapped", &value)) {
            info.zswapped = value;
            info.has_zswapped = true;
        } else if (take(line, "SwapTotal", &value)) {
            info.swap_total = value;
        } else if (take(line, "SwapFree", &value)) {
            info.swap_free = value;
        }
    }
    fclose(file);

    if (info.total == 0) {
        return VS_ERR_BACKEND;
    }

    memset(out, 0, sizeof *out);
    out->total_bytes = info.total;
    /* Pre-3.14 kernels have no MemAvailable; the documented approximation is
     * free plus the reclaimable page cache. */
    out->available_bytes = info.has_available
                               ? info.available
                               : info.free + info.buffers + info.cached + info.sreclaimable;
    if (out->available_bytes > info.total) {
        out->available_bytes = info.total;
    }
    out->used_bytes = saturating_sub(info.total, out->available_bytes);
    out->app_bytes = info.anon;
    out->cached_bytes = saturating_sub(info.buffers + info.cached + info.sreclaimable, info.shmem);
    out->compressed_bytes = info.zswapped;
    out->has_compressed = info.has_zswapped;
    out->swap_total_bytes = info.swap_total;
    out->swap_used_bytes = saturating_sub(info.swap_total, info.swap_free);

    double some = 0;
    double full = 0;
    if (read_psi(impl, &some, &full)) {
        out->pressure = some > 1.0 ? 1.0 : some;
        out->pressure_full = full > 1.0 ? 1.0 : full;
        out->pressure_is_psi = true;
    } else {
        double shortfall = 1.0 - (double)out->available_bytes / (double)info.total;
        if (shortfall < 0) {
            shortfall = 0;
        }
        if (shortfall > 1) {
            shortfall = 1;
        }
        out->pressure = shortfall;
        out->pressure_full = 0;
        out->pressure_is_psi = false;
    }

    return VS_OK;
}
