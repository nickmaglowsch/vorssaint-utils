/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * GPU telemetry, one vendor at a time, degrading honestly.
 *
 * macOS reads one number from IOAccelerator ("Device Utilization %") and every
 * GPU answers it. Linux has no such number: what exists differs per driver, and
 * the differences are the feature's real shape. The measured per-vendor matrix
 * is in docs/linux-port/SENSORS_BACKEND.md; in outline:
 *
 *   amdgpu   busy, VRAM used/total and, through its hwmon node, power, edge and
 *            junction temperature and the shader clock. Everything the panel
 *            wants, unprivileged.
 *   i915/xe  no busy percentage in sysfs at all: engine busyness lives in a
 *            perf PMU that needs either CAP_PERFMON or
 *            perf_event_paranoid <= 0, which the app must not require. What
 *            sysfs gives is the current and maximum GT frequency, and (on
 *            discrete cards) a hwmon node. `has_busy` stays false and the panel
 *            shows the clock instead of inventing a percentage.
 *   nvidia   NVML, dlopened (nvml.c): busy, VRAM, temperature, power, clock.
 *            Nothing without the proprietary driver.
 */

#include "vs_sensors_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

/* PCI vendor ids, as the `vendor` attribute of a PCI device spells them. */
#define PCI_VENDOR_AMD 0x1002
#define PCI_VENDOR_NVIDIA 0x10de
#define PCI_VENDOR_INTEL 0x8086

static int compare_names(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static bool read_key(const char *path, const char *key, char *out, size_t out_len)
{
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    size_t key_length = strlen(key);
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, key, key_length) != 0 || line[key_length] != '=') {
            continue;
        }
        vs_copy(out, out_len, vs_trim(line + key_length + 1));
        found = out[0] != '\0';
        break;
    }
    fclose(file);
    return found;
}

/* The card's hwmon node sits under device/hwmon/hwmonN and carries power,
 * temperature and clocks for amdgpu and for discrete Intel. */
static bool find_card_hwmon(const char *device_path, char *out, size_t out_len)
{
    char base[VS_FULLPATH_MAX];
    int written = snprintf(base, sizeof base, "%s/hwmon", device_path);
    if (written < 0 || (size_t)written >= sizeof base) {
        return false;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) {
            continue;
        }
        written = snprintf(out, out_len, "%s/%s", base, entry->d_name);
        found = written > 0 && (size_t)written < out_len;
        break;
    }
    closedir(dir);
    return found;
}

static bool sub_u64(const char *directory, const char *file, uint64_t *out)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", directory, file);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    return vs_read_u64(path, out);
}

static void fill_from_hwmon(const char *hwmon, vs_gpu_sample *out)
{
    uint64_t value = 0;
    if (sub_u64(hwmon, "temp1_input", &value)) {
        out->temperature_celsius = (double)value / 1000.0;
        out->has_temperature = true;
    }
    /* power1_average is the rolling figure the driver reports; power1_input is
     * the instantaneous one on the cards that have it. */
    if (sub_u64(hwmon, "power1_average", &value) && value > 0) {
        out->watts = (double)value / 1e6;
        out->has_watts = true;
    } else if (sub_u64(hwmon, "power1_input", &value) && value > 0) {
        out->watts = (double)value / 1e6;
        out->has_watts = true;
    }
    if (!out->has_clock && sub_u64(hwmon, "freq1_input", &value) && value > 0) {
        out->clock_mhz = (double)value / 1e6;
        out->has_clock = true;
    }
}

static void fill_intel_clock(const char *card_path, vs_gpu_sample *out)
{
    /* i915 up to 6.2 published gt_cur_freq_mhz at the card root; xe and newer
     * i915 moved it under gt/gt0/. Both are read, newest layout first. */
    static const char *const candidates[] = {
        "gt/gt0/rps_cur_freq_mhz",
        "gt/gt0/freq0_cur_freq",
        "gt_cur_freq_mhz",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        char path[VS_FULLPATH_MAX];
        int written = snprintf(path, sizeof path, "%s/%s", card_path, candidates[i]);
        if (written < 0 || (size_t)written >= sizeof path) {
            continue;
        }
        uint64_t mhz = 0;
        if (vs_read_u64(path, &mhz) && mhz > 0) {
            out->clock_mhz = (double)mhz;
            out->has_clock = true;
            return;
        }
    }
}

