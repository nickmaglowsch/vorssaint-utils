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

include(CTest)

add_library(vorssaint_platform_headers INTERFACE)
target_include_directories(vorssaint_platform_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(window)
