/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `vs-sensors`: the harness for the sensors backend, the way `vs-window` is the
 * harness for the window backend. Everything the Swift side will read can be
 * printed here, against the live machine or against a fixture tree, which is
 * what the ctest suites drive.
 *
 * Output is one `key=value` per line, grouped by a `[section]` header: stable
 * enough to assert on in a shell test and readable enough to paste into a bug
 * report.
 */

#include "vorssaint_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void)
{
    fprintf(stderr,
            "usage: vs-sensors [options] <command>\n"
            "\n"
            "commands:\n"
            "  caps     capability flags and the chosen power backend\n"
            "  cpu      aggregate and per-core usage, load, topology, frequency\n"
            "  memory   /proc/meminfo mapped onto the panel's rows, plus pressure\n"
            "  net      per-interface counters, rates, type, default route\n"
            "  disk     per-device throughput and per-mount capacity\n"
            "  power    battery, adapter and peripheral batteries\n"
            "  gpu      per-card telemetry and what each vendor can answer\n"
            "  temps    temperatures, their classification, and fans\n"
            "  procs    top processes by cpu (default) or memory\n"
            "  watch    sample repeatedly and dispatch events\n"
            "\n"
            "options:\n"
            "  --root PATH      read /proc and /sys under PATH (a fixture tree)\n"
            "  --power BACKEND  force \"upower\" or \"sysfs\"\n"
            "  --no-nvml        do not load libnvidia-ml.so.1\n"
            "  --interval SECS  sampling interval for rate-bearing commands (default 1.0)\n"
            "  --count N        samples to take (default 2, so rates exist)\n"
            "  --limit N        rows for procs (default 5)\n"
            "  --sort KEY       procs sort: \"cpu\" or \"memory\"\n"
            "  --no-group       do not consolidate processes by app\n");
}

static void sleep_seconds(double seconds)
{
    if (seconds <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static void print_caps(vs_sensors_system *sensors)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } flags[] = {
        {VS_SENSORS_HAS_HWMON, "has_hwmon"},
        {VS_SENSORS_HAS_UPOWER, "has_upower"},
        {VS_SENSORS_HAS_POWER_SUPPLY, "has_power_supply"},
        {VS_SENSORS_HAS_AMDGPU, "has_amdgpu"},
        {VS_SENSORS_HAS_NVML, "has_nvml"},
        {VS_SENSORS_HAS_INTEL_GPU, "has_intel_gpu"},
        {VS_SENSORS_HAS_PSI, "has_psi"},
        {VS_SENSORS_HAS_PROCFS_IO, "has_procfs_io"},
    };
    printf("[capabilities]\n");
    printf("backend=%s\n", sensors->name);
    printf("power_backend=%s\n", sensors->power_backend_name);
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        printf("%s=%d\n", flags[i].name, (sensors->capabilities & flags[i].bit) != 0 ? 1 : 0);
    }
}

