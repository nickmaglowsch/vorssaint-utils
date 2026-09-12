# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_program(BASH_PROGRAM bash REQUIRED)
find_program(PYTHON3_PROGRAM NAMES python3.12 python3 REQUIRED)

# Pure-C unit tests over the parsers and the classification rules, driven by the
# fixture trees under fixtures/. No /proc, no /sys, no bus.
add_executable(test_sensors_support test_sensors_support.c)
target_link_libraries(test_sensors_support PRIVATE vs_sensors)
target_compile_definitions(test_sensors_support
    PRIVATE VS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME sensors_support COMMAND test_sensors_support)

# The NVML binding against a stub that implements the same ABI: proves the
# dlopen path reads what NVML returns on a machine with no NVIDIA driver.
add_library(nvml_stub SHARED nvml_stub.c)
set_target_properties(nvml_stub PROPERTIES OUTPUT_NAME "nvidia-ml-stub")

function(vs_add_sensor_shell_test name script)
    add_test(NAME ${name}
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/${script}
                     $<TARGET_FILE:vs-sensors>)
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "VS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR};VS_PYTHON=${PYTHON3_PROGRAM};VS_NVML_STUB=$<TARGET_FILE:nvml_stub>"
        TIMEOUT 180)
endfunction()

# Fixture trees: machines this container is not. Each is a captured layout from
# a real machine's /proc and /sys, trimmed to the files this backend reads.
vs_add_sensor_shell_test(sensors_fixture_amd test_fixture_amd.sh)
vs_add_sensor_shell_test(sensors_fixture_intel test_fixture_intel.sh)
vs_add_sensor_shell_test(sensors_fixture_thinkpad test_fixture_thinkpad.sh)

# Live reads of what this machine really does have: /proc/stat, /proc/meminfo,
# /proc/net/dev, /proc/diskstats, /proc/self/mounts, /proc/<pid>/stat.
vs_add_sensor_shell_test(sensors_live_procfs test_live_procfs.sh)

# NVML through the stub, and the no-driver path.
vs_add_sensor_shell_test(sensors_nvml test_nvml.sh)

# UPower over a private session... system bus is not available in a container,
# so a fake UPower service on a private bus stands in for the daemon.
vs_add_sensor_shell_test(sensors_upower_fake test_upower_fake.sh)
