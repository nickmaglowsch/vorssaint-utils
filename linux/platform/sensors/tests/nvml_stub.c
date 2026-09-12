/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * A stand-in for libnvidia-ml.so.1 that implements the same entry points and
 * the same ABI, so the dlopen binding in nvml.c can be exercised on a machine
 * with no NVIDIA driver. It reports one device with values the test asserts on.
 *
 * This is a test double, not an emulator: it answers the ten calls nvml.c looks
 * up and nothing else.
 */

#include <stdlib.h>
#include <string.h>

#define NVML_SUCCESS 0
#define NVML_ERROR_NOT_SUPPORTED 3

typedef struct nvml_utilization {
    unsigned int gpu;
    unsigned int memory;
} nvml_utilization;

typedef struct nvml_memory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvml_memory;

static int initialized;

int nvmlInit_v2(void);
int nvmlShutdown(void);
int nvmlDeviceGetCount_v2(unsigned int *count);
int nvmlDeviceGetHandleByIndex_v2(unsigned int index, void **device);
int nvmlDeviceGetName(void *device, char *name, unsigned int length);
int nvmlDeviceGetUtilizationRates(void *device, nvml_utilization *utilization);
int nvmlDeviceGetMemoryInfo(void *device, nvml_memory *memory);
int nvmlDeviceGetTemperature(void *device, int sensor, unsigned int *celsius);
int nvmlDeviceGetPowerUsage(void *device, unsigned int *milliwatts);
int nvmlDeviceGetClockInfo(void *device, int type, unsigned int *mhz);

int nvmlInit_v2(void)
{
    /* VS_NVML_STUB_FAIL makes the library load but refuse to initialise, which
     * is what a driver/library version mismatch looks like. */
    if (getenv("VS_NVML_STUB_FAIL") != NULL) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    initialized = 1;
    return NVML_SUCCESS;
}

int nvmlShutdown(void)
{
    initialized = 0;
    return NVML_SUCCESS;
}

int nvmlDeviceGetCount_v2(unsigned int *count)
{
    if (!initialized) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *count = 1;
    return NVML_SUCCESS;
}

int nvmlDeviceGetHandleByIndex_v2(unsigned int index, void **device)
{
    if (!initialized || index != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    /* A non-NULL opaque handle is all nvml.c does with this. */
    *device = (void *)&initialized;
    return NVML_SUCCESS;
}

int nvmlDeviceGetName(void *device, char *name, unsigned int length)
{
    (void)device;
    const char *value = "NVIDIA GeForce RTX 4070 (stub)";
    if (length == 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    strncpy(name, value, length - 1);
    name[length - 1] = '\0';
    return NVML_SUCCESS;
}

int nvmlDeviceGetUtilizationRates(void *device, nvml_utilization *utilization)
{
    (void)device;
    utilization->gpu = 73;
    utilization->memory = 41;
    return NVML_SUCCESS;
}

int nvmlDeviceGetMemoryInfo(void *device, nvml_memory *memory)
{
    (void)device;
    memory->total = 12884901888ull;
    memory->used = 3221225472ull;
    memory->free = memory->total - memory->used;
    return NVML_SUCCESS;
}

int nvmlDeviceGetTemperature(void *device, int sensor, unsigned int *celsius)
{
    (void)device;
    if (sensor != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *celsius = 64;
    return NVML_SUCCESS;
}

int nvmlDeviceGetPowerUsage(void *device, unsigned int *milliwatts)
{
    (void)device;
    *milliwatts = 148500;
    return NVML_SUCCESS;
}

int nvmlDeviceGetClockInfo(void *device, int type, unsigned int *mhz)
{
    (void)device;
    if (type != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *mhz = 2610;
    return NVML_SUCCESS;
}
