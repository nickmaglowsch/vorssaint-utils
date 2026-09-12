# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gio-unix-2.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3 libspa-0.2)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

# No encoder here on purpose: WP-B5 owns ffmpeg. This library ends at raw
# frames and interleaved PCM, which is exactly what an encoder wants as input,
# and nothing in it knows what a codec is.

add_library(vs_capture STATIC
    vs_capture.c
    vs_capture_portal.c
    vs_capture_sources.c
    vs_capture_stream.c
    vs_capture_timing.c
    vs_capture_png.c
    vs_capture_png_read.c
    $<TARGET_OBJECTS:vorssaint_platform_common>)

target_include_directories(vs_capture
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# PipeWire's public headers use anonymous unions and designated initialisers in
# ways GCC flags under -Wpedantic; the project does not use -Wpedantic, but the
# spa_pod_builder macros do shadow, so they are quarantined to this library's
# own translation units rather than being allowed project-wide.
target_link_libraries(vs_capture
    PUBLIC PkgConfig::GLIB
    PRIVATE PkgConfig::PIPEWIRE PkgConfig::WAYLAND_CLIENT PkgConfig::ZLIB
            Threads::Threads)

# -------------------------------------------------------------------- tools

add_executable(vs-capture tools/vs_capture_cli.c)
# libm for the RMS the harness reports; the library itself needs no maths.
target_link_libraries(vs-capture PRIVATE vs_capture m)

install(TARGETS vs-capture RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../include/vorssaint_platform.h
        DESTINATION include)

# -------------------------------------------------------------------- tests

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