int vs_sensors_read_gpus(vs_sensors_impl *impl, vs_gpu_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    vs_gpu_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;

    char base[VS_FULLPATH_MAX];
    if (vs_path(impl, base, sizeof base, "/sys/class/drm")) {
        DIR *dir = opendir(base);
        if (dir != NULL) {
            char **cards = NULL;
            size_t card_count = 0;
            size_t card_capacity = 0;
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                /* "card0" is the device; "card0-DP-1" is a connector. */
                if (strncmp(entry->d_name, "card", 4) != 0 ||
                    strchr(entry->d_name, '-') != NULL) {
                    continue;
                }
                if (card_count == card_capacity) {
                    size_t next = card_capacity == 0 ? 4 : card_capacity * 2;
                    char **grown = realloc(cards, next * sizeof *grown);
                    if (grown == NULL) {
                        break;
                    }
                    cards = grown;
                    card_capacity = next;
                }
                cards[card_count] = strdup(entry->d_name);
                if (cards[card_count] == NULL) {
                    break;
                }
                card_count++;
            }
            closedir(dir);
            if (card_count > 1) {
                qsort(cards, card_count, sizeof *cards, compare_names);
            }

            for (size_t c = 0; c < card_count; c++) {
                char card_path[VS_FULLPATH_MAX];
                char device_path[VS_FULLPATH_MAX];
                int written = snprintf(card_path, sizeof card_path, "%s/%s", base, cards[c]);
                if (written < 0 || (size_t)written >= sizeof card_path) {
                    free(cards[c]);
                    continue;
                }
                written = snprintf(device_path, sizeof device_path, "%s/device", card_path);
                if (written < 0 || (size_t)written >= sizeof device_path) {
                    free(cards[c]);
                    continue;
                }

                char uevent[VS_FULLPATH_MAX];
                char driver[VS_SENSORS_NAME_MAX];
                driver[0] = '\0';
                written = snprintf(uevent, sizeof uevent, "%s/uevent", device_path);
                if (written > 0 && (size_t)written < sizeof uevent) {
                    (void)read_key(uevent, "DRIVER", driver, sizeof driver);
                }

                uint64_t vendor_id = 0;
                char vendor_path[VS_FULLPATH_MAX];
                written = snprintf(vendor_path, sizeof vendor_path, "%s/vendor", device_path);
                if (written > 0 && (size_t)written < sizeof vendor_path) {
                    char text[32];
                    if (vs_read_text(vendor_path, text, sizeof text)) {
                        vendor_id = strtoull(vs_trim(text), NULL, 0);
                    }
                }

                if (count == capacity) {
                    size_t next = capacity == 0 ? 4 : capacity * 2;
                    vs_gpu_sample *grown = realloc(samples, next * sizeof *grown);
                    if (grown == NULL) {
                        /* Release the names still ahead of us as well: the loop
                         * frees each as it consumes it, and breaking out skips
                         * the rest. */
                        while (c < card_count) {
                            free(cards[c++]);
                        }
                        break;
                    }
                    samples = grown;
                    capacity = next;
                }
                vs_gpu_sample *sample = &samples[count];
                memset(sample, 0, sizeof *sample);
                vs_copy(sample->id, sizeof sample->id, cards[c]);
                vs_copy(sample->driver, sizeof sample->driver,
                        driver[0] != '\0' ? driver : "unknown");
                switch (vendor_id) {
                case PCI_VENDOR_AMD:
                    sample->vendor = VS_GPU_VENDOR_AMD;
                    break;
                case PCI_VENDOR_INTEL:
                    sample->vendor = VS_GPU_VENDOR_INTEL;
                    break;
                case PCI_VENDOR_NVIDIA:
                    sample->vendor = VS_GPU_VENDOR_NVIDIA;
                    break;
                default:
                    sample->vendor = VS_GPU_VENDOR_UNKNOWN;
                    break;
                }
                snprintf(sample->name, sizeof sample->name, "%s %s", sample->driver, cards[c]);

                uint64_t value = 0;
                if (sub_u64(device_path, "gpu_busy_percent", &value)) {
                    sample->busy = (double)value / 100.0;
                    sample->has_busy = true;
                }
                uint64_t used = 0;
                uint64_t total = 0;
                if (sub_u64(device_path, "mem_info_vram_used", &used) &&
                    sub_u64(device_path, "mem_info_vram_total", &total) && total > 0) {
                    sample->vram_used_bytes = used;
                    sample->vram_total_bytes = total;
                    sample->has_vram = true;
                }
                if (sample->vendor == VS_GPU_VENDOR_INTEL) {
                    fill_intel_clock(card_path, sample);
                }
                char hwmon[VS_FULLPATH_MAX];
                if (find_card_hwmon(device_path, hwmon, sizeof hwmon)) {
                    fill_from_hwmon(hwmon, sample);
                }

                count++;
                free(cards[c]);
            }
            free(cards);
        }
    }

    /* NVIDIA's kernel module publishes nothing useful in sysfs, so its row
     * comes from NVML when the driver is installed, and the card simply has no
     * row otherwise. */
    if (impl->nvml != NULL) {
        vs_gpu_sample *nvidia_rows = samples;
        size_t nvidia_count = count;
        size_t nvidia_capacity = capacity;
        vs_nvml_collect(impl->nvml, &nvidia_rows, &nvidia_count, &nvidia_capacity);
        samples = nvidia_rows;
        count = nvidia_count;
        capacity = nvidia_capacity;
    }

    *out = samples;
    *count_out = count;
    return VS_OK;
}
