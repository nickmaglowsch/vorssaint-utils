/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * The `vs_sensors_system` vtable: capability probing, ownership, and the
 * dispatch-only event path.
 */

#include "vs_sensors_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

const char *vs_sensor_kind_string(vs_sensor_kind kind)
{
    switch (kind) {
    case VS_SENSOR_CPU:
        return "cpu";
    case VS_SENSOR_GPU:
        return "gpu";
    case VS_SENSOR_BATTERY:
        return "battery";
    case VS_SENSOR_DRIVE:
        return "drive";
    case VS_SENSOR_SYSTEM:
        return "system";
    case VS_SENSOR_OTHER:
        return "other";
    }
    return "other";
}

const char *vs_battery_state_string(vs_battery_state state)
{
    switch (state) {
    case VS_BATTERY_CHARGING:
        return "charging";
    case VS_BATTERY_DISCHARGING:
        return "discharging";
    case VS_BATTERY_EMPTY:
        return "empty";
    case VS_BATTERY_FULL:
        return "full";
    case VS_BATTERY_PENDING_CHARGE:
        return "pending-charge";
    case VS_BATTERY_PENDING_DISCHARGE:
        return "pending-discharge";
    case VS_BATTERY_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *vs_battery_kind_string(vs_battery_kind kind)
{
    switch (kind) {
    case VS_BATTERY_KIND_BATTERY:
        return "battery";
    case VS_BATTERY_KIND_UPS:
        return "ups";
    case VS_BATTERY_KIND_MOUSE:
        return "mouse";
    case VS_BATTERY_KIND_KEYBOARD:
        return "keyboard";
    case VS_BATTERY_KIND_HEADSET:
        return "headset";
    case VS_BATTERY_KIND_PHONE:
        return "phone";
    case VS_BATTERY_KIND_TOUCHPAD:
        return "touchpad";
    case VS_BATTERY_KIND_GAMEPAD:
        return "gamepad";
    case VS_BATTERY_KIND_PEN:
        return "pen";
    case VS_BATTERY_KIND_OTHER:
        return "other";
    case VS_BATTERY_KIND_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *vs_network_kind_string(vs_network_kind kind)
{
    switch (kind) {
    case VS_NETWORK_ETHERNET:
        return "ethernet";
    case VS_NETWORK_WIFI:
        return "wifi";
    case VS_NETWORK_LOOPBACK:
        return "loopback";
    case VS_NETWORK_VIRTUAL:
        return "virtual";
    case VS_NETWORK_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *vs_thermal_pressure_string(vs_thermal_pressure pressure)
{
    switch (pressure) {
    case VS_THERMAL_NOMINAL:
        return "nominal";
    case VS_THERMAL_FAIR:
        return "fair";
    case VS_THERMAL_SERIOUS:
        return "serious";
    case VS_THERMAL_CRITICAL:
        return "critical";
    }
    return "nominal";
}

const char *vs_gpu_vendor_string(vs_gpu_vendor vendor)
{
    switch (vendor) {
    case VS_GPU_VENDOR_AMD:
        return "amd";
    case VS_GPU_VENDOR_INTEL:
        return "intel";
    case VS_GPU_VENDOR_NVIDIA:
        return "nvidia";
    case VS_GPU_VENDOR_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

/* ---------------------------------------------------------------- probing */

static bool directory_has_entry(const vs_sensors_impl *impl, const char *path, const char *prefix)
{
    char full[VS_FULLPATH_MAX];
    if (!vs_path(impl, full, sizeof full, path)) {
        return false;
    }
    DIR *dir = opendir(full);
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (prefix == NULL || strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

/* A DRM card whose `device` subtree answers `gpu_busy_percent` is an amdgpu
 * that can report utilisation; a `gt` tree is Intel's frequency interface. */
static void probe_drm(vs_sensors_impl *impl, uint32_t *capabilities)
{
    char base[VS_FULLPATH_MAX];
    if (!vs_path(impl, base, sizeof base, "/sys/class/drm")) {
        return;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') != NULL) {
            continue;
        }
        char path[VS_FULLPATH_MAX];
        int written = snprintf(path, sizeof path, "%s/%s/device/gpu_busy_percent", base,
                               entry->d_name);
        if (written > 0 && (size_t)written < sizeof path && vs_path_exists(path)) {
            *capabilities |= VS_SENSORS_HAS_AMDGPU;
        }
        written = snprintf(path, sizeof path, "%s/%s/gt", base, entry->d_name);
        if (written > 0 && (size_t)written < sizeof path && vs_dir_exists(path)) {
            *capabilities |= VS_SENSORS_HAS_INTEL_GPU;
        } else {
            written = snprintf(path, sizeof path, "%s/%s/gt_cur_freq_mhz", base, entry->d_name);
            if (written > 0 && (size_t)written < sizeof path && vs_path_exists(path)) {
                *capabilities |= VS_SENSORS_HAS_INTEL_GPU;
            }
        }
    }
    closedir(dir);
}

static bool probe_procfs_io(const vs_sensors_impl *impl)
{
    char path[VS_FULLPATH_MAX];
    /* Our own /proc/self/io is the honest probe: readable for the processes we
     * own, which is the set the breakdown can describe. */
    if (!vs_path(impl, path, sizeof path, "/proc/self/io")) {
        return false;
    }
    return vs_path_exists(path);
}

/* ----------------------------------------------------------------- vtable */

static vs_sensors_impl *impl_of(vs_sensors_system *self)
{
    return self != NULL ? (vs_sensors_impl *)self->impl : NULL;
}

static int op_cpu(vs_sensors_system *self, vs_cpu_sample *out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_cpu(impl, out) : VS_ERR_INVALID;
}

static int op_memory(vs_sensors_system *self, vs_memory_sample *out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_memory(impl, out) : VS_ERR_INVALID;
}

static int op_network(vs_sensors_system *self, vs_network_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_network(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_network(vs_sensors_system *self, vs_network_sample *samples, size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_disk_devices(vs_sensors_system *self, vs_disk_device_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_disk_devices(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_disk_devices(vs_sensors_system *self, vs_disk_device_sample *samples,
                                 size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_disk_mounts(vs_sensors_system *self, vs_disk_mount_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_disk_mounts(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_disk_mounts(vs_sensors_system *self, vs_disk_mount_sample *samples,
                                size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_temperatures(vs_sensors_system *self, vs_temperature_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_temperatures(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_temperatures(vs_sensors_system *self, vs_temperature_sample *samples,
                                 size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_fans(vs_sensors_system *self, vs_fan_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_fans(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_fans(vs_sensors_system *self, vs_fan_sample *samples, size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_power(vs_sensors_system *self, vs_power_sample *out)
{
    vs_sensors_impl *impl = impl_of(self);
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    if ((self->capabilities & VS_SENSORS_HAS_UPOWER) != 0) {
        int status = vs_power_read_upower(impl, out);
        if (status == VS_OK) {
            return VS_OK;
        }
        /* UPower answered the probe and then failed: fall through rather than
         * leaving the panel with nothing, and let `dispatch` decide whether the
         * daemon is really gone. */
    }
    if ((self->capabilities & VS_SENSORS_HAS_POWER_SUPPLY) != 0) {
        return vs_power_read_sysfs(impl, out);
    }
    return VS_ERR_UNSUPPORTED;
}

static int op_peripheral_batteries(vs_sensors_system *self, vs_battery_sample **out,
                                   size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    if ((self->capabilities & VS_SENSORS_HAS_UPOWER) != 0) {
        int status = vs_power_peripherals_upower(impl, out, count_out);
        if (status == VS_OK) {
            return VS_OK;
        }
    }
    if ((self->capabilities & VS_SENSORS_HAS_POWER_SUPPLY) != 0) {
        /* The kernel marks a device's own battery with scope=Device; HID
         * peripherals that bind to a power_supply node show up there even
         * without UPower. */
        return vs_power_peripherals_sysfs(impl, out, count_out);
    }
    *out = NULL;
    *count_out = 0;
    return VS_OK;
}

static void op_free_batteries(vs_sensors_system *self, vs_battery_sample *samples, size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_gpus(vs_sensors_system *self, vs_gpu_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_gpus(impl, out, count_out) : VS_ERR_INVALID;
}

static void op_free_gpus(vs_sensors_system *self, vs_gpu_sample *samples, size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_processes(vs_sensors_system *self, const vs_process_query *query,
                        vs_process_sample **out, size_t *count_out)
{
    vs_sensors_impl *impl = impl_of(self);
    return impl != NULL ? vs_sensors_read_processes(impl, query, out, count_out) : VS_ERR_INVALID;
}

static void op_free_processes(vs_sensors_system *self, vs_process_sample *samples, size_t count)
{
    (void)self;
    (void)count;
    free(samples);
}

static int op_thermal_pressure(vs_sensors_system *self, vs_thermal_pressure *out)
{
    vs_sensors_impl *impl = impl_of(self);
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    if ((self->capabilities & VS_SENSORS_HAS_HWMON) == 0) {
        return VS_ERR_UNSUPPORTED;
    }
    *out = impl->thermal;
    return VS_OK;
}

static int op_set_event_callback(vs_sensors_system *self, vs_sensors_event_cb callback,
                                 void *user_data)
{
    vs_sensors_impl *impl = impl_of(self);
    if (impl == NULL) {
        return VS_ERR_INVALID;
    }
    impl->callback = callback;
    impl->callback_user = user_data;
    return VS_OK;
}

static int op_event_fd(vs_sensors_system *self)
{
    (void)self;
    /* Nothing here is pollable: hwmon has no notification interface and UPower
     * signals would need a thread or a bus loop the caller does not own. The
     * caller polls, and `dispatch` turns the polled state into events. */
    return -1;
}

static int op_dispatch(vs_sensors_system *self)
{
    vs_sensors_impl *impl = impl_of(self);
    if (impl == NULL) {
        return VS_ERR_INVALID;
    }
    int delivered = 0;

    if ((self->capabilities & VS_SENSORS_HAS_UPOWER) != 0 && !vs_upower_alive(impl)) {
        vs_upower_disconnect(impl);
        self->capabilities &= ~(uint32_t)VS_SENSORS_HAS_UPOWER;
        self->power_backend_name =
            (self->capabilities & VS_SENSORS_HAS_POWER_SUPPLY) != 0 ? "power_supply" : "none";
        if (impl->callback != NULL) {
            vs_sensors_event event;
            memset(&event, 0, sizeof event);
            event.type = VS_SENSORS_EVENT_BACKEND_LOST;
            event.thermal_pressure = impl->thermal;
            impl->callback(&event, impl->callback_user);
            delivered++;
        }
    }

    if (impl->thermal != impl->announced_thermal) {
        impl->announced_thermal = impl->thermal;
        if (impl->callback != NULL) {
            vs_sensors_event event;
            memset(&event, 0, sizeof event);
            event.type = VS_SENSORS_EVENT_THERMAL_PRESSURE;
            event.thermal_pressure = impl->thermal;
            impl->callback(&event, impl->callback_user);
            delivered++;
        }
    }

    return delivered;
}

static void op_destroy(vs_sensors_system *self)
{
    if (self == NULL) {
        return;
    }
    vs_sensors_impl *impl = impl_of(self);
    if (impl != NULL) {
        vs_upower_disconnect(impl);
        vs_nvml_close(impl->nvml);
        free(impl->prev_net);
        free(impl->prev_disk);
        free(impl->prev_proc);
        free(impl->desktop);
        free(impl);
    }
    free(self);
}

vs_sensors_system *vs_sensors_system_create(const vs_sensors_options *options, int *result_out)
{
    vs_sensors_options defaults;
    memset(&defaults, 0, sizeof defaults);
    if (options == NULL) {
        options = &defaults;
    }

    vs_sensors_impl *impl = calloc(1, sizeof *impl);
    if (impl == NULL) {
        if (result_out != NULL) {
            *result_out = VS_ERR_NO_MEM;
        }
        return NULL;
    }

    if (options->root != NULL && options->root[0] != '\0' && strcmp(options->root, "/") != 0) {
        size_t length = strlen(options->root);
        if (length >= sizeof impl->root) {
            free(impl);
            if (result_out != NULL) {
                *result_out = VS_ERR_INVALID;
            }
            return NULL;
        }
        vs_copy(impl->root, sizeof impl->root, options->root);
        while (length > 1 && impl->root[length - 1] == '/') {
            impl->root[--length] = '\0';
        }
        impl->rooted = true;
    }

    /* /proc/stat is the floor: without it nothing here can answer anything. */
    char probe[VS_FULLPATH_MAX];
    if (!vs_path(impl, probe, sizeof probe, "/proc/stat") || !vs_path_exists(probe)) {
        free(impl);
        if (result_out != NULL) {
            *result_out = VS_ERR_NO_BACKEND;
        }
        return NULL;
    }

    uint32_t capabilities = 0;
    if (directory_has_entry(impl, "/sys/class/hwmon", "hwmon")) {
        capabilities |= VS_SENSORS_HAS_HWMON;
    }
    if (vs_power_supply_present(impl)) {
        capabilities |= VS_SENSORS_HAS_POWER_SUPPLY;
    }
    if (vs_path(impl, probe, sizeof probe, "/proc/pressure/memory") && vs_path_exists(probe)) {
        capabilities |= VS_SENSORS_HAS_PSI;
    }
    if (probe_procfs_io(impl)) {
        capabilities |= VS_SENSORS_HAS_PROCFS_IO;
    }
    probe_drm(impl, &capabilities);

    const char *backend = "none";
    bool want_upower = !impl->rooted;
    if (options->power_backend != NULL) {
        want_upower = strcmp(options->power_backend, "upower") == 0 && !impl->rooted;
        if (strcmp(options->power_backend, "sysfs") == 0) {
            want_upower = false;
        }
    }
    if (want_upower && vs_upower_connect(impl)) {
        capabilities |= VS_SENSORS_HAS_UPOWER;
        backend = "upower";
    } else if ((capabilities & VS_SENSORS_HAS_POWER_SUPPLY) != 0) {
        backend = "power_supply";
    }

    if (!options->disable_nvml) {
        impl->nvml = vs_nvml_open();
        if (impl->nvml != NULL) {
            capabilities |= VS_SENSORS_HAS_NVML;
        }
    }

    vs_sensors_system *system = calloc(1, sizeof *system);
    if (system == NULL) {
        vs_upower_disconnect(impl);
        vs_nvml_close(impl->nvml);
        free(impl);
        if (result_out != NULL) {
            *result_out = VS_ERR_NO_MEM;
        }
        return NULL;
    }

    system->name = "procfs";
    system->power_backend_name = backend;
    system->capabilities = capabilities;
    system->impl = impl;
    system->cpu = op_cpu;
    system->memory = op_memory;
    system->network = op_network;
    system->free_network = op_free_network;
    system->disk_devices = op_disk_devices;
    system->free_disk_devices = op_free_disk_devices;
    system->disk_mounts = op_disk_mounts;
    system->free_disk_mounts = op_free_disk_mounts;
    system->temperatures = op_temperatures;
    system->free_temperatures = op_free_temperatures;
    system->fans = op_fans;
    system->free_fans = op_free_fans;
    system->power = op_power;
    system->peripheral_batteries = op_peripheral_batteries;
    system->free_batteries = op_free_batteries;
    system->gpus = op_gpus;
    system->free_gpus = op_free_gpus;
    system->processes = op_processes;
    system->free_processes = op_free_processes;
    system->thermal_pressure = op_thermal_pressure;
    system->set_event_callback = op_set_event_callback;
    system->event_fd = op_event_fd;
    system->dispatch = op_dispatch;
    system->destroy = op_destroy;

    if (result_out != NULL) {
        *result_out = VS_OK;
    }
    return system;
}
