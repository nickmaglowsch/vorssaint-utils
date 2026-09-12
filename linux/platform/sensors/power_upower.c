/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * UPower over the system bus: the preferred power source when it is running,
 * because it is the only one that knows about peripheral batteries (a mouse, a
 * keyboard, a headset) and because it does the charge-rate smoothing that
 * AppleSmartBattery does on macOS.
 *
 * `org.freedesktop.UPower.Device` properties read here, against
 * `PowerReading` in PowerSampler.swift:
 *
 *   Percentage    -> chargePercent          State       -> isCharging
 *   EnergyRate    -> batteryWatts (signed by State)
 *   TimeToEmpty   -> timeRemainingSeconds   TimeToFull  -> (charging estimate)
 *   Temperature   -> batteryTemperature     Capacity    -> healthPercent
 *   ChargeCycles  -> cycleCount (UPower 0.99.12+; absent before, and then the
 *                    field stays absent rather than being guessed)
 *   Online (on a Type=LinePower device) -> externalConnected
 *
 * UPower publishes no adapter wattage, so `adapter_watts` and
 * `adapter_max_watts` are filled from `/sys/class/power_supply` even on this
 * path; that is a read of the same kernel objects UPower itself watches.
 */

#include "vs_sensors_internal.h"

#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define UPOWER_SERVICE "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_DEVICE_INTERFACE "org.freedesktop.UPower.Device"

/* org.freedesktop.UPower.Device Type */
enum {
    UPOWER_TYPE_UNKNOWN = 0,
    UPOWER_TYPE_LINE_POWER = 1,
    UPOWER_TYPE_BATTERY = 2,
    UPOWER_TYPE_UPS = 3,
    UPOWER_TYPE_MOUSE = 5,
    UPOWER_TYPE_KEYBOARD = 6,
    UPOWER_TYPE_PHONE = 8,
    UPOWER_TYPE_TABLET = 10,
    UPOWER_TYPE_GAMING_INPUT = 12,
    UPOWER_TYPE_PEN = 13,
    UPOWER_TYPE_TOUCHPAD = 14,
    UPOWER_TYPE_HEADSET = 17,
    UPOWER_TYPE_HEADPHONES = 19,
};

vs_battery_kind vs_battery_kind_from_upower_type(uint32_t type)
{
    switch (type) {
    case UPOWER_TYPE_BATTERY:
        return VS_BATTERY_KIND_BATTERY;
    case UPOWER_TYPE_UPS:
        return VS_BATTERY_KIND_UPS;
    case UPOWER_TYPE_MOUSE:
        return VS_BATTERY_KIND_MOUSE;
    case UPOWER_TYPE_KEYBOARD:
        return VS_BATTERY_KIND_KEYBOARD;
    case UPOWER_TYPE_HEADSET:
    case UPOWER_TYPE_HEADPHONES:
        return VS_BATTERY_KIND_HEADSET;
    case UPOWER_TYPE_PHONE:
    case UPOWER_TYPE_TABLET:
        return VS_BATTERY_KIND_PHONE;
    case UPOWER_TYPE_TOUCHPAD:
        return VS_BATTERY_KIND_TOUCHPAD;
    case UPOWER_TYPE_GAMING_INPUT:
        return VS_BATTERY_KIND_GAMEPAD;
    case UPOWER_TYPE_PEN:
        return VS_BATTERY_KIND_PEN;
    case UPOWER_TYPE_UNKNOWN:
        return VS_BATTERY_KIND_UNKNOWN;
    default:
        return VS_BATTERY_KIND_OTHER;
    }
}

static vs_battery_state state_from_upower(uint32_t state)
{
    switch (state) {
    case 1:
        return VS_BATTERY_CHARGING;
    case 2:
        return VS_BATTERY_DISCHARGING;
    case 3:
        return VS_BATTERY_EMPTY;
    case 4:
        return VS_BATTERY_FULL;
    case 5:
        return VS_BATTERY_PENDING_CHARGE;
    case 6:
        return VS_BATTERY_PENDING_DISCHARGE;
    default:
        return VS_BATTERY_UNKNOWN;
    }
}

static bool property_double(sd_bus *bus, const char *path, const char *name, double *out)
{
    return sd_bus_get_property_trivial(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name,
                                       NULL, 'd', out) >= 0;
}

static bool property_uint32(sd_bus *bus, const char *path, const char *name, uint32_t *out)
{
    return sd_bus_get_property_trivial(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name,
                                       NULL, 'u', out) >= 0;
}

static bool property_int64(sd_bus *bus, const char *path, const char *name, int64_t *out)
{
    return sd_bus_get_property_trivial(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name,
                                       NULL, 'x', out) >= 0;
}

static bool property_int32(sd_bus *bus, const char *path, const char *name, int32_t *out)
{
    return sd_bus_get_property_trivial(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name,
                                       NULL, 'i', out) >= 0;
}

