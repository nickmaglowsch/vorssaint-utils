/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/proc/diskstats` throughput per device and `statvfs` capacity per mounted
 * real filesystem.
 *
 * Out of scope, deliberately: SMART and NVMe health. `DiskSMARTReading` in
 * Sources/VorssaintCore/Services/Metrics/DiskSupport.swift is filled on macOS
 * from IOKit properties that need no privilege. The Linux equivalents do:
 * SMART needs SG_IO on the block device and NVMe health needs the
 * NVME_IOCTL_ADMIN_CMD ioctl, both root-only on a stock distribution. That
 * makes it a helper method (WP-S1's daemon) with its own polkit action, not
 * something this unprivileged library can read, so the fields stay nil and the
 * panel hides the rows. Nothing here opens a block device.
 */

#include "vs_sensors_internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

/* The kernel documents /proc/diskstats sector counts as fixed 512-byte units
 * regardless of the device's logical block size (Documentation/admin-guide/
 * iostats.rst), so this is not queue/hw_sector_size. */
#define VS_DISKSTAT_SECTOR_BYTES 512u

static const char *const pseudo_filesystems[] = {
    "autofs",  "bpf",       "binfmt_misc", "cgroup",   "cgroup2",  "configfs", "debugfs",
    "devpts",  "devtmpfs",  "efivarfs",    "fuse.gvfsd-fuse",      "fusectl",  "hugetlbfs",
    "mqueue",  "nsfs",      "overlay",     "proc",     "pstore",   "ramfs",    "rpc_pipefs",
    "securityfs", "selinuxfs", "sysfs",    "tmpfs",    "tracefs",  "vboxsf",
};

static bool is_pseudo(const char *filesystem)
{
    for (size_t i = 0; i < sizeof pseudo_filesystems / sizeof pseudo_filesystems[0]; i++) {
        if (strcmp(pseudo_filesystems[i], filesystem) == 0) {
            return true;
        }
    }
    return false;
}

/* /proc/self/mounts octal-escapes space, tab, newline and backslash. */
static void unescape(char *s)
{
    char *read = s;
    char *write = s;
    while (*read != '\0') {
        if (read[0] == '\\' && read[1] >= '0' && read[1] <= '3' && read[2] >= '0' &&
            read[2] <= '7' && read[3] >= '0' && read[3] <= '7') {
            *write++ = (char)(((read[1] - '0') << 6) | ((read[2] - '0') << 3) | (read[3] - '0'));
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static const vs_counter_prev *find_previous(const vs_sensors_impl *impl, const char *name)
{
    for (size_t i = 0; i < impl->prev_disk_count; i++) {
        if (strcmp(impl->prev_disk[i].name, name) == 0) {
            return &impl->prev_disk[i];
        }
    }
    return NULL;
}

int vs_sensors_read_disk_devices(vs_sensors_impl *impl, vs_disk_device_sample **out,
                                 size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    FILE *file = vs_open_rooted(impl, "/proc/diskstats");
    if (file == NULL) {
        return VS_ERR_BACKEND;
    }

    double now = vs_now();
    double elapsed = now - impl->prev_disk_time;
    bool have_previous = impl->prev_disk != NULL && impl->prev_disk_count > 0 && elapsed > 0;

    vs_disk_device_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[1024];
    int result = VS_OK;

    while (fgets(line, sizeof line, file) != NULL) {
        char name[VS_SENSORS_NAME_MAX];
        unsigned long long read_ios = 0;
        unsigned long long read_sectors = 0;
        unsigned long long write_ios = 0;
        unsigned long long write_sectors = 0;
        unsigned long long io_ticks = 0;
        /* major minor name r_ios r_merges r_sectors r_ms w_ios w_merges w_sectors w_ms
         * in_flight io_ticks ... */
        if (sscanf(line, "%*u %*u %63s %llu %*u %llu %*u %llu %*u %llu %*u %*u %llu", name,
                   &read_ios, &read_sectors, &write_ios, &write_sectors, &io_ticks) != 6) {
            continue;
        }

        if (count == capacity) {
            size_t next = capacity == 0 ? 16 : capacity * 2;
            vs_disk_device_sample *grown = realloc(samples, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            samples = grown;
            capacity = next;
        }

        vs_disk_device_sample *sample = &samples[count];
        memset(sample, 0, sizeof *sample);
        vs_copy(sample->name, sizeof sample->name, name);
        sample->read_ios = read_ios;
        sample->write_ios = write_ios;
        sample->read_bytes = (uint64_t)read_sectors * VS_DISKSTAT_SECTOR_BYTES;
        sample->write_bytes = (uint64_t)write_sectors * VS_DISKSTAT_SECTOR_BYTES;
        sample->io_ticks = io_ticks;

        char path[VS_FULLPATH_MAX];
        sample->is_partition = vs_pathf(impl, path, sizeof path, "/sys/class/block/%s/partition",
                                        name) &&
                               vs_path_exists(path);

        if (have_previous) {
            const vs_counter_prev *previous = find_previous(impl, name);
            if (previous != NULL) {
                sample->read_bytes_per_second = vs_rate(previous->a, sample->read_bytes, elapsed);
                sample->write_bytes_per_second = vs_rate(previous->b, sample->write_bytes, elapsed);
                sample->has_rates = true;
            }
        }
        count++;
    }
    fclose(file);

    if (result != VS_OK) {
        free(samples);
        return result;
    }

    vs_counter_prev *next_prev = count > 0 ? calloc(count, sizeof *next_prev) : NULL;
    if (next_prev != NULL) {
        for (size_t i = 0; i < count; i++) {
            vs_copy(next_prev[i].name, sizeof next_prev[i].name, samples[i].name);
            next_prev[i].a = samples[i].read_bytes;
            next_prev[i].b = samples[i].write_bytes;
        }
        free(impl->prev_disk);
        impl->prev_disk = next_prev;
        impl->prev_disk_count = count;
        impl->prev_disk_time = now;
    }

    *out = samples;
    *count_out = count;
    return VS_OK;
}

/* "/dev/nvme0n1p2" -> "nvme0n1p2"; a UUID= or LABEL= source has no device. */
static void device_name_of(const char *source, char *out, size_t out_len)
{
    out[0] = '\0';
    if (strncmp(source, "/dev/", 5) != 0) {
        return;
    }
    const char *tail = source + 5;
    const char *slash = strrchr(tail, '/');
    vs_copy(out, out_len, slash != NULL ? slash + 1 : tail);
}

int vs_sensors_read_disk_mounts(vs_sensors_impl *impl, vs_disk_mount_sample **out,
                                size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    FILE *file = vs_open_rooted(impl, "/proc/self/mounts");
    if (file == NULL) {
        return VS_ERR_BACKEND;
    }

    vs_disk_mount_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[2048];
    int result = VS_OK;

    while (fgets(line, sizeof line, file) != NULL) {
        char source[VS_SENSORS_PATH_MAX];
        char mount_point[VS_SENSORS_PATH_MAX];
        char filesystem[VS_SENSORS_NAME_MAX];
        char options[512];
        if (sscanf(line, "%255s %255s %63s %511s", source, mount_point, filesystem, options) != 4) {
            continue;
        }
        unescape(source);
        unescape(mount_point);
        if (is_pseudo(filesystem)) {
            continue;
        }

        /* The same device mounted twice (a bind mount) is one volume to the
         * user; keep the first mount point, as the macOS side keeps the first
         * volume URL. */
        bool duplicate = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(samples[i].source, source) == 0 &&
                strcmp(samples[i].mount_point, mount_point) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            vs_disk_mount_sample *grown = realloc(samples, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            samples = grown;
            capacity = next;
        }

        vs_disk_mount_sample *sample = &samples[count];
        memset(sample, 0, sizeof *sample);
        vs_copy(sample->mount_point, sizeof sample->mount_point, mount_point);
        vs_copy(sample->source, sizeof sample->source, source);
        vs_copy(sample->filesystem, sizeof sample->filesystem, filesystem);
        device_name_of(source, sample->device, sizeof sample->device);
        sample->is_read_only = strncmp(options, "ro", 2) == 0 &&
                               (options[2] == '\0' || options[2] == ',');

        /* statvfs is the one call that cannot be redirected by a root prefix,
         * so under a fixture root it is applied to the rooted path: the numbers
         * then describe the filesystem the fixture lives on, which is real, and
         * the test asserts the parse and the filtering rather than the size. */
        char target[VS_FULLPATH_MAX];
        struct statvfs vfs;
        if (vs_path(impl, target, sizeof target, mount_point) && statvfs(target, &vfs) == 0) {
            uint64_t unit = vfs.f_frsize != 0 ? (uint64_t)vfs.f_frsize : (uint64_t)vfs.f_bsize;
            sample->total_bytes = (uint64_t)vfs.f_blocks * unit;
            sample->free_bytes = (uint64_t)vfs.f_bavail * unit;
        }
        count++;
    }
    fclose(file);

    if (result != VS_OK) {
        free(samples);
        return result;
    }

    *out = samples;
    *count_out = count;
    return VS_OK;
}
