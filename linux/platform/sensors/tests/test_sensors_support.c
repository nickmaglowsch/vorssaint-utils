/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * Unit tests for the sensors backend: the pure classification rules, and every
 * reader driven against the fixture trees in fixtures/ (see fixtures/generate.py
 * for what each tree stands for). No /proc of this machine is touched here, so
 * the expectations are exact; the live reads are in test_live_procfs.sh.
 */

#include "vorssaint_platform.h"
#include "vs_sensors_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void check(bool condition, const char *what, const char *file, int line)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

static void check_near(double actual, double expected, double tolerance, const char *what,
                       const char *file, int line)
{
    checks++;
    if (fabs(actual - expected) > tolerance) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s: expected %.6f, got %.6f\n", file, line, what, expected,
                actual);
    }
}

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
    check_near((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

static void check_equal_u64(uint64_t actual, uint64_t expected, const char *what, const char *file,
                            int line)
{
    checks++;
    if (actual != expected) {
        failures++;
        fprintf(stderr, "FAIL %s:%d: %s: expected %llu, got %llu\n", file, line, what,
                (unsigned long long)expected, (unsigned long long)actual);
    }
}

#define CHECK_U64(actual, expected)                                                                \
    check_equal_u64((actual), (expected), #actual, __FILE__, __LINE__)

static vs_sensors_system *open_fixture(const char *name)
{
    static char root[512];
    snprintf(root, sizeof root, "%s/%s", VS_FIXTURE_DIR, name);
    vs_sensors_options options;
    memset(&options, 0, sizeof options);
    options.root = root;
    options.disable_nvml = true;
    int result = VS_OK;
    vs_sensors_system *sensors = vs_sensors_system_create(&options, &result);
    if (sensors == NULL) {
        fprintf(stderr, "FATAL: fixture %s did not open: %d\n", name, result);
        exit(1);
    }
    return sensors;
}

/* ------------------------------------------------- classification rules */

static void test_classification(void)
{
    /* One row per driver the work package names, plus the label-driven chips
     * that only the label can resolve. */
    CHECK(vs_sensor_classify("coretemp", "Package id 0") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("coretemp", "Core 3") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("k10temp", "Tctl") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("k10temp", "Tccd1") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("zenpower", "Tdie") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("nvme", "Composite") == VS_SENSOR_DRIVE);
    CHECK(vs_sensor_classify("nvme", "Sensor 1") == VS_SENSOR_DRIVE);
    CHECK(vs_sensor_classify("drivetemp", "") == VS_SENSOR_DRIVE);
    CHECK(vs_sensor_classify("acpitz", "") == VS_SENSOR_SYSTEM);
    CHECK(vs_sensor_classify("amdgpu", "edge") == VS_SENSOR_GPU);
    CHECK(vs_sensor_classify("amdgpu", "junction") == VS_SENSOR_GPU);
    CHECK(vs_sensor_classify("nouveau", "") == VS_SENSOR_GPU);
    CHECK(vs_sensor_classify("i915", "") == VS_SENSOR_GPU);

    /* A platform chip's name says nothing; its labels say everything. */
    CHECK(vs_sensor_classify("thinkpad", "CPU") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("thinkpad", "GPU") == VS_SENSOR_GPU);
    CHECK(vs_sensor_classify("thinkpad", "Bat0") == VS_SENSOR_BATTERY);
    CHECK(vs_sensor_classify("thinkpad", "Ambient") == VS_SENSOR_SYSTEM);
    CHECK(vs_sensor_classify("dell_smm", "CPU") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("dell_smm", "Other") == VS_SENSOR_SYSTEM);
    CHECK(vs_sensor_classify("asus", "CPU Temperature") == VS_SENSOR_CPU);
    CHECK(vs_sensor_classify("asus", "GPU Temperature") == VS_SENSOR_GPU);

    /* Battery packs, and an unknown driver falling through to its label. */
    CHECK(vs_sensor_classify("BAT0", "") == VS_SENSOR_BATTERY);
    CHECK(vs_sensor_classify("bq27441-0", "") == VS_SENSOR_BATTERY);
    CHECK(vs_sensor_classify("iwlwifi_1", "") == VS_SENSOR_OTHER);
    CHECK(vs_sensor_classify("something", "GPU core") == VS_SENSOR_GPU);
    CHECK(vs_sensor_classify(NULL, NULL) == VS_SENSOR_OTHER);

    CHECK(vs_sensor_label_is_package("Package id 0"));
    CHECK(vs_sensor_label_is_package("Tctl"));
    CHECK(vs_sensor_label_is_package("Tdie"));
    CHECK(vs_sensor_label_is_package("Composite"));
    CHECK(vs_sensor_label_is_package("edge"));
    CHECK(!vs_sensor_label_is_package("Core 0"));
    CHECK(!vs_sensor_label_is_package("Tccd1"));

    /* TemperatureSensorSelector's window, unchanged. */
    CHECK(!vs_sensor_temperature_plausible(9.9));
    CHECK(vs_sensor_temperature_plausible(10.0));
    CHECK(vs_sensor_temperature_plausible(124.9));
    CHECK(!vs_sensor_temperature_plausible(125.0));

    CHECK(vs_thermal_from_ratio(0.0) == VS_THERMAL_NOMINAL);
    CHECK(vs_thermal_from_ratio(0.749) == VS_THERMAL_NOMINAL);
    CHECK(vs_thermal_from_ratio(0.75) == VS_THERMAL_FAIR);
    CHECK(vs_thermal_from_ratio(0.85) == VS_THERMAL_SERIOUS);
    CHECK(vs_thermal_from_ratio(0.95) == VS_THERMAL_CRITICAL);
    CHECK(vs_thermal_from_ratio(1.2) == VS_THERMAL_CRITICAL);
}

/* ---------------------------------------------------------------- helpers */

static void test_rate_helper(void)
{
    /* MetricFormat.netSpeed's rule: a non-increasing counter is 0, never a
     * spike, and a zero interval is 0. */
    CHECK_NEAR(vs_rate(100, 200, 2.0), 50.0, 1e-9);
    CHECK_NEAR(vs_rate(200, 100, 2.0), 0.0, 1e-9);
    CHECK_NEAR(vs_rate(100, 200, 0.0), 0.0, 1e-9);
    CHECK_NEAR(vs_rate(100, 100, 1.0), 0.0, 1e-9);
}

/* -------------------------------------------------------- fixture: amd */

static void test_amd_fixture(void)
{
    vs_sensors_system *sensors = open_fixture("amd-desktop");

    CHECK((sensors->capabilities & VS_SENSORS_HAS_HWMON) != 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_PSI) != 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_AMDGPU) != 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_INTEL_GPU) == 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_NVML) == 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_UPOWER) == 0);
    /* The tree has /sys/class/power_supply but nothing in it: a desktop. */
    CHECK((sensors->capabilities & VS_SENSORS_HAS_POWER_SUPPLY) == 0);
    CHECK(strcmp(sensors->power_backend_name, "none") == 0);

    vs_cpu_sample cpu;
    CHECK(sensors->cpu(sensors, &cpu) == VS_OK);
    CHECK(cpu.core_count == 8);
    CHECK(cpu.physical_core_count == 4);
    CHECK(cpu.package_count == 1);
    CHECK(!cpu.has_rates); /* first sample of an instance, as macOS returns nil */
    CHECK_NEAR(cpu.load_average[0], 1.42, 1e-6);
    CHECK_NEAR(cpu.load_average[2], 0.61, 1e-6);
    CHECK_NEAR(cpu.cores[0].frequency_mhz, 3800.0, 1e-6);
    CHECK(cpu.cores[1].core_id == 0 && cpu.cores[2].core_id == 1);
    /* A second read against an unchanged /proc/stat has an interval but no
     * tick movement, so there is still nothing to divide by. */
    CHECK(sensors->cpu(sensors, &cpu) == VS_OK);
    CHECK(!cpu.has_rates);

    vs_memory_sample memory;
    CHECK(sensors->memory(sensors, &memory) == VS_OK);
    CHECK_U64(memory.total_bytes, 32768000ull * 1024);
    CHECK_U64(memory.available_bytes, 24000000ull * 1024);
    CHECK_U64(memory.used_bytes, (32768000ull - 24000000ull) * 1024);
    CHECK_U64(memory.app_bytes, 7100000ull * 1024);
    /* Buffers + Cached + SReclaimable - Shmem */
    CHECK_U64(memory.cached_bytes, (260000ull + 6200000ull + 700000ull - 340000ull) * 1024);
    CHECK_U64(memory.swap_total_bytes, 8388604ull * 1024);
    CHECK_U64(memory.swap_used_bytes, 0);
    CHECK(memory.has_compressed); /* the tree has a Zswapped line */
    CHECK(memory.pressure_is_psi);
    CHECK_NEAR(memory.pressure, 0.0042, 1e-9);
    CHECK_NEAR(memory.pressure_full, 0.0011, 1e-9);

    vs_network_sample *interfaces = NULL;
    size_t interface_count = 0;
    CHECK(sensors->network(sensors, &interfaces, &interface_count) == VS_OK);
    CHECK(interface_count == 3);
    bool saw_default = false;
    for (size_t i = 0; i < interface_count; i++) {
        if (strcmp(interfaces[i].name, "enp5s0") == 0) {
            CHECK(interfaces[i].kind == VS_NETWORK_ETHERNET);
            CHECK(interfaces[i].is_default_route);
            CHECK(interfaces[i].is_up);
            CHECK_U64(interfaces[i].rx_bytes, 91827364);
            CHECK_U64(interfaces[i].tx_bytes, 12345678);
            CHECK(!interfaces[i].has_rates);
            saw_default = true;
        } else if (strcmp(interfaces[i].name, "lo") == 0) {
            CHECK(interfaces[i].kind == VS_NETWORK_LOOPBACK);
            CHECK(!interfaces[i].is_default_route);
        } else if (strcmp(interfaces[i].name, "virbr0") == 0) {
            CHECK(interfaces[i].kind == VS_NETWORK_VIRTUAL);
        }
    }
    CHECK(saw_default);
    sensors->free_network(sensors, interfaces, interface_count);

    vs_disk_device_sample *devices = NULL;
    size_t device_count = 0;
    CHECK(sensors->disk_devices(sensors, &devices, &device_count) == VS_OK);
    CHECK(device_count == 5);
    for (size_t i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, "nvme0n1") == 0) {
            CHECK(!devices[i].is_partition);
            CHECK_U64(devices[i].read_bytes, 61254144ull * 512);
            CHECK_U64(devices[i].write_bytes, 40551232ull * 512);
            CHECK_U64(devices[i].read_ios, 1204513);
            CHECK_U64(devices[i].io_ticks, 921444);
        } else if (strcmp(devices[i].name, "nvme0n1p2") == 0) {
            CHECK(devices[i].is_partition);
        }
    }
    sensors->free_disk_devices(sensors, devices, device_count);

    vs_disk_mount_sample *mounts = NULL;
    size_t mount_count = 0;
    CHECK(sensors->disk_mounts(sensors, &mounts, &mount_count) == VS_OK);
    /* proc, sysfs, tmpfs and cgroup2 are pseudo filesystems and are skipped. */
    CHECK(mount_count == 3);
    for (size_t i = 0; i < mount_count; i++) {
        CHECK(strcmp(mounts[i].filesystem, "proc") != 0);
        CHECK(strcmp(mounts[i].filesystem, "tmpfs") != 0);
        if (strcmp(mounts[i].mount_point, "/mnt/data") == 0) {
            CHECK(mounts[i].is_read_only);
            CHECK(strcmp(mounts[i].device, "sda") == 0);
        }
        if (strcmp(mounts[i].mount_point, "/") == 0) {
            CHECK(!mounts[i].is_read_only);
            CHECK(strcmp(mounts[i].device, "nvme0n1p2") == 0);
            /* statvfs cannot be rooted, so it measured the filesystem the
             * fixture lives on. That it answered at all is the assertion. */
            CHECK(mounts[i].total_bytes > 0);
        }
    }
    sensors->free_disk_mounts(sensors, mounts, mount_count);

    vs_temperature_sample *temperatures = NULL;
    size_t temperature_count = 0;
    CHECK(sensors->temperatures(sensors, &temperatures, &temperature_count) == VS_OK);
    CHECK(temperature_count == 7);
    int cpu_primaries = 0;
    int gpu_primaries = 0;
    int drive_primaries = 0;
    for (size_t i = 0; i < temperature_count; i++) {
        if (!temperatures[i].is_primary) {
            continue;
        }
        if (temperatures[i].kind == VS_SENSOR_CPU) {
            cpu_primaries++;
            CHECK(strcmp(temperatures[i].label, "Tctl") == 0);
            CHECK_NEAR(temperatures[i].celsius, 48.375, 1e-6);
            CHECK_NEAR(temperatures[i].critical_celsius, 100.0, 1e-6);
        } else if (temperatures[i].kind == VS_SENSOR_GPU) {
            gpu_primaries++;
            CHECK(strcmp(temperatures[i].label, "edge") == 0);
        } else if (temperatures[i].kind == VS_SENSOR_DRIVE) {
            drive_primaries++;
            CHECK(strcmp(temperatures[i].label, "Composite") == 0);
        }
    }
    CHECK(cpu_primaries == 1);
    CHECK(gpu_primaries == 1);
    CHECK(drive_primaries == 1);
    sensors->free_temperatures(sensors, temperatures, temperature_count);

    vs_thermal_pressure pressure = VS_THERMAL_CRITICAL;
    CHECK(sensors->thermal_pressure(sensors, &pressure) == VS_OK);
    /* Hottest CPU/GPU ratio here is amdgpu junction 61/110 = 0.554. */
    CHECK(pressure == VS_THERMAL_NOMINAL);

    vs_fan_sample *fans = NULL;
    size_t fan_count = 0;
    CHECK(sensors->fans(sensors, &fans, &fan_count) == VS_OK);
    CHECK(fan_count == 1);
    if (fan_count == 1) {
        CHECK(fans[0].rpm == 1180);
        CHECK(fans[0].min_rpm == 0);
        CHECK(fans[0].max_rpm == 3200);
        CHECK(fans[0].is_controllable);
        CHECK(fans[0].pwm == 94);
        CHECK(strstr(fans[0].pwm_path, "/pwm1") != NULL);
    }
    sensors->free_fans(sensors, fans, fan_count);

    vs_gpu_sample *gpus = NULL;
    size_t gpu_count = 0;
    CHECK(sensors->gpus(sensors, &gpus, &gpu_count) == VS_OK);
    CHECK(gpu_count == 1);
    if (gpu_count == 1) {
        CHECK(gpus[0].vendor == VS_GPU_VENDOR_AMD);
        CHECK(strcmp(gpus[0].driver, "amdgpu") == 0);
        CHECK(gpus[0].has_busy);
        CHECK_NEAR(gpus[0].busy, 0.37, 1e-9);
        CHECK(gpus[0].has_vram);
        CHECK_U64(gpus[0].vram_used_bytes, 2415919104ull);
        CHECK_U64(gpus[0].vram_total_bytes, 17163091968ull);
        CHECK(gpus[0].has_temperature);
        CHECK_NEAR(gpus[0].temperature_celsius, 52.0, 1e-6);
        CHECK(gpus[0].has_watts);
        CHECK_NEAR(gpus[0].watts, 43.0, 1e-6);
        CHECK(gpus[0].has_clock);
        CHECK_NEAR(gpus[0].clock_mhz, 2405.0, 1e-6);
    }
    sensors->free_gpus(sensors, gpus, gpu_count);

    vs_power_sample power;
    CHECK(sensors->power(sensors, &power) == VS_ERR_UNSUPPORTED);

    sensors->destroy(sensors);
}