static int print_cpu(vs_sensors_system *sensors)
{
    vs_cpu_sample sample;
    int status = sensors->cpu(sensors, &sample);
    if (status != VS_OK) {
        fprintf(stderr, "cpu: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[cpu]\n");
    printf("has_rates=%d\n", sample.has_rates ? 1 : 0);
    printf("interval_seconds=%.3f\n", sample.interval_seconds);
    printf("total_usage=%.4f\n", sample.total_usage);
    printf("user_usage=%.4f\n", sample.user_usage);
    printf("system_usage=%.4f\n", sample.system_usage);
    printf("idle_usage=%.4f\n", sample.idle_usage);
    printf("iowait_usage=%.4f\n", sample.iowait_usage);
    printf("load1=%.2f\nload5=%.2f\nload15=%.2f\n", sample.load_average[0], sample.load_average[1],
           sample.load_average[2]);
    printf("core_count=%zu\n", sample.core_count);
    printf("physical_core_count=%zu\n", sample.physical_core_count);
    printf("package_count=%zu\n", sample.package_count);
    for (size_t i = 0; i < sample.core_count; i++) {
        printf("core%d usage=%.4f mhz=%.1f package=%d core_id=%d\n", sample.cores[i].index,
               sample.cores[i].usage, sample.cores[i].frequency_mhz, sample.cores[i].package_id,
               sample.cores[i].core_id);
    }
    return 0;
}

static int print_memory(vs_sensors_system *sensors)
{
    vs_memory_sample sample;
    int status = sensors->memory(sensors, &sample);
    if (status != VS_OK) {
        fprintf(stderr, "memory: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[memory]\n");
    printf("total_bytes=%llu\n", (unsigned long long)sample.total_bytes);
    printf("used_bytes=%llu\n", (unsigned long long)sample.used_bytes);
    printf("app_bytes=%llu\n", (unsigned long long)sample.app_bytes);
    printf("cached_bytes=%llu\n", (unsigned long long)sample.cached_bytes);
    printf("available_bytes=%llu\n", (unsigned long long)sample.available_bytes);
    printf("has_compressed=%d\n", sample.has_compressed ? 1 : 0);
    printf("compressed_bytes=%llu\n", (unsigned long long)sample.compressed_bytes);
    printf("swap_total_bytes=%llu\n", (unsigned long long)sample.swap_total_bytes);
    printf("swap_used_bytes=%llu\n", (unsigned long long)sample.swap_used_bytes);
    printf("pressure=%.4f\n", sample.pressure);
    printf("pressure_full=%.4f\n", sample.pressure_full);
    printf("pressure_is_psi=%d\n", sample.pressure_is_psi ? 1 : 0);
    return 0;
}

static int print_network(vs_sensors_system *sensors)
{
    vs_network_sample *samples = NULL;
    size_t count = 0;
    int status = sensors->network(sensors, &samples, &count);
    if (status != VS_OK) {
        fprintf(stderr, "net: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[network]\n");
    printf("interface_count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%s kind=%s up=%d default=%d rx=%llu tx=%llu rx_bps=%.1f tx_bps=%.1f "
               "rx_packets=%llu tx_packets=%llu rx_errors=%llu tx_errors=%llu rates=%d\n",
               samples[i].name, vs_network_kind_string(samples[i].kind), samples[i].is_up ? 1 : 0,
               samples[i].is_default_route ? 1 : 0, (unsigned long long)samples[i].rx_bytes,
               (unsigned long long)samples[i].tx_bytes, samples[i].rx_bytes_per_second,
               samples[i].tx_bytes_per_second, (unsigned long long)samples[i].rx_packets,
               (unsigned long long)samples[i].tx_packets, (unsigned long long)samples[i].rx_errors,
               (unsigned long long)samples[i].tx_errors, samples[i].has_rates ? 1 : 0);
    }
    sensors->free_network(sensors, samples, count);
    return 0;
}

static int print_disk(vs_sensors_system *sensors)
{
    vs_disk_device_sample *devices = NULL;
    size_t device_count = 0;
    int status = sensors->disk_devices(sensors, &devices, &device_count);
    if (status != VS_OK) {
        fprintf(stderr, "disk devices: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[disk-devices]\n");
    printf("device_count=%zu\n", device_count);
    for (size_t i = 0; i < device_count; i++) {
        printf("%s partition=%d read=%llu write=%llu read_bps=%.1f write_bps=%.1f io_ticks=%llu "
               "rates=%d\n",
               devices[i].name, devices[i].is_partition ? 1 : 0,
               (unsigned long long)devices[i].read_bytes,
               (unsigned long long)devices[i].write_bytes, devices[i].read_bytes_per_second,
               devices[i].write_bytes_per_second, (unsigned long long)devices[i].io_ticks,
               devices[i].has_rates ? 1 : 0);
    }
    sensors->free_disk_devices(sensors, devices, device_count);

    vs_disk_mount_sample *mounts = NULL;
    size_t mount_count = 0;
    status = sensors->disk_mounts(sensors, &mounts, &mount_count);
    if (status != VS_OK) {
        fprintf(stderr, "disk mounts: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[disk-mounts]\n");
    printf("mount_count=%zu\n", mount_count);
    for (size_t i = 0; i < mount_count; i++) {
        printf("%s source=%s fs=%s device=%s ro=%d total=%llu free=%llu\n", mounts[i].mount_point,
               mounts[i].source, mounts[i].filesystem, mounts[i].device[0] != '\0' ? mounts[i].device : "-",
               mounts[i].is_read_only ? 1 : 0, (unsigned long long)mounts[i].total_bytes,
               (unsigned long long)mounts[i].free_bytes);
    }
    sensors->free_disk_mounts(sensors, mounts, mount_count);
    printf("smart=out-of-scope\n");
    return 0;
}

static void print_battery(const char *prefix, const vs_battery_sample *battery)
{
    printf("%s.id=%s\n", prefix, battery->id);
    printf("%s.label=%s\n", prefix, battery->label);
    printf("%s.vendor=%s\n", prefix, battery->vendor);
    printf("%s.kind=%s\n", prefix, vs_battery_kind_string(battery->kind));
    printf("%s.state=%s\n", prefix, vs_battery_state_string(battery->state));
    if (battery->has_percentage) {
        printf("%s.percentage=%.1f\n", prefix, battery->percentage);
    }
    if (battery->has_watts) {
        printf("%s.watts=%.3f\n", prefix, battery->watts);
    }
    if (battery->has_time_to_empty) {
        printf("%s.time_to_empty_seconds=%.0f\n", prefix, battery->time_to_empty_seconds);
    }
    if (battery->has_time_to_full) {
        printf("%s.time_to_full_seconds=%.0f\n", prefix, battery->time_to_full_seconds);
    }
    if (battery->has_health) {
        printf("%s.health=%.4f\n", prefix, battery->health);
    }
    if (battery->has_cycle_count) {
        printf("%s.cycle_count=%d\n", prefix, battery->cycle_count);
    }
    if (battery->has_temperature) {
        printf("%s.temperature_celsius=%.1f\n", prefix, battery->temperature_celsius);
    }
    printf("%s.energy_wh=%.3f\n", prefix, battery->energy_wh);
    printf("%s.energy_full_wh=%.3f\n", prefix, battery->energy_full_wh);
    printf("%s.energy_full_design_wh=%.3f\n", prefix, battery->energy_full_design_wh);
    printf("%s.voltage_v=%.3f\n", prefix, battery->voltage_v);
}

static int print_power(vs_sensors_system *sensors)
{
    printf("[power]\n");
    printf("power_backend=%s\n", sensors->power_backend_name);
    vs_power_sample sample;
    int status = sensors->power(sensors, &sample);
    if (status != VS_OK) {
        printf("power=%s\n", vs_result_string(status));
    } else {
        printf("has_battery=%d\n", sample.has_battery ? 1 : 0);
        printf("external_connected=%d\n", sample.external_connected ? 1 : 0);
        if (sample.has_adapter_watts) {
            printf("adapter_watts=%.3f\n", sample.adapter_watts);
        }
        if (sample.has_adapter_max_watts) {
            printf("adapter_max_watts=%.3f\n", sample.adapter_max_watts);
        }
        if (sample.has_system_watts) {
            printf("system_watts=%.3f\n", sample.system_watts);
        }
        if (sample.has_battery) {
            print_battery("battery", &sample.battery);
        }
    }

    vs_battery_sample *peripherals = NULL;
    size_t count = 0;
    status = sensors->peripheral_batteries(sensors, &peripherals, &count);
    printf("[peripheral-batteries]\n");
    if (status != VS_OK) {
        printf("peripherals=%s\n", vs_result_string(status));
        return 0;
    }
    printf("peripheral_count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "peripheral%zu", i);
        print_battery(prefix, &peripherals[i]);
    }
    sensors->free_batteries(sensors, peripherals, count);
    return 0;
}

static int print_gpu(vs_sensors_system *sensors)
{
    vs_gpu_sample *samples = NULL;
    size_t count = 0;
    int status = sensors->gpus(sensors, &samples, &count);
    if (status != VS_OK) {
        fprintf(stderr, "gpu: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[gpu]\n");
    printf("gpu_count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%s vendor=%s driver=%s name=%s\n", samples[i].id,
               vs_gpu_vendor_string(samples[i].vendor), samples[i].driver, samples[i].name);
        printf("%s.has_busy=%d\n", samples[i].id, samples[i].has_busy ? 1 : 0);
        if (samples[i].has_busy) {
            printf("%s.busy=%.4f\n", samples[i].id, samples[i].busy);
        }
        printf("%s.has_vram=%d\n", samples[i].id, samples[i].has_vram ? 1 : 0);
        if (samples[i].has_vram) {
            printf("%s.vram_used_bytes=%llu\n", samples[i].id,
                   (unsigned long long)samples[i].vram_used_bytes);
            printf("%s.vram_total_bytes=%llu\n", samples[i].id,
                   (unsigned long long)samples[i].vram_total_bytes);
        }
        if (samples[i].has_temperature) {
            printf("%s.temperature_celsius=%.1f\n", samples[i].id, samples[i].temperature_celsius);
        }
        if (samples[i].has_watts) {
            printf("%s.watts=%.2f\n", samples[i].id, samples[i].watts);
        }
        if (samples[i].has_clock) {
            printf("%s.clock_mhz=%.1f\n", samples[i].id, samples[i].clock_mhz);
        }
    }
    sensors->free_gpus(sensors, samples, count);
    return 0;
}

static int print_temps(vs_sensors_system *sensors)
{
    vs_temperature_sample *temperatures = NULL;
    size_t count = 0;
    int status = sensors->temperatures(sensors, &temperatures, &count);
    if (status != VS_OK) {
        fprintf(stderr, "temps: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[temperatures]\n");
    printf("temperature_count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%s label=\"%s\" driver=%s kind=%s celsius=%.1f high=%.1f crit=%.1f primary=%d\n",
               temperatures[i].id, temperatures[i].label, temperatures[i].driver,
               vs_sensor_kind_string(temperatures[i].kind), temperatures[i].celsius,
               temperatures[i].high_celsius, temperatures[i].critical_celsius,
               temperatures[i].is_primary ? 1 : 0);
    }
    sensors->free_temperatures(sensors, temperatures, count);

    vs_thermal_pressure pressure = VS_THERMAL_NOMINAL;
    status = sensors->thermal_pressure(sensors, &pressure);
    printf("thermal_pressure=%s\n",
           status == VS_OK ? vs_thermal_pressure_string(pressure) : vs_result_string(status));

    vs_fan_sample *fans = NULL;
    size_t fan_count = 0;
    status = sensors->fans(sensors, &fans, &fan_count);
    if (status != VS_OK) {
        fprintf(stderr, "fans: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[fans]\n");
    printf("fan_count=%zu\n", fan_count);
    for (size_t i = 0; i < fan_count; i++) {
        printf("%s label=\"%s\" driver=%s rpm=%d min=%d max=%d controllable=%d pwm=%d\n",
               fans[i].id, fans[i].label, fans[i].driver, fans[i].rpm, fans[i].min_rpm,
               fans[i].max_rpm, fans[i].is_controllable ? 1 : 0, fans[i].pwm);
    }
    sensors->free_fans(sensors, fans, fan_count);
    return 0;
}

static int print_processes(vs_sensors_system *sensors, const vs_process_query *query)
{
    vs_process_sample *samples = NULL;
    size_t count = 0;
    int status = sensors->processes(sensors, query, &samples, &count);
    if (status != VS_OK) {
        fprintf(stderr, "procs: %s\n", vs_result_string(status));
        return 1;
    }
    printf("[processes]\n");
    printf("row_count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("pid=%d comm=%s name=\"%s\" group=%s cpu_percent=%.3f rss=%llu anon=%llu "
               "swap=%llu members=%u rate=%d\n",
               samples[i].pid, samples[i].comm, samples[i].name, samples[i].group_key,
               samples[i].cpu_percent, (unsigned long long)samples[i].rss_bytes,
               (unsigned long long)samples[i].anon_bytes, (unsigned long long)samples[i].swap_bytes,
               samples[i].member_count, samples[i].has_rate ? 1 : 0);
    }
    sensors->free_processes(sensors, samples, count);
    return 0;
}

static void on_event(const vs_sensors_event *event, void *user_data)
{
    (void)user_data;
    switch (event->type) {
    case VS_SENSORS_EVENT_THERMAL_PRESSURE:
        printf("event=thermal_pressure value=%s\n",
               vs_thermal_pressure_string(event->thermal_pressure));
        break;
    case VS_SENSORS_EVENT_BACKEND_LOST:
        printf("event=backend_lost\n");
        break;
    }
}

int main(int argc, char **argv)
{
    vs_sensors_options options;
    memset(&options, 0, sizeof options);
    double interval = 1.0;
    int samples = 2;
    vs_process_query query;
    memset(&query, 0, sizeof query);
    query.sort = VS_PROCESS_SORT_CPU;
    query.limit = 5;
    query.group_by_app = true;
    query.minimum_cpu_percent = 0.01;

    int index = 1;
    for (; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--root") == 0 && index + 1 < argc) {
            options.root = argv[++index];
        } else if (strcmp(argument, "--power") == 0 && index + 1 < argc) {
            options.power_backend = argv[++index];
        } else if (strcmp(argument, "--no-nvml") == 0) {
            options.disable_nvml = true;
        } else if (strcmp(argument, "--interval") == 0 && index + 1 < argc) {
            interval = strtod(argv[++index], NULL);
        } else if (strcmp(argument, "--count") == 0 && index + 1 < argc) {
            samples = (int)strtol(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--limit") == 0 && index + 1 < argc) {
            query.limit = (size_t)strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--sort") == 0 && index + 1 < argc) {
            query.sort = strcmp(argv[++index], "memory") == 0 ? VS_PROCESS_SORT_MEMORY
                                                              : VS_PROCESS_SORT_CPU;
        } else if (strcmp(argument, "--no-group") == 0) {
            query.group_by_app = false;
        } else if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            usage();
            return 0;
        } else if (argument[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argument);
            usage();
            return 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        usage();
        return 2;
    }
    const char *command = argv[index];
    if (samples < 1) {
        samples = 1;
    }

    int result = VS_OK;
    vs_sensors_system *sensors = vs_sensors_system_create(&options, &result);
    if (sensors == NULL) {
        fprintf(stderr, "vs-sensors: no backend: %s\n", vs_result_string(result));
        return 1;
    }

    int status = 0;
    if (strcmp(command, "caps") == 0) {
        print_caps(sensors);
    } else if (strcmp(command, "watch") == 0) {
        sensors->set_event_callback(sensors, on_event, NULL);
        for (int i = 0; i < samples && status == 0; i++) {
            if (i > 0) {
                sleep_seconds(interval);
            }
            printf("[sample %d]\n", i);
            status = print_cpu(sensors);
            if (status == 0) {
                status = print_memory(sensors);
            }
            if (status == 0) {
                status = print_network(sensors);
            }
            if (status == 0 && (sensors->capabilities & VS_SENSORS_HAS_HWMON) != 0) {
                status = print_temps(sensors);
            }
            sensors->dispatch(sensors);
        }
    } else {
        /* Rate-bearing commands need two samples separated by the interval;
         * --count 1 asks for the raw counters only. */
        bool rate_bearing = strcmp(command, "cpu") == 0 || strcmp(command, "net") == 0 ||
                            strcmp(command, "disk") == 0 || strcmp(command, "procs") == 0;
        int rounds = rate_bearing ? samples : 1;
        for (int i = 0; i < rounds && status == 0; i++) {
            if (i > 0) {
                sleep_seconds(interval);
            }
            bool last = i == rounds - 1;
            if (!last) {
                /* Prime the deltas without printing. */
                if (strcmp(command, "cpu") == 0) {
                    vs_cpu_sample discard;
                    sensors->cpu(sensors, &discard);
                } else if (strcmp(command, "net") == 0) {
                    vs_network_sample *discard = NULL;
                    size_t discard_count = 0;
                    if (sensors->network(sensors, &discard, &discard_count) == VS_OK) {
                        sensors->free_network(sensors, discard, discard_count);
                    }
                } else if (strcmp(command, "disk") == 0) {
                    vs_disk_device_sample *discard = NULL;
                    size_t discard_count = 0;
                    if (sensors->disk_devices(sensors, &discard, &discard_count) == VS_OK) {
                        sensors->free_disk_devices(sensors, discard, discard_count);
                    }
                } else {
                    vs_process_sample *discard = NULL;
                    size_t discard_count = 0;
                    if (sensors->processes(sensors, &query, &discard, &discard_count) == VS_OK) {
                        sensors->free_processes(sensors, discard, discard_count);
                    }
                }
                continue;
            }
            if (strcmp(command, "cpu") == 0) {
                status = print_cpu(sensors);
            } else if (strcmp(command, "memory") == 0) {
                status = print_memory(sensors);
            } else if (strcmp(command, "net") == 0) {
                status = print_network(sensors);
            } else if (strcmp(command, "disk") == 0) {
                status = print_disk(sensors);
            } else if (strcmp(command, "power") == 0) {
                status = print_power(sensors);
            } else if (strcmp(command, "gpu") == 0) {
                status = print_gpu(sensors);
            } else if (strcmp(command, "temps") == 0) {
                status = print_temps(sensors);
            } else if (strcmp(command, "procs") == 0) {
                status = print_processes(sensors, &query);
            } else {
                fprintf(stderr, "unknown command: %s\n", command);
                usage();
                status = 2;
            }
        }
    }

    sensors->destroy(sensors);
    return status;
}
