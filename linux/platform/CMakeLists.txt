# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

cmake_minimum_required(VERSION 3.22)
project(vorssaint-platform C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    -Wall -Wextra -Werror
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
)

# A bare `cmake -S linux/platform -B build` must produce what ships. Without
# this, "no build type" is a fourth configuration nothing is ever checked
# against: GCC inlines more at -O2 and so reasons differently about truncation
# and maybe-uninitialized, and _FORTIFY_SOURCE is inert without optimisation.
# scripts/build-matrix.sh builds all four for exactly that reason (AGENT_PLAYBOOK).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type; RelWithDebInfo is what is installed" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo")
endif()

include(CTest)

add_library(vorssaint_platform_headers INTERFACE)
target_include_directories(vorssaint_platform_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The header's common section: shared by every concern, owned by none of them.
add_library(vs_platform_common STATIC vs_result.c)
target_link_libraries(vs_platform_common PUBLIC vorssaint_platform_headers)

add_subdirectory(window)
add_subdirectory(audio)
