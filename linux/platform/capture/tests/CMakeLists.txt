# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

# Pure-logic tests: no compositor, no PipeWire, no bus. These run everywhere,
# including in every configuration of scripts/build-matrix.sh.
add_executable(test_capture_support test_capture_support.c)
target_link_libraries(test_capture_support PRIVATE vs_capture)
add_test(NAME capture_support COMMAND test_capture_support)

# Stack tests: they need a headless sway + PipeWire + xdg-desktop-portal
# session. Each skips with a message (ctest "Skipped" via the 77 return code)
# when the pieces are missing, rather than failing on a machine that was never
# going to have them.
set(STACK_TESTS
    enumerate      # AvailableSourceTypes, monitor list, honest WINDOW reporting
    frame          # one non-blank frame through both screenshot paths
    stream         # 5 s, >= 100 frames, monotonic timestamps, both APIs
    region         # engine-side crop
    audio          # system audio non-silent with a tone
    pause          # pause/resume keeps video and audio aligned
    token          # restore token round trip
)
foreach(name IN LISTS STACK_TESTS)
    add_test(NAME capture_stack_${name}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_stack_test.sh ${name}
                     $<TARGET_FILE:vs-capture>)
    set_tests_properties(capture_stack_${name} PROPERTIES
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
        TIMEOUT 300)
endforeach()