static bool property_bool(sd_bus *bus, const char *path, const char *name, bool *out)
{
    int value = 0;
    if (sd_bus_get_property_trivial(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name, NULL,
                                    'b', &value) < 0) {
        return false;
    }
    *out = value != 0;
    return true;
}

static bool property_string(sd_bus *bus, const char *path, const char *name, char *out,
                            size_t out_len)
{
    char *value = NULL;
    if (sd_bus_get_property_string(bus, UPOWER_SERVICE, path, UPOWER_DEVICE_INTERFACE, name, NULL,
                                   &value) < 0) {
        return false;
    }
    vs_copy(out, out_len, value);
    free(value);
    return true;
}

bool vs_upower_connect(vs_sensors_impl *impl)
{
    if (impl == NULL || impl->rooted) {
        return false;
    }
    sd_bus *bus = NULL;
    if (sd_bus_default_system(&bus) < 0 || bus == NULL) {
        return false;
    }
    /* Owning the name is not enough: ask for something only UPower answers, so
     * a stale activation entry does not look like a running daemon. */
    char *version = NULL;
    int status = sd_bus_get_property_string(bus, UPOWER_SERVICE, UPOWER_PATH, UPOWER_SERVICE,
                                            "DaemonVersion", NULL, &version);
    if (status < 0) {
        sd_bus_unref(bus);
        return false;
    }
    free(version);
    impl->bus = bus;
    return true;
}

void vs_upower_disconnect(vs_sensors_impl *impl)
{
    if (impl == NULL || impl->bus == NULL) {
        return;
    }
    sd_bus_unref((sd_bus *)impl->bus);
    impl->bus = NULL;
}

bool vs_upower_alive(vs_sensors_impl *impl)
{
    if (impl == NULL || impl->bus == NULL) {
        return false;
    }
    char *version = NULL;
    int status = sd_bus_get_property_string((sd_bus *)impl->bus, UPOWER_SERVICE, UPOWER_PATH,
                                            UPOWER_SERVICE, "DaemonVersion", NULL, &version);
    if (status < 0) {
        return false;
    }
    free(version);
    return true;
}

static void read_device(sd_bus *bus, const char *path, vs_battery_sample *out)
{
    memset(out, 0, sizeof *out);
    const char *tail = strrchr(path, '/');
    vs_copy(out->id, sizeof out->id, tail != NULL ? tail + 1 : path);
    vs_copy(out->label, sizeof out->label, out->id);

    char text[VS_SENSORS_LABEL_MAX];
    if (property_string(bus, path, "Model", text, sizeof text) && text[0] != '\0') {
        vs_copy(out->label, sizeof out->label, text);
    }
    char vendor[VS_SENSORS_NAME_MAX];
    if (property_string(bus, path, "Vendor", vendor, sizeof vendor)) {
        vs_copy(out->vendor, sizeof out->vendor, vendor);
    }

    uint32_t type = 0;
    if (property_uint32(bus, path, "Type", &type)) {
        out->kind = vs_battery_kind_from_upower_type(type);
    }
    uint32_t state = 0;
    if (property_uint32(bus, path, "State", &state)) {
        out->state = state_from_upower(state);
    }

    double value = 0;
    if (property_double(bus, path, "Percentage", &value)) {
        out->percentage = value;
        out->has_percentage = true;
    }
    if (property_double(bus, path, "EnergyRate", &value) && value != 0) {
        /* EnergyRate is unsigned; the direction is in State, exactly as
         * AppleSmartBattery's signed Amperage encodes it on macOS. */
        double magnitude = value < 0 ? -value : value;
        out->watts = out->state == VS_BATTERY_DISCHARGING ? -magnitude : magnitude;
        out->has_watts = true;
    }
    int64_t seconds = 0;
    if (property_int64(bus, path, "TimeToEmpty", &seconds) && seconds > 0) {
        out->time_to_empty_seconds = (double)seconds;
        out->has_time_to_empty = true;
    }
    if (property_int64(bus, path, "TimeToFull", &seconds) && seconds > 0) {
        out->time_to_full_seconds = (double)seconds;
        out->has_time_to_full = true;
    }
    if (property_double(bus, path, "Temperature", &value) && value != 0) {
        out->temperature_celsius = value;
        out->has_temperature = true;
    }
    if (property_double(bus, path, "Capacity", &value) && value > 0) {
        /* UPower's Capacity is a percentage of design capacity; the Swift side
         * wants a 0...1 fraction. */
        out->health = value / 100.0;
        out->has_health = true;
    }
    int32_t cycles = 0;
    if (property_int32(bus, path, "ChargeCycles", &cycles) && cycles > 0) {
        out->cycle_count = cycles;
        out->has_cycle_count = true;
    }
    if (property_double(bus, path, "Energy", &value)) {
        out->energy_wh = value;
    }
    if (property_double(bus, path, "EnergyFull", &value)) {
        out->energy_full_wh = value;
    }
    if (property_double(bus, path, "EnergyFullDesign", &value)) {
        out->energy_full_design_wh = value;
    }
    if (property_double(bus, path, "Voltage", &value)) {
        out->voltage_v = value;
    }
}

