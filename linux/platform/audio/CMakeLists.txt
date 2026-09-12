cmake_minimum_required(VERSION 3.16)
project(vorssaint-audio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Strict C11 with the GNU surface named rather than inherited from the gnu11
# default: the backend uses clock_gettime and strdup, and saying so makes it a
# decision instead of an accident. Same reasoning as linux/helper.
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_GNU_SOURCE)

# This library parses names, icon hints and media titles that arrive from every
# other application on the machine, so the warning set is a gate.
add_compile_options(-Wall -Wextra -Wshadow -Wformat=2 -Werror)

# A bare `cmake -S . -B build` must produce what ships. Without this, "no build
# type" is a fourth configuration that nothing is ever checked against --
# _FORTIFY_SOURCE is inert without optimisation and GCC's truncation and
# maybe-uninitialized analyses see different things at each -O level.
# scripts/build-matrix.sh builds all four for exactly that reason.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Build type; RelWithDebInfo is what is installed" FORCE)
  message(STATUS "No CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(WARNING "_FORTIFY_SOURCE has no effect in a Debug build; "
                  "use RelWithDebInfo for a build that hardens")
endif()
add_compile_options(-D_FORTIFY_SOURCE=2 -fstack-protector-strong)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

add_library(vsaudio STATIC
  src/vs_audio.c
  src/vs_audio_pw.c
  src/vs_audio_pulse.c
)
target_include_directories(vsaudio PUBLIC ../include src)
target_link_libraries(vsaudio
  PUBLIC PkgConfig::PIPEWIRE PkgConfig::PULSE Threads::Threads m)

add_executable(vs-audio tools/vs_audio_cli.c)
target_link_libraries(vs-audio PRIVATE vsaudio)

# --- tests ------------------------------------------------------------------
# Every test needs a live audio server. scripts/run-stack.sh provides one; a
# test that cannot find one exits 77, which ctest reports as SKIPPED rather
# than as a pass, so a machine without PipeWire cannot look green by accident.

enable_testing()

add_executable(vs-audio-test tests/test_audio.c)
target_link_libraries(vs-audio-test PRIVATE vsaudio)

add_executable(vs-audio-unit tests/test_units.c)
target_link_libraries(vs-audio-unit PRIVATE vsaudio)

# The unit test touches no server at all: scale conversion, clamping and the
# node-array builder. It must pass everywhere, which is what makes it the one
# test that is never allowed to skip.
add_test(NAME units COMMAND vs-audio-unit)

set(VS_AUDIO_STACK_TESTS
  registry          # 1. enumeration, props, defaults
  events            # 1. add/remove/param change, debounced
  volume            # 2. per-stream volume and mute, cubic/linear, 150 %
  routing           # 3. target.object and the links that follow it
  default_sink      # 4. default switching and headphone-disconnect
  mute_inputs       # 5. mute every input and restore
  pulse_fallback    # 6. the libpulse path against pipewire-pulse
  capabilities      # 7. capability flags
)
foreach(case ${VS_AUDIO_STACK_TESTS})
  add_test(NAME ${case}
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-test.sh
                   $<TARGET_FILE:vs-audio-test> ${case})
  set_tests_properties(${case} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endforeach()
