/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Private to linux/platform/sensors. The contract is
 * include/vorssaint_platform.h; nothing outside this directory includes this.
 */

#ifndef VS_SENSORS_INTERNAL_H
#define VS_SENSORS_INTERNAL_H

#include "vorssaint_platform.h"

#include <stdio.h>

/* Every reader is rooted, so a fixture tree replaces /proc and /sys wholesale.
 * The root is stored without a trailing slash; "/" is stored as "". */
#define VS_ROOT_MAX 512
#define VS_FULLPATH_MAX 1024

typedef struct vs_cpu_ticks {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
    bool valid;
} vs_cpu_ticks;

typedef struct vs_counter_prev {
    char name[VS_SENSORS_NAME_MAX];
    uint64_t a;
    uint64_t b;
} vs_counter_prev;

typedef struct vs_proc_prev {
    int32_t pid;
    uint64_t cpu_time_ns;
} vs_proc_prev;

typedef struct vs_desktop_entry {
    /* Basename of the .desktop file without the suffix: "org.gnome.Nautilus". */
    char id[VS_SENSORS_ID_MAX];
    /* Basename of the first Exec token: "nautilus". */
    char exec[VS_SENSORS_NAME_MAX];
    char name[VS_SENSORS_LABEL_MAX];
} vs_desktop_entry;

struct vs_nvml;

typedef struct vs_sensors_impl {
    char root[VS_ROOT_MAX];
    bool rooted;

    vs_cpu_ticks prev_total;
    vs_cpu_ticks prev_core[VS_SENSORS_MAX_CORES];
    double prev_cpu_time;

    /* Topology is fixed for the life of a boot, and reading it is two file
     * opens per core per sample otherwise. Frequency is not cached: it is the
     * one part of the tree that moves. */
    bool topology_scanned;
    size_t topology_cores;
    int32_t package_id[VS_SENSORS_MAX_CORES];
    int32_t core_id[VS_SENSORS_MAX_CORES];
    size_t physical_core_count;
    size_t package_count;

    vs_counter_prev *prev_net;
    size_t prev_net_count;
    double prev_net_time;

    vs_counter_prev *prev_disk;
    size_t prev_disk_count;
    double prev_disk_time;

    vs_proc_prev *prev_proc;
    size_t prev_proc_count;
    double prev_proc_time;

    /* Built once on first use; a desktop file index is ~1000 small reads. */
    vs_desktop_entry *desktop;
    size_t desktop_count;
    bool desktop_scanned;

    struct vs_nvml *nvml;

    /* sd_bus *, opaque here so only power_upower.c needs the header. */
    void *bus;

    vs_thermal_pressure thermal;
    vs_thermal_pressure announced_thermal;

    vs_sensors_event_cb callback;
    void *callback_user;
} vs_sensors_impl;

/* ------------------------------------------------------------------- paths */

/** Join the instance root and an absolute kernel path ("/proc/stat") into
 *  `out`. Returns false when it would not fit, and `out` is then empty. */
