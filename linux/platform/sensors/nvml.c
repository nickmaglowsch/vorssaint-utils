/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * NVIDIA telemetry through NVML, `dlopen`ed at runtime and never linked.
 *
 * The AppImage and the Flatpak have to start on a machine with no NVIDIA driver
 * at all, so there is no `-lnvidia-ml` anywhere in this tree and no NVML header
 * is vendored: the six entry points used here are declared locally against the
 * ABI NVIDIA has published unchanged since driver 340, and every one of them is
 * looked up by name. A machine without the driver takes the `dlopen` failure
 * path, `VS_SENSORS_HAS_NVML` stays clear, and the GPU list simply has no
 * NVIDIA row.
 *
 * `VS_NVML_LIBRARY` overrides the soname. It exists so the test suite can load
 * a stub that implements this same ABI and prove the binding really reads what
 * NVML returns, on a container that has no GPU; nothing in the product sets it.
 */

#include "vs_sensors_internal.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_DEVICE_NAME_BUFFER_SIZE 96

typedef struct nvml_utilization {
    unsigned int gpu;
    unsigned int memory;
} nvml_utilization;

typedef struct nvml_memory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvml_memory;

typedef void *nvml_device;

struct vs_nvml {
    void *handle;
    int (*init)(void);
    int (*shutdown)(void);
    int (*device_count)(unsigned int *count);
    int (*device_handle)(unsigned int index, nvml_device *device);
    int (*device_name)(nvml_device device, char *name, unsigned int length);
    int (*utilization)(nvml_device device, nvml_utilization *utilization);
    int (*memory_info)(nvml_device device, nvml_memory *memory);
    int (*temperature)(nvml_device device, int sensor, unsigned int *celsius);
    int (*power_usage)(nvml_device device, unsigned int *milliwatts);
    int (*clock_info)(nvml_device device, int type, unsigned int *mhz);
};

static void *resolve(void *handle, const char *name)
{
    /* dlsym returns a data pointer; converting it to a function pointer is
     * defined by POSIX but not by ISO C, and -Wpedantic would say so. The cast
     * through the object pointer is the documented idiom. */
    return dlsym(handle, name);
}

struct vs_nvml *vs_nvml_open(void)
{
    const char *soname = getenv("VS_NVML_LIBRARY");
    if (soname == NULL || soname[0] == '\0') {
        soname = "libnvidia-ml.so.1";
    }
    void *handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        return NULL;
    }

    struct vs_nvml *nvml = calloc(1, sizeof *nvml);
    if (nvml == NULL) {
        dlclose(handle);
        return NULL;
    }
    nvml->handle = handle;
    *(void **)&nvml->init = resolve(handle, "nvmlInit_v2");
    if (nvml->init == NULL) {
        *(void **)&nvml->init = resolve(handle, "nvmlInit");
    }
    *(void **)&nvml->shutdown = resolve(handle, "nvmlShutdown");
    *(void **)&nvml->device_count = resolve(handle, "nvmlDeviceGetCount_v2");
    if (nvml->device_count == NULL) {
        *(void **)&nvml->device_count = resolve(handle, "nvmlDeviceGetCount");
    }
    *(void **)&nvml->device_handle = resolve(handle, "nvmlDeviceGetHandleByIndex_v2");
    if (nvml->device_handle == NULL) {
        *(void **)&nvml->device_handle = resolve(handle, "nvmlDeviceGetHandleByIndex");
    }
    *(void **)&nvml->device_name = resolve(handle, "nvmlDeviceGetName");
    *(void **)&nvml->utilization = resolve(handle, "nvmlDeviceGetUtilizationRates");
    *(void **)&nvml->memory_info = resolve(handle, "nvmlDeviceGetMemoryInfo");
    *(void **)&nvml->temperature = resolve(handle, "nvmlDeviceGetTemperature");
    *(void **)&nvml->power_usage = resolve(handle, "nvmlDeviceGetPowerUsage");
    *(void **)&nvml->clock_info = resolve(handle, "nvmlDeviceGetClockInfo");

    /* Without these four there is nothing worth reporting; the rest degrade
     * field by field. */
    if (nvml->init == NULL || nvml->device_count == NULL || nvml->device_handle == NULL ||
        nvml->utilization == NULL) {
        dlclose(handle);
        free(nvml);
        return NULL;
    }
    if (nvml->init() != NVML_SUCCESS) {
        dlclose(handle);
        free(nvml);
        return NULL;
    }
    return nvml;
}

void vs_nvml_close(struct vs_nvml *nvml)
{
    if (nvml == NULL) {
        return;
    }
    if (nvml->shutdown != NULL) {
        nvml->shutdown();
    }
    dlclose(nvml->handle);
    free(nvml);
}

size_t vs_nvml_collect(struct vs_nvml *nvml, vs_gpu_sample **out, size_t *count, size_t *capacity)
{
    if (nvml == NULL || out == NULL || count == NULL || capacity == NULL) {
        return 0;
    }
    unsigned int device_count = 0;
    if (nvml->device_count(&device_count) != NVML_SUCCESS) {
        return 0;
    }

    size_t added = 0;
    for (unsigned int i = 0; i < device_count; i++) {
        nvml_device device = NULL;
        if (nvml->device_handle(i, &device) != NVML_SUCCESS) {
            continue;
        }
        if (*count == *capacity) {
            size_t next = *capacity == 0 ? 4 : *capacity * 2;
            vs_gpu_sample *grown = realloc(*out, next * sizeof *grown);
            if (grown == NULL) {
                break;
            }
            *out = grown;
            *capacity = next;
        }
        vs_gpu_sample *sample = &(*out)[*count];
        memset(sample, 0, sizeof *sample);
        snprintf(sample->id, sizeof sample->id, "nvml%u", i);
        vs_copy(sample->driver, sizeof sample->driver, "nvidia");
        sample->vendor = VS_GPU_VENDOR_NVIDIA;
        vs_copy(sample->name, sizeof sample->name, "NVIDIA GPU");

        char name[NVML_DEVICE_NAME_BUFFER_SIZE];
        if (nvml->device_name != NULL &&
            nvml->device_name(device, name, sizeof name) == NVML_SUCCESS) {
            name[sizeof name - 1] = '\0';
            vs_copy(sample->name, sizeof sample->name, name);
        }
        nvml_utilization utilization;
        memset(&utilization, 0, sizeof utilization);
        if (nvml->utilization(device, &utilization) == NVML_SUCCESS) {
            sample->busy = (double)utilization.gpu / 100.0;
            sample->has_busy = true;
        }
        nvml_memory memory;
        memset(&memory, 0, sizeof memory);
        if (nvml->memory_info != NULL && nvml->memory_info(device, &memory) == NVML_SUCCESS &&
            memory.total > 0) {
            sample->vram_used_bytes = memory.used;
            sample->vram_total_bytes = memory.total;
            sample->has_vram = true;
        }
        unsigned int value = 0;
        if (nvml->temperature != NULL &&
            nvml->temperature(device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) {
            sample->temperature_celsius = (double)value;
            sample->has_temperature = true;
        }
        if (nvml->power_usage != NULL && nvml->power_usage(device, &value) == NVML_SUCCESS) {
            sample->watts = (double)value / 1000.0;
            sample->has_watts = true;
        }
        if (nvml->clock_info != NULL &&
            nvml->clock_info(device, NVML_CLOCK_GRAPHICS, &value) == NVML_SUCCESS) {
            sample->clock_mhz = (double)value;
            sample->has_clock = true;
        }
        (*count)++;
        added++;
    }
    return added;
}