/* ------------------------------------------------------ fixture: intel */

static void test_intel_fixture(void)
{
    vs_sensors_system *sensors = open_fixture("intel-laptop");

    CHECK((sensors->capabilities & VS_SENSORS_HAS_PSI) == 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_INTEL_GPU) != 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_AMDGPU) == 0);
    CHECK((sensors->capabilities & VS_SENSORS_HAS_POWER_SUPPLY) != 0);
    CHECK(strcmp(sensors->power_backend_name, "power_supply") == 0);

    vs_memory_sample memory;
    CHECK(sensors->memory(sensors, &memory) == VS_OK);
    CHECK(!memory.has_compressed);
    CHECK(!memory.pressure_is_psi);
    /* Without PSI the level is the MemAvailable shortfall. */
    CHECK_NEAR(memory.pressure, 1.0 - 9100000.0 / 16384000.0, 1e-9);
    CHECK_U64(memory.swap_used_bytes, (4194300ull - 4090000ull) * 1024);

    vs_network_sample *interfaces = NULL;
    size_t interface_count = 0;
    CHECK(sensors->network(sensors, &interfaces, &interface_count) == VS_OK);
    for (size_t i = 0; i < interface_count; i++) {
        if (strcmp(interfaces[i].name, "wlan0") == 0) {
            CHECK(interfaces[i].kind == VS_NETWORK_WIFI);
            CHECK(interfaces[i].is_default_route);
        } else if (strcmp(interfaces[i].name, "docker0") == 0) {
            CHECK(interfaces[i].kind == VS_NETWORK_VIRTUAL);
            CHECK(!interfaces[i].is_up);
        }
    }
    sensors->free_network(sensors, interfaces, interface_count);

    vs_gpu_sample *gpus = NULL;
    size_t gpu_count = 0;
    CHECK(sensors->gpus(sensors, &gpus, &gpu_count) == VS_OK);
    CHECK(gpu_count == 1);
    if (gpu_count == 1) {
        CHECK(gpus[0].vendor == VS_GPU_VENDOR_INTEL);
        CHECK(strcmp(gpus[0].driver, "i915") == 0);
        /* The honest part: Intel publishes no busy percentage in sysfs. */
        CHECK(!gpus[0].has_busy);
        CHECK(!gpus[0].has_vram);
        CHECK(gpus[0].has_clock);
        CHECK_NEAR(gpus[0].clock_mhz, 950.0, 1e-6);
    }
    sensors->free_gpus(sensors, gpus, gpu_count);

    vs_power_sample power;
    CHECK(sensors->power(sensors, &power) == VS_OK);
    CHECK(power.has_battery);
    CHECK(!power.external_connected);
    CHECK(power.battery.state == VS_BATTERY_DISCHARGING);
    CHECK(power.battery.has_percentage);
    CHECK_NEAR(power.battery.percentage, 62.0, 1e-6);
    CHECK(power.battery.has_watts);
    CHECK_NEAR(power.battery.watts, -8.94, 1e-6); /* negative while discharging */
    CHECK(power.has_system_watts);
    CHECK_NEAR(power.system_watts, 8.94, 1e-6);
    CHECK(power.battery.has_health);
    CHECK_NEAR(power.battery.health, 50720000.0 / 57000000.0, 1e-9);
    CHECK(power.battery.has_cycle_count);
    CHECK(power.battery.cycle_count == 214);
    CHECK(power.battery.has_time_to_empty);
    CHECK_NEAR(power.battery.time_to_empty_seconds, 31.46 / 8.94 * 3600.0, 1.0);
    CHECK(strcmp(power.battery.label, "5B10W51") == 0);
    CHECK(strcmp(power.battery.vendor, "SMP") == 0);

    vs_temperature_sample *temperatures = NULL;
    size_t temperature_count = 0;
    CHECK(sensors->temperatures(sensors, &temperatures, &temperature_count) == VS_OK);
    CHECK(temperature_count == 5);
    for (size_t i = 0; i < temperature_count; i++) {
        if (strcmp(temperatures[i].driver, "acpitz") == 0) {
            CHECK(temperatures[i].kind == VS_SENSOR_SYSTEM);
        } else if (strcmp(temperatures[i].driver, "BAT0") == 0) {
            CHECK(temperatures[i].kind == VS_SENSOR_BATTERY);
            CHECK_NEAR(temperatures[i].celsius, 31.2, 1e-6);
        } else if (temperatures[i].is_primary && temperatures[i].kind == VS_SENSOR_CPU) {
            CHECK(strcmp(temperatures[i].label, "Package id 0") == 0);
            CHECK_NEAR(temperatures[i].celsius, 56.0, 1e-6);
        }
    }
    sensors->free_temperatures(sensors, temperatures, temperature_count);

    /* No fan node on this machine: an empty list, not an error. */
    vs_fan_sample *fans = NULL;
    size_t fan_count = 0;
    CHECK(sensors->fans(sensors, &fans, &fan_count) == VS_OK);
    CHECK(fan_count == 0);
    sensors->free_fans(sensors, fans, fan_count);

    sensors->destroy(sensors);
}

