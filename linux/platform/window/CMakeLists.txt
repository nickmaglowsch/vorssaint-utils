# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_package(PkgConfig REQUIRED)

pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-ewmh xcb-icccm)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

# --------------------------------------------------------- generated protocol

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# ext-foreign-toplevel-list-v1 ships in wayland-protocols (staging) from 1.32.
# wlr-foreign-toplevel-management has no distribution package at all, so it is
# vendored; see protocols/README.md.
set(EXT_TOPLEVEL_XML
    ${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml)
if(NOT EXISTS ${EXT_TOPLEVEL_XML})
    message(FATAL_ERROR
        "ext-foreign-toplevel-list-v1.xml not found under ${WAYLAND_PROTOCOLS_DIR}. "
        "Install wayland-protocols >= 1.32, or vendor the XML into protocols/.")
endif()

function(vs_wayland_protocol xml stem)
    add_custom_command(
        OUTPUT ${GENERATED_DIR}/${stem}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${GENERATED_DIR}/${stem}-client-protocol.h
        DEPENDS ${xml}
        VERBATIM)
    add_custom_command(
        OUTPUT ${GENERATED_DIR}/${stem}-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${GENERATED_DIR}/${stem}-protocol.c
        DEPENDS ${xml}
        VERBATIM)
endfunction()

vs_wayland_protocol(${EXT_TOPLEVEL_XML} ext-foreign-toplevel-list-v1)
vs_wayland_protocol(
    ${PROTOCOL_DIR}/wlr-foreign-toplevel-management-unstable-v1.xml
    wlr-foreign-toplevel-management-unstable-v1)

set(PROTOCOL_SOURCES
    ${GENERATED_DIR}/ext-foreign-toplevel-list-v1-protocol.c
    ${GENERATED_DIR}/ext-foreign-toplevel-list-v1-client-protocol.h
    ${GENERATED_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    ${GENERATED_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)

# wayland-scanner's private-code output trips -Wmissing-prototypes; it is not
# ours to fix, so it is compiled as its own object library without that flag.
add_library(vs_window_protocols OBJECT ${PROTOCOL_SOURCES})
target_include_directories(vs_window_protocols PUBLIC ${GENERATED_DIR})
target_link_libraries(vs_window_protocols PUBLIC PkgConfig::WAYLAND)
target_compile_options(vs_window_protocols PRIVATE -Wno-missing-prototypes)

# ------------------------------------------------------------------ library

add_library(vs_window STATIC
    vs_window.c
    backend_x11.c
    backend_wlr.c
    backend_hyprland.c
    backend_kwin.c
    bridge_client.c
    sway_ipc.c
    $<TARGET_OBJECTS:vs_window_protocols>)

target_include_directories(vs_window
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${GENERATED_DIR})

target_link_libraries(vs_window
    PRIVATE PkgConfig::XCB PkgConfig::WAYLAND PkgConfig::JSONC PkgConfig::SDBUS)

add_dependencies(vs_window vs_window_protocols)

# -------------------------------------------------------------------- tools

add_executable(vs-window tools/vs_window_cli.c)
target_link_libraries(vs-window PRIVATE vs_window)

install(TARGETS vs-window RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../include/vorssaint_platform.h
        DESTINATION include)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/kwin/vorssaint-window.js
        DESTINATION share/vorssaint/kwin)

# -------------------------------------------------------------------- tests

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
