/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/sys/class/hwmon` temperatures and fans, and the classification rules that
 * replace `TemperatureSensorSelector`'s SMC-key prefixes.
 *
 * On macOS the chip generation decides which SMC keys are "the CPU": a fixed
 * table per Apple Silicon family, with the hottest plausible reading as the
 * fallback for a Mac that does not carry its family's sensors. On Linux the
 * driver name carries that information instead, and the drivers split into two
 * groups:
 *
 *   - single-purpose drivers, where the name alone is the answer: coretemp,
 *     k10temp, zenpower, k8temp (CPU); amdgpu, nouveau, nvidia, radeon, i915,
 *     xe (GPU); nvme, drivetemp (drive); acpitz, pch (system);
 *   - platform/super-I/O chips that publish a dozen unrelated sensors under one
 *     name: thinkpad, dell_smm, asus*, nct6775 and friends. For those the
 *     per-sensor `temp*_label` decides, by the same keywords a person reads.
 *
 * The plausibility window and the "hottest of its kind" fallback are taken
 * straight from `TemperatureSensorSelector.displayedCPUTemperature`.
 */

#include "vs_sensors_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct driver_rule {
    const char *prefix;
    vs_sensor_kind kind;
} driver_rule;

/* Longest-prefix wins, so "nvme" does not capture "nvmem" style names by
 * accident: every entry here is matched as a whole-name prefix against the
 * hwmon `name` file, which the kernel keeps short and stable. */
static const driver_rule driver_rules[] = {
    {"coretemp", VS_SENSOR_CPU},      {"k10temp", VS_SENSOR_CPU},
    {"k8temp", VS_SENSOR_CPU},        {"zenpower", VS_SENSOR_CPU},
    {"cpu_thermal", VS_SENSOR_CPU},   {"cpu-thermal", VS_SENSOR_CPU},
    {"via-cputemp", VS_SENSOR_CPU},   {"amdgpu", VS_SENSOR_GPU},
    {"radeon", VS_SENSOR_GPU},        {"nouveau", VS_SENSOR_GPU},
    {"nvidia", VS_SENSOR_GPU},        {"i915", VS_SENSOR_GPU},
    {"xe", VS_SENSOR_GPU},            {"nvme", VS_SENSOR_DRIVE},
    {"drivetemp", VS_SENSOR_DRIVE},   {"acpitz", VS_SENSOR_SYSTEM},
    {"pch_", VS_SENSOR_SYSTEM},       {"BAT", VS_SENSOR_BATTERY},
    {"bq27", VS_SENSOR_BATTERY},      {"rt5033-battery", VS_SENSOR_BATTERY},
    {"surface_battery", VS_SENSOR_BATTERY},
};

/* Multi-sensor platform chips: the name says nothing, the label says
 * everything. */
static const char *const label_driven_drivers[] = {
    "thinkpad", "dell_smm", "dell-smm", "asus", "nct", "it87", "f71", "w836", "applesmc",
};

static bool matches_any_prefix(const char *value, const char *const *prefixes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (strncmp(value, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static vs_sensor_kind classify_label(const char *label)
{
    if (label == NULL || label[0] == '\0') {
        return VS_SENSOR_OTHER;
    }
    if (vs_contains_ci(label, "gpu") || vs_contains_ci(label, "graphics")) {
        return VS_SENSOR_GPU;
    }
    if (vs_contains_ci(label, "cpu") || vs_contains_ci(label, "core") || vs_contains_ci(label, "package") ||
        vs_contains_ci(label, "tctl") || vs_contains_ci(label, "tdie") || vs_contains_ci(label, "tccd")) {
        return VS_SENSOR_CPU;
    }
    if (vs_contains_ci(label, "batt") || vs_contains_ci(label, "bat0") || vs_contains_ci(label, "bat1")) {
        return VS_SENSOR_BATTERY;
    }
    if (vs_contains_ci(label, "nvme") || vs_contains_ci(label, "ssd") || vs_contains_ci(label, "hdd") ||
        vs_contains_ci(label, "drive") || vs_contains_ci(label, "composite")) {
        return VS_SENSOR_DRIVE;
    }
    if (vs_contains_ci(label, "ambient") || vs_contains_ci(label, "systin") ||
        vs_contains_ci(label, "system") || vs_contains_ci(label, "mainboard") ||
        vs_contains_ci(label, "motherboard") || vs_contains_ci(label, "pch") ||
        vs_contains_ci(label, "chipset")) {
        return VS_SENSOR_SYSTEM;
    }
    return VS_SENSOR_OTHER;
}

vs_sensor_kind vs_sensor_classify(const char *driver, const char *label)
{
    if (driver == NULL) {
        driver = "";
    }
    /* Label-driven chips first: "thinkpad" carries CPU, GPU and battery
     * sensors at once, so the name must not decide for all of them. */
    if (matches_any_prefix(driver, label_driven_drivers,
                           sizeof label_driven_drivers / sizeof label_driven_drivers[0])) {
        vs_sensor_kind kind = classify_label(label);
        return kind == VS_SENSOR_OTHER ? VS_SENSOR_SYSTEM : kind;
    }

    size_t best_length = 0;
    vs_sensor_kind best = VS_SENSOR_OTHER;
    for (size_t i = 0; i < sizeof driver_rules / sizeof driver_rules[0]; i++) {
        size_t length = strlen(driver_rules[i].prefix);
        if (length > best_length && strncmp(driver, driver_rules[i].prefix, length) == 0) {
            best_length = length;
            best = driver_rules[i].kind;
        }
    }
    if (best != VS_SENSOR_OTHER) {
        return best;
    }
    return classify_label(label);
}

bool vs_sensor_label_is_package(const char *label)
{
    if (label == NULL) {
        return false;
    }
    /* coretemp says "Package id 0"; k10temp and zenpower say "Tctl" (the
     * control value the fan curve uses) or "Tdie"; nvme says "Composite". */
    return vs_contains_ci(label, "package id") || strcasecmp(label, "tctl") == 0 ||
           strcasecmp(label, "tdie") == 0 || strcasecmp(label, "composite") == 0 ||
           strcasecmp(label, "edge") == 0;
}

bool vs_sensor_temperature_plausible(double celsius)
{
    /* TemperatureSensorSelector.minimumChipTemperature is 10, and its upper
     * bound is 125. */
    return celsius >= 10.0 && celsius < 125.0;
}

vs_thermal_pressure vs_thermal_from_ratio(double ratio)
{
    if (ratio >= 0.95) {
        return VS_THERMAL_CRITICAL;
    }
    if (ratio >= 0.85) {
        return VS_THERMAL_SERIOUS;
    }
    if (ratio >= 0.75) {
        return VS_THERMAL_FAIR;
    }
    return VS_THERMAL_NOMINAL;
}

/* --------------------------------------------------------------- hwmon walk */

typedef struct hwmon_dir {
    /* Path under the configured root, as opened. */
    char path[VS_FULLPATH_MAX];
    char driver[VS_SENSORS_NAME_MAX];
} hwmon_dir;

static int compare_names(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

/* Every hwmonN under /sys/class/hwmon, with its `name`, in name order so the
 * list is stable across calls. Only the modern layout (sensor files directly in
 * hwmonN) is read: the pre-2.6.32 `device/` indirection is gone from every
 * kernel this port targets. */
static size_t list_hwmon(vs_sensors_impl *impl, hwmon_dir **out)
{
    char base[VS_FULLPATH_MAX];
    *out = NULL;
    if (!vs_path(impl, base, sizeof base, "/sys/class/hwmon")) {
        return 0;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return 0;
    }

    char **names = NULL;
    size_t name_count = 0;
    size_t name_capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) {
            continue;
        }
        if (name_count == name_capacity) {
            size_t next = name_capacity == 0 ? 8 : name_capacity * 2;
            char **grown = realloc(names, next * sizeof *grown);
            if (grown == NULL) {
                break;
            }
            names = grown;
            name_capacity = next;
        }
        names[name_count] = strdup(entry->d_name);
        if (names[name_count] == NULL) {
            break;
        }
        name_count++;
    }
    closedir(dir);
    if (name_count > 1) {
        qsort(names, name_count, sizeof *names, compare_names);
    }

    hwmon_dir *devices = name_count > 0 ? calloc(name_count, sizeof *devices) : NULL;
    size_t count = 0;
    for (size_t i = 0; i < name_count; i++) {
        if (devices != NULL) {
            char name_path[VS_FULLPATH_MAX];
            char driver[VS_SENSORS_NAME_MAX];
            int written = snprintf(devices[count].path, sizeof devices[count].path, "%s/%s", base,
                                   names[i]);
            if (written > 0 && (size_t)written < sizeof devices[count].path) {
                written = snprintf(name_path, sizeof name_path, "%s/name", devices[count].path);
                if (written > 0 && (size_t)written < sizeof name_path &&
                    vs_read_text(name_path, driver, sizeof driver)) {
                    vs_copy(devices[count].driver, sizeof devices[count].driver, vs_trim(driver));
                } else {
                    vs_copy(devices[count].driver, sizeof devices[count].driver, "unknown");
                }
                count++;
            }
        }
        free(names[i]);
    }
    free(names);

    *out = devices;
    return count;
}

static bool read_sensor_label(const char *directory, const char *stem, char *out, size_t out_len)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s_label", directory, stem);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    if (!vs_read_text(path, out, out_len)) {
        return false;
    }
    vs_trim(out);
    return out[0] != '\0';
}

static bool read_sensor_value(const char *directory, const char *stem, const char *suffix,
                              int64_t *out)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s_%s", directory, stem, suffix);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    return vs_read_i64(path, out);
}

/* temp<N>_input / fan<N>_input names in one hwmon directory, sorted. */
static size_t list_sensor_stems(const char *directory, const char *prefix, char ***out)
{
    *out = NULL;
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return 0;
    }
    char **stems = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (strncmp(entry->d_name, prefix, prefix_length) != 0 || length <= 6 ||
            strcmp(entry->d_name + length - 6, "_input") != 0) {
            continue;
        }
        if (count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            char **grown = realloc(stems, next * sizeof *grown);
            if (grown == NULL) {
                break;
            }
            stems = grown;
            capacity = next;
        }
        char *stem = strndup(entry->d_name, length - 6);
        if (stem == NULL) {
            break;
        }
        stems[count++] = stem;
    }
    closedir(dir);
    if (count > 1) {
        qsort(stems, count, sizeof *stems, compare_names);
    }
    *out = stems;
    return count;
}

static void free_stems(char **stems, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(stems[i]);
    }
    free(stems);
}

/* Pick one primary per kind: the driver's package sensor, else the hottest
 * plausible reading of that kind. This is
 * TemperatureSensorSelector.displayedCPUTemperature, generalised from CPU to
 * every kind the panel has a badge for. */
static void mark_primaries(vs_temperature_sample *samples, size_t count)
{
    const vs_sensor_kind kinds[] = {VS_SENSOR_CPU, VS_SENSOR_GPU, VS_SENSOR_BATTERY,
                                    VS_SENSOR_DRIVE, VS_SENSOR_SYSTEM};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        size_t package_index = count;
        size_t hottest_index = count;
        for (size_t i = 0; i < count; i++) {
            if (samples[i].kind != kinds[k] ||
                !vs_sensor_temperature_plausible(samples[i].celsius)) {
                continue;
            }
            if (package_index == count && vs_sensor_label_is_package(samples[i].label)) {
                package_index = i;
            }
            if (hottest_index == count || samples[i].celsius > samples[hottest_index].celsius) {
                hottest_index = i;
            }
        }
        size_t chosen = package_index != count ? package_index : hottest_index;
        if (chosen != count) {
            samples[chosen].is_primary = true;
        }
    }
}

static vs_thermal_pressure pressure_of(const vs_temperature_sample *samples, size_t count)
{
    double worst = 0;
    for (size_t i = 0; i < count; i++) {
        if (samples[i].kind != VS_SENSOR_CPU && samples[i].kind != VS_SENSOR_GPU) {
            continue;
        }
        if (!vs_sensor_temperature_plausible(samples[i].celsius)) {
            continue;
        }
        double trip = samples[i].critical_celsius > 0 ? samples[i].critical_celsius
                                                      : samples[i].high_celsius;
        if (trip <= 0) {
            continue;
        }
        double ratio = samples[i].celsius / trip;
        if (ratio > worst) {
            worst = ratio;
        }
    }
    return vs_thermal_from_ratio(worst);
}

int vs_sensors_read_temperatures(vs_sensors_impl *impl, vs_temperature_sample **out,
                                 size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    hwmon_dir *devices = NULL;
    size_t device_count = list_hwmon(impl, &devices);

    vs_temperature_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result = VS_OK;

    for (size_t d = 0; d < device_count && result == VS_OK; d++) {
        char **stems = NULL;
        size_t stem_count = list_sensor_stems(devices[d].path, "temp", &stems);
        for (size_t s = 0; s < stem_count; s++) {
            int64_t millidegrees = 0;
            if (!read_sensor_value(devices[d].path, stems[s], "input", &millidegrees)) {
                continue;
            }
            if (count == capacity) {
                size_t next = capacity == 0 ? 16 : capacity * 2;
                vs_temperature_sample *grown = realloc(samples, next * sizeof *grown);
                if (grown == NULL) {
                    result = VS_ERR_NO_MEM;
                    break;
                }
                samples = grown;
                capacity = next;
            }
            vs_temperature_sample *sample = &samples[count];
            memset(sample, 0, sizeof *sample);
            snprintf(sample->id, sizeof sample->id, "%s/%s_input", devices[d].driver, stems[s]);
            vs_copy(sample->driver, sizeof sample->driver, devices[d].driver);
            char label[VS_SENSORS_LABEL_MAX];
            if (read_sensor_label(devices[d].path, stems[s], label, sizeof label)) {
                vs_copy(sample->label, sizeof sample->label, label);
            } else {
                snprintf(sample->label, sizeof sample->label, "%s %s", devices[d].driver, stems[s]);
            }
            sample->celsius = (double)millidegrees / 1000.0;
            int64_t trip = 0;
            if (read_sensor_value(devices[d].path, stems[s], "max", &trip)) {
                sample->high_celsius = (double)trip / 1000.0;
            }
            if (read_sensor_value(devices[d].path, stems[s], "crit", &trip)) {
                sample->critical_celsius = (double)trip / 1000.0;
            }
            sample->kind = vs_sensor_classify(devices[d].driver, sample->label);
            count++;
        }
        free_stems(stems, stem_count);
    }
    free(devices);

    if (result != VS_OK) {
        free(samples);
        return result;
    }

    mark_primaries(samples, count);
    impl->thermal = pressure_of(samples, count);

    *out = samples;
    *count_out = count;
    return VS_OK;
}

int vs_sensors_read_fans(vs_sensors_impl *impl, vs_fan_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    hwmon_dir *devices = NULL;
    size_t device_count = list_hwmon(impl, &devices);

    vs_fan_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result = VS_OK;

    for (size_t d = 0; d < device_count && result == VS_OK; d++) {
        char **stems = NULL;
        size_t stem_count = list_sensor_stems(devices[d].path, "fan", &stems);
        for (size_t s = 0; s < stem_count; s++) {
            int64_t rpm = 0;
            if (!read_sensor_value(devices[d].path, stems[s], "input", &rpm)) {
                continue;
            }
            if (count == capacity) {
                size_t next = capacity == 0 ? 8 : capacity * 2;
                vs_fan_sample *grown = realloc(samples, next * sizeof *grown);
                if (grown == NULL) {
                    result = VS_ERR_NO_MEM;
                    break;
                }
                samples = grown;
                capacity = next;
            }
            vs_fan_sample *sample = &samples[count];
            memset(sample, 0, sizeof *sample);
            snprintf(sample->id, sizeof sample->id, "%s/%s_input", devices[d].driver, stems[s]);
            vs_copy(sample->driver, sizeof sample->driver, devices[d].driver);
            char label[VS_SENSORS_LABEL_MAX];
            if (read_sensor_label(devices[d].path, stems[s], label, sizeof label)) {
                vs_copy(sample->label, sizeof sample->label, label);
            } else {
                snprintf(sample->label, sizeof sample->label, "%s %s", devices[d].driver, stems[s]);
            }
            sample->rpm = (int32_t)rpm;
            int64_t bound = 0;
            sample->min_rpm = read_sensor_value(devices[d].path, stems[s], "min", &bound)
                                  ? (int32_t)bound
                                  : -1;
            sample->max_rpm = read_sensor_value(devices[d].path, stems[s], "max", &bound)
                                  ? (int32_t)bound
                                  : -1;

            /* fanN and pwmN are the same channel N by hwmon convention. */
            sample->pwm = -1;
            const char *digits = stems[s] + 3;
            char pwm_path[VS_FULLPATH_MAX];
            int written = snprintf(pwm_path, sizeof pwm_path, "%s/pwm%s", devices[d].path, digits);
            if (written > 0 && (size_t)written < sizeof pwm_path && vs_path_exists(pwm_path)) {
                sample->is_controllable = true;
                vs_copy(sample->pwm_path, sizeof sample->pwm_path, pwm_path);
                int64_t pwm = 0;
                if (vs_read_i64(pwm_path, &pwm)) {
                    sample->pwm = (int32_t)pwm;
                }
            }
            count++;
        }
        free_stems(stems, stem_count);
    }
    free(devices);

    if (result != VS_OK) {
        free(samples);
        return result;
    }

    *out = samples;
    *count_out = count;
    return VS_OK;
}
