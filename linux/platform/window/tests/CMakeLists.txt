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