/* EnumerateDevices returns an array of object paths; the caller frees each. */
static int enumerate(sd_bus *bus, char ***paths_out, size_t *count_out)
{
    *paths_out = NULL;
    *count_out = 0;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int status = sd_bus_call_method(bus, UPOWER_SERVICE, UPOWER_PATH, UPOWER_SERVICE,
                                    "EnumerateDevices", &error, &reply, "");
    if (status < 0) {
        sd_bus_error_free(&error);
        return VS_ERR_BACKEND;
    }
    sd_bus_error_free(&error);

    status = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "o");
    if (status < 0) {
        sd_bus_message_unref(reply);
        return VS_ERR_BACKEND;
    }

    char **paths = NULL;
    size_t count = 0;
    size_t capacity = 0;
    const char *path = NULL;
    int result = VS_OK;
    while (sd_bus_message_read_basic(reply, SD_BUS_TYPE_OBJECT_PATH, &path) > 0) {
        if (count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            char **grown = realloc(paths, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            paths = grown;
            capacity = next;
        }
        paths[count] = strdup(path);
        if (paths[count] == NULL) {
            result = VS_ERR_NO_MEM;
            break;
        }
        count++;
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    if (result != VS_OK) {
        for (size_t i = 0; i < count; i++) {
            free(paths[i]);
        }
        free(paths);
        return result;
    }
    *paths_out = paths;
    *count_out = count;
    return VS_OK;
}

static void free_paths(char **paths, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(paths[i]);
    }
    free(paths);
}

int vs_power_read_upower(vs_sensors_impl *impl, vs_power_sample *out)
{
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    if (impl->bus == NULL) {
        return VS_ERR_UNSUPPORTED;
    }
    sd_bus *bus = (sd_bus *)impl->bus;

    char **paths = NULL;
    size_t count = 0;
    int status = enumerate(bus, &paths, &count);
    if (status != VS_OK) {
        return status;
    }

    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < count; i++) {
        uint32_t type = 0;
        if (!property_uint32(bus, paths[i], "Type", &type)) {
            continue;
        }
        bool power_supply = false;
        if (!property_bool(bus, paths[i], "PowerSupply", &power_supply)) {
            power_supply = false;
        }
        if (type == UPOWER_TYPE_LINE_POWER) {
            bool online = false;
            if (property_bool(bus, paths[i], "Online", &online) && online) {
                out->external_connected = true;
            }
        } else if (type == UPOWER_TYPE_BATTERY && power_supply && !out->has_battery) {
            read_device(bus, paths[i], &out->battery);
            out->has_battery = true;
        }
    }
    free_paths(paths, count);

    if (out->has_battery && out->battery.has_watts &&
        out->battery.state == VS_BATTERY_DISCHARGING) {
        out->system_watts = -out->battery.watts;
        out->has_system_watts = true;
    }

    /* UPower has no adapter wattage; the kernel objects it watches do. */
    if (vs_power_supply_present(impl)) {
        vs_power_sample sysfs;
        if (vs_power_read_sysfs(impl, &sysfs) == VS_OK) {
            if (sysfs.has_adapter_watts) {
                out->adapter_watts = sysfs.adapter_watts;
                out->has_adapter_watts = true;
            }
            if (sysfs.has_adapter_max_watts) {
                out->adapter_max_watts = sysfs.adapter_max_watts;
                out->has_adapter_max_watts = true;
            }
            if (!out->external_connected && sysfs.external_connected) {
                out->external_connected = true;
            }
        }
    }

    return VS_OK;
}

int vs_power_peripherals_upower(vs_sensors_impl *impl, vs_battery_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;
    if (impl->bus == NULL) {
        return VS_ERR_UNSUPPORTED;
    }
    sd_bus *bus = (sd_bus *)impl->bus;

    char **paths = NULL;
    size_t path_count = 0;
    int status = enumerate(bus, &paths, &path_count);
    if (status != VS_OK) {
        return status;
    }

    vs_battery_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result = VS_OK;
    for (size_t i = 0; i < path_count; i++) {
        uint32_t type = 0;
        if (!property_uint32(bus, paths[i], "Type", &type) || type == UPOWER_TYPE_LINE_POWER) {
            continue;
        }
        bool power_supply = false;
        if (property_bool(bus, paths[i], "PowerSupply", &power_supply) && power_supply) {
            /* The machine's own battery; `power()` reports that one. */
            continue;
        }
        if (count == capacity) {
            size_t next = capacity == 0 ? 4 : capacity * 2;
            vs_battery_sample *grown = realloc(samples, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            samples = grown;
            capacity = next;
        }
        read_device(bus, paths[i], &samples[count]);
        count++;
    }
    free_paths(paths, path_count);

    if (result != VS_OK) {
        free(samples);
        return result;
    }
    *out = samples;
    *count_out = count;
    return VS_OK;
}
