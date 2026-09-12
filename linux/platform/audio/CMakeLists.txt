# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_package(PkgConfig REQUIRED)

pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

# ------------------------------------------------------------------ library

add_library(vs_audio STATIC
    vs_audio.c
    backend_pipewire.c
    backend_pulse.c)

target_include_directories(vs_audio
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# libm for cbrtf: the linear/cubic conversion the whole percentage mapping
# rests on.
target_link_libraries(vs_audio
    PUBLIC vs_platform_common
    PRIVATE PkgConfig::PIPEWIRE PkgConfig::PULSE m)

# -------------------------------------------------------------------- tools

add_executable(vs-audio tools/vs_audio_cli.c)
target_link_libraries(vs-audio PRIVATE vs_audio)

install(TARGETS vs-audio RUNTIME DESTINATION bin)

# -------------------------------------------------------------------- tests

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
