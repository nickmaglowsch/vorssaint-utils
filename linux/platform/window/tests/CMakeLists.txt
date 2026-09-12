# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_program(BASH_PROGRAM bash REQUIRED)
find_program(PYTHON3_PROGRAM NAMES python3.12 python3 REQUIRED)

# Pure-C unit tests: no display, no compositor, no bus.
add_executable(test_window_support test_window_support.c)
target_link_libraries(test_window_support PRIVATE vs_window)
target_include_directories(test_window_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME window_support COMMAND test_window_support)

function(vs_add_shell_test name script)
    add_test(NAME ${name}
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/${script}
                     $<TARGET_FILE:vs-window>)
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "VS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR};VS_PYTHON=${PYTHON3_PROGRAM};VS_KWIN_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/../kwin/vorssaint-window.js"
        TIMEOUT 240)
endfunction()

# Run here: a real X server (Xvfb + openbox) and a real compositor (headless sway).
vs_add_shell_test(window_x11 test_x11.sh)
vs_add_shell_test(window_sway test_sway.sh)

# Cannot run here: Hyprland needs DRM, KWin and GNOME Shell need their own
# session. All three are driven against a fake that speaks the documented
# protocol, on a private session bus where the fake is a D-Bus service.
vs_add_shell_test(window_hyprland_fake test_hyprland_fake.sh)
vs_add_shell_test(window_kwin_fake test_kwin_fake.sh)
vs_add_shell_test(window_gnome_fake test_gnome_fake.sh)

# WP-C2's GNOME Shell extension, held to the same assertions as the Python
# fake: `gjs` runs the extension's own D-Bus layer (lib/service.js) over a
# fixture in place of Mutter, and the C backend talks to it for real. This is
# the closest thing to a live GNOME Shell available without a session.
find_program(GJS_PROGRAM gjs)
set(GNOME_EXTENSION_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../gnome-extension)
if(GJS_PROGRAM)
    add_test(NAME window_gnome_extension
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/test_gnome_fake.sh
                     $<TARGET_FILE:vs-window>)
    set_tests_properties(window_gnome_extension PROPERTIES
        ENVIRONMENT "VS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR};VS_PYTHON=${PYTHON3_PROGRAM};VS_BRIDGE_CMD=${GJS_PROGRAM} -m ${GNOME_EXTENSION_DIR}/test/run_bridge.js;VS_BRIDGE_LABEL=the vorssaint-bridge extension under gjs"
        TIMEOUT 240)

    # Pure logic of the extension: interface XML, encoding, throttling, ids.
    add_test(NAME gnome_extension_units
             COMMAND ${GJS_PROGRAM} -m ${GNOME_EXTENSION_DIR}/test/test_units.js)

    # Every Meta/Shell member the extension calls, against whatever GNOME
    # introspection data this machine has. Skips itself where there is none.
    add_test(NAME gnome_extension_api
             COMMAND ${GJS_PROGRAM} -m ${GNOME_EXTENSION_DIR}/test/check_mutter_api.js)

    # ESM parse of every file (including the two that cannot run outside
    # gnome-shell), `gnome-extensions pack`, and the no-eval/no-subprocess rule
    # SECURITY.md promises.
    add_test(NAME gnome_extension_static
             COMMAND ${BASH_PROGRAM} ${GNOME_EXTENSION_DIR}/test/check_static.sh)

    # The clipboard members (WP-A8) over a real bus, with gdbus as the client:
    # the C side does not speak them yet.
    add_test(NAME gnome_extension_clipboard
             COMMAND ${BASH_PROGRAM} ${GNOME_EXTENSION_DIR}/test/test_clipboard_dbus.sh)

    set_tests_properties(window_gnome_extension gnome_extension_units gnome_extension_api
                         gnome_extension_static gnome_extension_clipboard
                         PROPERTIES SKIP_RETURN_CODE 77)
endif()