/* --------------------------------------------------- fixture: thinkpad */

static void test_thinkpad_fixture(void)
{
    vs_sensors_system *sensors = open_fixture("thinkpad");

    vs_cpu_sample cpu;
    CHECK(sensors->cpu(sensors, &cpu) == VS_OK);
    CHECK(cpu.core_count == 2);
    /* No cpufreq tree here, so the frequency comes from /proc/cpuinfo. */
    CHECK_NEAR(cpu.cores[0].frequency_mhz, 800.0, 1e-6);
    CHECK_NEAR(cpu.cores[1].frequency_mhz, 801.0, 1e-6);

    vs_temperature_sample *temperatures = NULL;
    size_t temperature_count = 0;
    CHECK(sensors->temperatures(sensors, &temperatures, &temperature_count) == VS_OK);
    /* thinkpad 4 + dell_smm 1 + asus 1 + zenpower 2 + nouveau 1 */
    CHECK(temperature_count == 9);
    int cpu_count = 0;
    int gpu_count = 0;
    int battery_count = 0;
    int system_count = 0;
    for (size_t i = 0; i < temperature_count; i++) {
        switch (temperatures[i].kind) {
        case VS_SENSOR_CPU:
            cpu_count++;
            break;
        case VS_SENSOR_GPU:
            gpu_count++;
            break;
        case VS_SENSOR_BATTERY:
            battery_count++;
            break;
        case VS_SENSOR_SYSTEM:
            system_count++;
            break;
        default:
            break;
        }
    }
    /* thinkpad CPU, dell_smm CPU, asus CPU, zenpower Tdie + Tctl */
    CHECK(cpu_count == 5);
    /* thinkpad GPU, nouveau */
    CHECK(gpu_count == 2);
    CHECK(battery_count == 1);
    CHECK(system_count == 1);
    sensors->free_temperatures(sensors, temperatures, temperature_count);

    vs_fan_sample *fans = NULL;
    size_t fan_count = 0;
    CHECK(sensors->fans(sensors, &fans, &fan_count) == VS_OK);
    CHECK(fan_count == 5);
    for (size_t i = 0; i < fan_count; i++) {
        if (strcmp(fans[i].id, "thinkpad/fan1_input") == 0) {
            CHECK(fans[i].rpm == 2914);
            CHECK(fans[i].is_controllable);
            CHECK(fans[i].pwm == 128);
        } else if (strcmp(fans[i].id, "thinkpad/fan2_input") == 0) {
            /* No pwm2 next to it: readable, not drivable. */
            CHECK(!fans[i].is_controllable);
            CHECK(fans[i].pwm == -1);
        }
    }
    sensors->free_fans(sensors, fans, fan_count);

    vs_power_sample power;
    CHECK(sensors->power(sensors, &power) == VS_OK);
    CHECK(power.has_battery);
    CHECK(power.external_connected);
    CHECK(power.battery.state == VS_BATTERY_CHARGING);
    /* charge_* units: energy = charge * voltage, and the percentage has to be
     * derived because this battery publishes no `capacity`. */
    CHECK(power.battery.has_percentage);
    CHECK_NEAR(power.battery.percentage, 3120000.0 / 4210000.0 * 100.0, 0.01);
    CHECK(power.battery.has_watts);
    CHECK_NEAR(power.battery.watts, 1.9 * 12.48, 1e-6); /* positive while charging */
    CHECK(power.battery.has_health);
    CHECK_NEAR(power.battery.health, 4210000.0 / 4800000.0, 1e-6);
    CHECK(power.battery.has_temperature);
    CHECK_NEAR(power.battery.temperature_celsius, 31.2, 1e-6);
    CHECK(power.battery.has_cycle_count && power.battery.cycle_count == 87);
    /* A USB-PD charger: instantaneous draw and the charger's rating. */
    CHECK(power.has_adapter_watts);
    CHECK_NEAR(power.adapter_watts, 20.0 * 2.25, 1e-6);
    CHECK(power.has_adapter_max_watts);
    CHECK_NEAR(power.adapter_max_watts, 20.0 * 3.25, 1e-6);
    /* Plugged in, so nothing measures whole-machine draw. */
    CHECK(!power.has_system_watts);

    vs_battery_sample *peripherals = NULL;
    size_t peripheral_count = 0;
    CHECK(sensors->peripheral_batteries(sensors, &peripherals, &peripheral_count) == VS_OK);
    CHECK(peripheral_count == 1);
    if (peripheral_count == 1) {
        CHECK(peripherals[0].kind == VS_BATTERY_KIND_MOUSE);
        CHECK_NEAR(peripherals[0].percentage, 55.0, 1e-6);
        CHECK(strcmp(peripherals[0].label, "MX Master 3 Mouse") == 0);
    }
    sensors->free_batteries(sensors, peripherals, peripheral_count);

    sensors->destroy(sensors);
}

