cmake_minimum_required(VERSION 3.16)
project(vorssaint-helper C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Strict C11 with the POSIX/GNU surface declared explicitly, rather than the
# gnu11 default: the daemon uses clock_gettime, strtok_r, readlink and fork,
# and naming _GNU_SOURCE makes that a decision instead of an accident.
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_GNU_SOURCE)

# The helper runs as root and parses strings handed to it by an unprivileged
# caller, so the warning set is a gate, not advice.
add_compile_options(-Wall -Wextra -Wshadow -Wformat=2 -Werror)

# _FORTIFY_SOURCE needs an optimising build to do anything; saying so beats
# silently shipping a hardening flag that is a no-op.
if(CMAKE_C_FLAGS MATCHES "-O0" OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(WARNING "_FORTIFY_SOURCE has no effect without optimisation; "
                  "use -DCMAKE_BUILD_TYPE=RelWithDebInfo for a build that hardens")
endif()
add_compile_options(-D_FORTIFY_SOURCE=2 -fstack-protector-strong)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-fPIE)
add_link_options(-pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

# WITH_POLKIT is ON for anything that will be installed. It is switched off
# only for the private-bus harness, where no polkit authority is running, and
# the resulting binary announces itself in the Authorization property.
option(WITH_POLKIT "Link the real polkit authority check" ON)
if(WITH_POLKIT)
  pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-gobject-1)
endif()

# --- the daemon's libraries -------------------------------------------------

add_library(helpercore STATIC
  src/rules.c
  src/device_fake.c
  src/device_evdev.c
  src/grabholder.c
  src/session.c
  src/fan.c
  src/ddc.c
  src/caps.c
)
target_include_directories(helpercore PUBLIC src)
target_link_libraries(helpercore PUBLIC PkgConfig::EVDEV PkgConfig::UDEV PkgConfig::SDBUS)

add_library(authz STATIC src/polkit_check.c)
target_include_directories(authz PUBLIC src)
if(WITH_POLKIT)
  target_compile_definitions(authz PUBLIC WITH_POLKIT=1)
  target_link_libraries(authz PUBLIC PkgConfig::POLKIT)
endif()

# --- binaries ----------------------------------------------------------------

add_executable(vorssaint-helper src/helper.c)
target_link_libraries(vorssaint-helper PRIVATE helpercore authz PkgConfig::SDBUS Threads::Threads)

# The relay CLI is the developer's way to run the device layer without a bus:
# --replay, --bench and --tap all drive the same code the daemon runs.
add_executable(vorssaint-relay src/relay.c)
target_link_libraries(vorssaint-relay PRIVATE helpercore)

add_executable(gen-stream src/gen_stream.c)

# The client library the platform layer links.
add_library(vorssaint-helper-client STATIC client/vorssaint_helper_client.c)
target_include_directories(vorssaint-helper-client PUBLIC client)
target_link_libraries(vorssaint-helper-client PUBLIC PkgConfig::SDBUS)

add_executable(vorssaint-helperctl client/helperctl.c)
target_link_libraries(vorssaint-helperctl PRIVATE vorssaint-helper-client)

# --- tests -------------------------------------------------------------------

enable_testing()

add_executable(test_rules tests/test_rules.c)
target_link_libraries(test_rules PRIVATE helpercore)
add_test(NAME rules COMMAND test_rules)

add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE helpercore)
add_test(NAME session COMMAND test_session)

add_executable(test_fan tests/test_fan.c)
target_link_libraries(test_fan PRIVATE helpercore)
add_test(NAME fan COMMAND test_fan)

add_executable(test_ddc tests/test_ddc.c)
target_link_libraries(test_ddc PRIVATE helpercore)
add_test(NAME ddc COMMAND test_ddc)

add_executable(test_grabholder tests/test_grabholder.c)
target_link_libraries(test_grabholder PRIVATE helpercore)
add_test(NAME grabholder COMMAND test_grabholder)

add_executable(test_caps tests/test_caps.c)
target_link_libraries(test_caps PRIVATE helpercore)
add_test(NAME caps COMMAND test_caps)

# The rules proven against a recorded evdev stream rather than the unit
# harness, so a regression in the device layer shows up too.
add_test(NAME replay
         COMMAND ${CMAKE_COMMAND}
                 -DGEN=$<TARGET_FILE:gen-stream>
                 -DRELAY=$<TARGET_FILE:vorssaint-relay>
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_expected.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_replay.cmake)

# install.sh and uninstall.sh against a DESTDIR fake root: idempotence and
# complete removal, without touching this machine.
add_test(NAME install_script
         COMMAND ${CMAKE_COMMAND} -E env
                 HELPER_DIST=${CMAKE_CURRENT_SOURCE_DIR}/dist
                 HELPER_BIN=$<TARGET_FILE:vorssaint-helper>
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install.sh)

# --- installation ------------------------------------------------------------
#
# These are exactly the seven paths dist/install.sh writes, in the same
# prefix-relative form, so `make install` and the script cannot drift apart.
# vorssaint-helperctl is deliberately absent: it is the harness for the client
# library, not something a user's machine should carry.
include(GNUInstallDirs)
install(TARGETS vorssaint-helper RUNTIME DESTINATION libexec)
install(FILES dist/org.vorssaint.helper.policy DESTINATION share/polkit-1/actions)
install(FILES dist/org.vorssaint.Helper1.conf DESTINATION share/dbus-1/system.d)
install(FILES dist/org.vorssaint.Helper1.service DESTINATION share/dbus-1/system-services)
install(FILES dist/vorssaint-helper.service DESTINATION lib/systemd/system)
install(FILES dist/70-vorssaint-uinput.rules dist/71-vorssaint-i2c.rules
        DESTINATION lib/udev/rules.d)
