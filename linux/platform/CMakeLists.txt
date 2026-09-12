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

# The fifth leg of scripts/build-matrix.sh. ASan/UBSan/LSan catch what no
# optimisation level can: a use-after-free in a callback that fires long after
# the call that armed it, and an exit path that leaks. Off by default because
# the sanitizers change timing enough to matter for the live suites, and on in
# the matrix so nothing reaches review without having been run under them.
option(VS_SANITIZE "Build with AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer" OFF)
if(VS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    # UBSan otherwise prints and continues, so a test can pass with undefined
    # behaviour in it. Trapping on the first one is the point.
    add_compile_options(-fno-sanitize-recover=undefined)
endif()

include(CTest)

add_library(vorssaint_platform_headers INTERFACE)
target_include_directories(vorssaint_platform_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The header's common section: the few contract symbols that belong to every
# concern and to none of them. STATIC rather than OBJECT, so a binary linking
# two backends gets one definition from the archive instead of two copies.
add_library(vs_platform_common STATIC vs_result.c)
target_link_libraries(vs_platform_common PUBLIC vorssaint_platform_headers)

add_subdirectory(window)
add_subdirectory(capture)
add_subdirectory(audio)
add_subdirectory(sensors)