/* ------------------------------------------------------------ lifecycle */

static int event_count;
static vs_thermal_pressure last_event_pressure;

static void on_event(const vs_sensors_event *event, void *user_data)
{
    (void)user_data;
    if (event->type == VS_SENSORS_EVENT_THERMAL_PRESSURE) {
        event_count++;
        last_event_pressure = event->thermal_pressure;
    }
}

static void test_events_and_lifecycle(void)
{
    vs_sensors_system *sensors = open_fixture("amd-desktop");
    CHECK(sensors->event_fd(sensors) == -1); /* this backend polls */
    CHECK(sensors->set_event_callback(sensors, on_event, NULL) == VS_OK);

    /* Nothing has been read yet, so the pressure is still its initial value
     * and dispatch has nothing to announce. */
    CHECK(sensors->dispatch(sensors) == 0);
    CHECK(event_count == 0);

    vs_temperature_sample *temperatures = NULL;
    size_t count = 0;
    CHECK(sensors->temperatures(sensors, &temperatures, &count) == VS_OK);
    sensors->free_temperatures(sensors, temperatures, count);

    /* The fixture is nominal, which is also the initial value: still nothing
     * to announce, and that is the point of comparing rather than emitting. */
    CHECK(sensors->dispatch(sensors) == 0);
    CHECK(event_count == 0);
    CHECK(last_event_pressure == VS_THERMAL_NOMINAL);

    CHECK(sensors->set_event_callback(sensors, NULL, NULL) == VS_OK);
    sensors->destroy(sensors);
}

static void test_bad_root(void)
{
    vs_sensors_options options;
    memset(&options, 0, sizeof options);
    options.root = "/nonexistent-fixture-root";
    options.disable_nvml = true;
    int result = VS_OK;
    vs_sensors_system *sensors = vs_sensors_system_create(&options, &result);
    CHECK(sensors == NULL);
    CHECK(result == VS_ERR_NO_BACKEND);
}

int main(void)
{
    test_classification();
    test_rate_helper();
    test_amd_fixture();
    test_intel_fixture();
    test_thinkpad_fixture();
    test_events_and_lifecycle();
    test_bad_root();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
