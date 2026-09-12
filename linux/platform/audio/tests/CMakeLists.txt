# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint

find_program(BASH_PROGRAM bash REQUIRED)

# Pure-C unit tests: no audio server, no daemon, no bus. This is the one suite
# that is never allowed to skip, so a machine with no PipeWire still checks the
# scale conversion, the clamping and the event queue.
add_executable(test_audio_support test_audio_support.c)
target_link_libraries(test_audio_support PRIVATE vs_audio)
target_include_directories(test_audio_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME audio_support COMMAND test_audio_support)

# Everything else needs a live audio server. scripts/run-stack.sh builds a
# private one (its own dbus-daemon, pipewire, wireplumber and pipewire-pulse,
# with null sinks standing in for hardware); a machine without those exits 77,
# which ctest reports as "not run" rather than as a pass.
add_executable(test_audio_live test_audio_live.c)
target_link_libraries(test_audio_live PRIVATE vs_audio)
target_include_directories(test_audio_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(VS_AUDIO_LIVE_CASES
    registry        # 1. enumeration, props, defaults
    events          # 1. add/remove/param change, debounced, dispatch-only
    volume          # 2. per-stream volume and mute, cubic/linear, 150 %
    routing         # 3. target.object, and the links that follow it
    default_sink    # 4. default switching and headphone-disconnect
    mute_inputs     # 5. mute every input and restore
    pulse_fallback  # 6. the libpulse path against pipewire-pulse
    capabilities)   # 7. capability flags

foreach(case ${VS_AUDIO_LIVE_CASES})
    add_test(NAME audio_${case}
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/run-live-test.sh
                     $<TARGET_FILE:test_audio_live> ${case})
    set_tests_properties(audio_${case} PROPERTIES
        ENVIRONMENT "VS_AUDIO_SCRIPTS=${CMAKE_CURRENT_SOURCE_DIR}/../scripts"
        SKIP_RETURN_CODE 77
        TIMEOUT 180)
endforeach()
