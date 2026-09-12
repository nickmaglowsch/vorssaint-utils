# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_package(PkgConfig REQUIRED)

# sd-bus, for UPower. Everything else here is /proc and /sys, and NVML is
# dlopened at runtime and deliberately not linked: see nvml.c.
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

add_library(vs_sensors STATIC
    vs_sensors.c
    util.c
    cpu.c
    memory.c
    net.c
    disk.c
    hwmon.c
    power_sysfs.c
    power_upower.c
    gpu.c
    nvml.c
    procs.c)

target_include_directories(vs_sensors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vs_sensors
    PUBLIC vs_platform_common
    PRIVATE PkgConfig::SDBUS ${CMAKE_DL_LIBS})

add_executable(vs-sensors tools/vs_sensors_cli.c)
target_link_libraries(vs-sensors PRIVATE vs_sensors)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
