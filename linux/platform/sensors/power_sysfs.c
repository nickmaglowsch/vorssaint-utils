/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/sys/class/power_supply`: the fallback for machines with no UPower on the
 * bus (a minimal wlroots session, a Flatpak sandbox without the system bus).
 * It is also the only power path a fixture tree can exercise, so it is what the
 * tests here drive.
 *
 * Field-for-field against `PowerReading` in
 * Sources/Vorssaint/Services/Metrics/PowerSampler.swift:
 *
 *   chargePercent        capacity, else energy_now/energy_full
 *   isCharging           status == "Charging"
 *   externalConnected    a Mains/USB supply with online == 1
 *   batteryWatts         power_now, else voltage_now * current_now; signed by
 *                        status, the same convention AppleSmartBattery's signed
 *                        Amperage gives
 *   timeRemainingSeconds time_to_empty_now, else energy_now / power_now
 *   healthPercent        energy_full / energy_full_design (or the charge pair)
 *   cycleCount           cycle_count
 *   adapterWatts         the mains supply's power_now
 *   adapterMaxWatts      voltage_max_design * current_max, else
 *                        input_power_limit
 *   systemWatts          no Linux equivalent of the SMC PSTR key: while
 *                        discharging the battery's own draw IS the machine's
 *                        draw and is reported; while plugged in nothing on a
 *                        stock kernel measures it, and it stays absent.
 */

#include "vs_sensors_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool supply_u64(const char *directory, const char *file, uint64_t *out)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", directory, file);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    return vs_read_u64(path, out);
}

static bool supply_i64(const char *directory, const char *file, int64_t *out)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", directory, file);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    return vs_read_i64(path, out);
}

static bool supply_text(const char *directory, const char *file, char *out, size_t out_len)
{
    char path[VS_FULLPATH_MAX];
    int written = snprintf(path, sizeof path, "%s/%s", directory, file);
    if (written < 0 || (size_t)written >= sizeof path) {
        return false;
    }
    if (!vs_read_text(path, out, out_len)) {
        return false;
    }
    vs_trim(out);
    return out[0] != '\0';
}

vs_battery_state vs_battery_state_from_string(const char *text)
{
    if (text == NULL) {
        return VS_BATTERY_UNKNOWN;
    }
    if (strcasecmp(text, "charging") == 0) {
        return VS_BATTERY_CHARGING;
    }
    if (strcasecmp(text, "discharging") == 0) {
        return VS_BATTERY_DISCHARGING;
    }
    if (strcasecmp(text, "full") == 0) {
        return VS_BATTERY_FULL;
    }
    if (strcasecmp(text, "empty") == 0) {
        return VS_BATTERY_EMPTY;
    }
    /* "Not charging" is a plugged-in battery the firmware is holding below
     * 100 % on purpose (a charge threshold). It is neither charging nor
     * discharging; UPower calls that pending-charge. */
    if (strcasecmp(text, "not charging") == 0) {
        return VS_BATTERY_PENDING_CHARGE;
    }
    return VS_BATTERY_UNKNOWN;
}

vs_battery_kind vs_battery_kind_from_sysfs(const char *type, const char *name)
{
    if (type != NULL && strcasecmp(type, "ups") == 0) {
        return VS_BATTERY_KIND_UPS;
    }
    if (name != NULL) {
        if (vs_contains_ci(name, "mouse")) {
            return VS_BATTERY_KIND_MOUSE;
        }
        if (vs_contains_ci(name, "keyboard") || vs_contains_ci(name, "kbd")) {
            return VS_BATTERY_KIND_KEYBOARD;
        }
        if (vs_contains_ci(name, "headset") || vs_contains_ci(name, "headphone")) {
            return VS_BATTERY_KIND_HEADSET;
        }
        if (vs_contains_ci(name, "stylus") || vs_contains_ci(name, "pen")) {
            return VS_BATTERY_KIND_PEN;
        }
    }
    return VS_BATTERY_KIND_BATTERY;
}

/* Charge is published either in energy units (µWh, the ACPI laptops) or in
 * charge units (µAh, most phones and some ThinkPads); the second needs the
 * voltage to become watt-hours. */
static void fill_battery(const char *directory, const char *name, vs_battery_sample *out)
{
    memset(out, 0, sizeof *out);
    vs_copy(out->id, sizeof out->id, name);
    vs_copy(out->label, sizeof out->label, name);

    char text[128];
    if (supply_text(directory, "manufacturer", text, sizeof text)) {
        vs_copy(out->vendor, sizeof out->vendor, text);
    }
    if (supply_text(directory, "model_name", text, sizeof text)) {
        vs_copy(out->label, sizeof out->label, text);
    }
    if (supply_text(directory, "status", text, sizeof text)) {
        out->state = vs_battery_state_from_string(text);
    }
    char type[64];
    type[0] = '\0';
    (void)supply_text(directory, "type", type, sizeof type);
    out->kind = vs_battery_kind_from_sysfs(type, out->label);

    int64_t micro_volt = 0;
    if (supply_i64(directory, "voltage_now", &micro_volt) && micro_volt > 0) {
        out->voltage_v = (double)micro_volt / 1e6;
    }

    uint64_t energy_now = 0;
    uint64_t energy_full = 0;
    uint64_t energy_design = 0;
    bool have_energy = supply_u64(directory, "energy_now", &energy_now);
    bool have_full = supply_u64(directory, "energy_full", &energy_full);
    bool have_design = supply_u64(directory, "energy_full_design", &energy_design);
    if (!have_energy || !have_full) {
        uint64_t charge_now = 0;
        uint64_t charge_full = 0;
        uint64_t charge_design = 0;
        bool have_charge = supply_u64(directory, "charge_now", &charge_now);
        bool have_charge_full = supply_u64(directory, "charge_full", &charge_full);
        bool have_charge_design = supply_u64(directory, "charge_full_design", &charge_design);
        if (have_charge && out->voltage_v > 0) {
            energy_now = (uint64_t)((double)charge_now * out->voltage_v);
            have_energy = true;
        }
        if (have_charge_full && out->voltage_v > 0) {
            energy_full = (uint64_t)((double)charge_full * out->voltage_v);
            have_full = true;
        }
        if (have_charge_design && out->voltage_v > 0) {
            energy_design = (uint64_t)((double)charge_design * out->voltage_v);
            have_design = true;
        }
        /* Health needs no voltage when both sides are in the same unit. */
        if (!have_full && have_charge_full && have_charge_design && charge_design > 0) {
            out->health = (double)charge_full / (double)charge_design;
            out->has_health = true;
        }
    }
    if (have_energy) {
        out->energy_wh = (double)energy_now / 1e6;
    }
    if (have_full) {
        out->energy_full_wh = (double)energy_full / 1e6;
    }
    if (have_design) {
        out->energy_full_design_wh = (double)energy_design / 1e6;
    }
    if (!out->has_health && have_full && have_design && energy_design > 0) {
        out->health = (double)energy_full / (double)energy_design;
        out->has_health = true;
    }

    uint64_t capacity = 0;
    if (supply_u64(directory, "capacity", &capacity)) {
        out->percentage = (double)capacity;
        out->has_percentage = true;
    } else if (have_energy && have_full && energy_full > 0) {
        out->percentage = (double)energy_now * 100.0 / (double)energy_full;
        out->has_percentage = true;
    }

    double watts = 0;
    bool have_watts = false;
    int64_t micro_watt = 0;
    if (supply_i64(directory, "power_now", &micro_watt) && micro_watt != 0) {
        watts = (double)micro_watt / 1e6;
        have_watts = true;
    } else {
        int64_t micro_amp = 0;
        if (supply_i64(directory, "current_now", &micro_amp) && micro_amp != 0 &&
            out->voltage_v > 0) {
            watts = (double)micro_amp / 1e6 * out->voltage_v;
            have_watts = true;
        }
    }
    if (have_watts) {
        double magnitude = watts < 0 ? -watts : watts;
        out->watts = out->state == VS_BATTERY_DISCHARGING ? -magnitude : magnitude;
        out->has_watts = true;
    }

    uint64_t seconds = 0;
    if (supply_u64(directory, "time_to_empty_now", &seconds) && seconds > 0) {
        out->time_to_empty_seconds = (double)seconds;
        out->has_time_to_empty = true;
    } else if (out->state == VS_BATTERY_DISCHARGING && have_watts && out->energy_wh > 0) {
        double magnitude = out->watts < 0 ? -out->watts : out->watts;
        if (magnitude > 0) {
            out->time_to_empty_seconds = out->energy_wh / magnitude * 3600.0;
            out->has_time_to_empty = true;
        }
    }
    if (supply_u64(directory, "time_to_full_now", &seconds) && seconds > 0) {
        out->time_to_full_seconds = (double)seconds;
        out->has_time_to_full = true;
    } else if (out->state == VS_BATTERY_CHARGING && have_watts && out->energy_full_wh > 0 &&
               out->watts > 0) {
        double missing = out->energy_full_wh - out->energy_wh;
        if (missing > 0) {
            out->time_to_full_seconds = missing / out->watts * 3600.0;
            out->has_time_to_full = true;
        }
    }

    int64_t cycles = 0;
    if (supply_i64(directory, "cycle_count", &cycles) && cycles > 0) {
        out->cycle_count = (int32_t)cycles;
        out->has_cycle_count = true;
    }
    int64_t decidegrees = 0;
    if (supply_i64(directory, "temp", &decidegrees) && decidegrees != 0) {
        out->temperature_celsius = (double)decidegrees / 10.0;
        out->has_temperature = true;
    }
}

static void fill_adapter(const char *directory, vs_power_sample *out)
{
    uint64_t online = 0;
    if (!supply_u64(directory, "online", &online) || online == 0) {
        return;
    }
    out->external_connected = true;

    int64_t micro_watt = 0;
    if (supply_i64(directory, "power_now", &micro_watt) && micro_watt > 0) {
        out->adapter_watts = (double)micro_watt / 1e6;
        out->has_adapter_watts = true;
    } else {
        int64_t micro_volt = 0;
        int64_t micro_amp = 0;
        if (supply_i64(directory, "voltage_now", &micro_volt) && micro_volt > 0 &&
            supply_i64(directory, "current_now", &micro_amp) && micro_amp > 0) {
            out->adapter_watts = (double)micro_volt / 1e6 * (double)micro_amp / 1e6;
            out->has_adapter_watts = true;
        }
    }

    int64_t limit = 0;
    int64_t volt_max = 0;
    int64_t amp_max = 0;
    if (supply_i64(directory, "voltage_max_design", &volt_max) && volt_max > 0 &&
        supply_i64(directory, "current_max", &amp_max) && amp_max > 0) {
        out->adapter_max_watts = (double)volt_max / 1e6 * (double)amp_max / 1e6;
        out->has_adapter_max_watts = true;
    } else if (supply_i64(directory, "input_power_limit", &limit) && limit > 0) {
        out->adapter_max_watts = (double)limit / 1e6;
        out->has_adapter_max_watts = true;
    }
}

bool vs_power_supply_present(const vs_sensors_impl *impl)
{
    char base[VS_FULLPATH_MAX];
    if (!vs_path(impl, base, sizeof base, "/sys/class/power_supply")) {
        return false;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

/* `scope` is how the kernel distinguishes the machine's own battery ("System",
 * or absent) from a battery inside an attached device ("Device"): a wireless
 * mouse, a stylus, a game controller. */
static bool is_device_scope(const char *directory)
{
    char scope[32];
    return supply_text(directory, "scope", scope, sizeof scope) && strcasecmp(scope, "device") == 0;
}

int vs_power_read_sysfs(vs_sensors_impl *impl, vs_power_sample *out)
{
    if (impl == NULL || out == NULL) {
        return VS_ERR_INVALID;
    }
    char base[VS_FULLPATH_MAX];
    if (!vs_path(impl, base, sizeof base, "/sys/class/power_supply")) {
        return VS_ERR_BACKEND;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return VS_ERR_UNSUPPORTED;
    }

    memset(out, 0, sizeof *out);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char directory[VS_FULLPATH_MAX];
        int written = snprintf(directory, sizeof directory, "%s/%s", base, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof directory) {
            continue;
        }
        char type[64];
        if (!supply_text(directory, "type", type, sizeof type)) {
            continue;
        }
        if (strcasecmp(type, "battery") == 0) {
            if (is_device_scope(directory) || out->has_battery) {
                continue;
            }
            fill_battery(directory, entry->d_name, &out->battery);
            out->has_battery = true;
        } else if (strcasecmp(type, "mains") == 0 || strcasecmp(type, "usb") == 0 ||
                   strcasecmp(type, "usb_pd") == 0 || strcasecmp(type, "usb_pd_drp") == 0) {
            fill_adapter(directory, out);
        }
    }
    closedir(dir);

    if (out->has_battery && out->battery.has_watts && out->battery.state == VS_BATTERY_DISCHARGING) {
        out->system_watts = -out->battery.watts;
        out->has_system_watts = true;
    }
    return VS_OK;
}

int vs_power_peripherals_sysfs(vs_sensors_impl *impl, vs_battery_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    char base[VS_FULLPATH_MAX];
    if (!vs_path(impl, base, sizeof base, "/sys/class/power_supply")) {
        return VS_ERR_BACKEND;
    }
    DIR *dir = opendir(base);
    if (dir == NULL) {
        return VS_OK;
    }

    vs_battery_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int result = VS_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char directory[VS_FULLPATH_MAX];
        int written = snprintf(directory, sizeof directory, "%s/%s", base, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof directory) {
            continue;
        }
        char type[64];
        if (!supply_text(directory, "type", type, sizeof type) ||
            strcasecmp(type, "battery") != 0 || !is_device_scope(directory)) {
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
        fill_battery(directory, entry->d_name, &samples[count]);
        count++;
    }
    closedir(dir);

    if (result != VS_OK) {
        free(samples);
        return result;
    }
    *out = samples;
    *count_out = count;
    return VS_OK;
}
