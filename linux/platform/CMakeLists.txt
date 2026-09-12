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

# A bare `cmake -S . -B build` must produce what ships. "No build type" is its
# own configuration -- GCC inlines less without optimisation and so reasons
# differently about -Wmaybe-uninitialized and -Wformat-truncation -- and under
# -Werror a warning only one configuration sees is a build failure for whoever
# hits it first. capture/scripts/build-matrix.sh checks all four.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type; RelWithDebInfo is what is installed" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo")
endif()

include(CTest)

add_library(vorssaint_platform_headers INTERFACE)
target_include_directories(vorssaint_platform_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The few contract symbols that belong to no single concern. Linked as objects
# into each backend library, so a binary using two of them still has exactly one
# definition of each.
add_library(vorssaint_platform_common OBJECT vs_result.c)
target_link_libraries(vorssaint_platform_common PUBLIC vorssaint_platform_headers)

add_subdirectory(window)
add_subdirectory(capture)