bool vs_path(const vs_sensors_impl *impl, char *out, size_t out_len, const char *path);
/** As `vs_path`, with a printf format for the tail. */
bool vs_pathf(const vs_sensors_impl *impl, char *out, size_t out_len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* ---------------------------------------------------------------- file I/O */

/** Whole small file into `buf`, NUL-terminated, trailing newline removed.
 *  False when it cannot be opened or is empty. */
bool vs_read_text(const char *path, char *buf, size_t buf_len);
bool vs_read_u64(const char *path, uint64_t *out);
bool vs_read_i64(const char *path, int64_t *out);
bool vs_read_double(const char *path, double *out);
bool vs_path_exists(const char *path);
bool vs_dir_exists(const char *path);

/** Open a rooted path for reading, NULL on failure. */
FILE *vs_open_rooted(const vs_sensors_impl *impl, const char *path);

/* -------------------------------------------------------------- small util */

void vs_copy(char *dst, size_t dst_len, const char *src);
/** Trim ASCII whitespace in place and return `s`. */
char *vs_trim(char *s);
/** Case-insensitive substring test. `strcasestr` is a GNU extension that needs
 *  _GNU_SOURCE; this keeps the sources plain C11 with no feature-test macro. */
bool vs_contains_ci(const char *haystack, const char *needle);
/** Seconds on CLOCK_MONOTONIC. */
double vs_now(void);
/** Positive difference of two cumulative counters, 0 on a reset: the rule
 *  `MetricFormat.netSpeed` and `diskSpeed` follow. */
double vs_rate(uint64_t previous, uint64_t current, double elapsed);

/* ------------------------------------------------------------- subsystems */

int vs_sensors_read_cpu(vs_sensors_impl *impl, vs_cpu_sample *out);
int vs_sensors_read_memory(vs_sensors_impl *impl, vs_memory_sample *out);
int vs_sensors_read_network(vs_sensors_impl *impl, vs_network_sample **out, size_t *count_out);
int vs_sensors_read_disk_devices(vs_sensors_impl *impl, vs_disk_device_sample **out, size_t *count_out);
int vs_sensors_read_disk_mounts(vs_sensors_impl *impl, vs_disk_mount_sample **out, size_t *count_out);
int vs_sensors_read_temperatures(vs_sensors_impl *impl, vs_temperature_sample **out, size_t *count_out);
int vs_sensors_read_fans(vs_sensors_impl *impl, vs_fan_sample **out, size_t *count_out);
int vs_sensors_read_processes(vs_sensors_impl *impl, const vs_process_query *query,
                              vs_process_sample **out, size_t *count_out);
int vs_sensors_read_gpus(vs_sensors_impl *impl, vs_gpu_sample **out, size_t *count_out);

/** Classification rules that replace TemperatureSensorSelector's SMC-key
 *  prefixes. Exposed for the unit tests. */
vs_sensor_kind vs_sensor_classify(const char *driver, const char *label);
/** True for a label the driver publishes as the package/die reading. */
bool vs_sensor_label_is_package(const char *label);
/** The macOS plausibility window: 10 <= t < 125. */
bool vs_sensor_temperature_plausible(double celsius);
vs_thermal_pressure vs_thermal_from_ratio(double ratio);

/* power */
bool vs_power_supply_present(const vs_sensors_impl *impl);
int vs_power_read_sysfs(vs_sensors_impl *impl, vs_power_sample *out);
int vs_power_read_upower(vs_sensors_impl *impl, vs_power_sample *out);
int vs_power_peripherals_upower(vs_sensors_impl *impl, vs_battery_sample **out, size_t *count_out);
/** The kernel marks a device's own battery with scope=Device, so HID
 *  peripherals that bind a power_supply node are visible without UPower. */
int vs_power_peripherals_sysfs(vs_sensors_impl *impl, vs_battery_sample **out, size_t *count_out);
/** Connects to the system bus and checks that UPower answers. False (and no
 *  bus) when it is not there. */
bool vs_upower_connect(vs_sensors_impl *impl);
void vs_upower_disconnect(vs_sensors_impl *impl);
/** Still on the bus? Used by `dispatch` to shrink capabilities honestly. */
bool vs_upower_alive(vs_sensors_impl *impl);

vs_battery_state vs_battery_state_from_string(const char *text);
vs_battery_kind vs_battery_kind_from_upower_type(uint32_t type);
vs_battery_kind vs_battery_kind_from_sysfs(const char *type, const char *name);

/* NVML, dlopened at runtime and never linked. */
struct vs_nvml *vs_nvml_open(void);
void vs_nvml_close(struct vs_nvml *nvml);
/** Appends every NVML device to `*out` (realloc'd), returns the number added. */
size_t vs_nvml_collect(struct vs_nvml *nvml, vs_gpu_sample **out, size_t *count, size_t *capacity);

#endif /* VS_SENSORS_INTERNAL_H */
