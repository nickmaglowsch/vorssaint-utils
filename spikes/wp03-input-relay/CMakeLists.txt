cmake_minimum_required(VERSION 3.16)
project(vorssaint-input-relay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wshadow -Wformat=2 -Werror)

# The helper runs as root and reads a string from an unprivileged caller.
add_compile_options(-D_FORTIFY_SOURCE=2 -fstack-protector-strong)
add_link_options(-Wl,-z,relro -Wl,-z,now)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)

# WITH_POLKIT is ON for anything that will be installed. It is switched off
# only for the private-bus harness, where no polkit authority is running.
option(WITH_POLKIT "Link the real polkit authority check" ON)
if(WITH_POLKIT)
  pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-gobject-1)
endif()

add_library(relaycore STATIC
  src/rules.c
  src/device_fake.c
  src/device_evdev.c
)
target_include_directories(relaycore PUBLIC src)
target_link_libraries(relaycore PUBLIC PkgConfig::EVDEV PkgConfig::UDEV)

add_library(authz STATIC src/polkit_check.c)
target_include_directories(authz PUBLIC src)
if(WITH_POLKIT)
  target_compile_definitions(authz PUBLIC WITH_POLKIT=1)
  target_link_libraries(authz PUBLIC PkgConfig::POLKIT)
endif()

add_executable(vorssaint-relay src/relay.c)
target_link_libraries(vorssaint-relay PRIVATE relaycore)

add_executable(vorssaint-helper src/helper.c)
target_link_libraries(vorssaint-helper PRIVATE relaycore authz PkgConfig::SDBUS Threads::Threads)

add_executable(vorssaint-relayctl src/client.c)
target_link_libraries(vorssaint-relayctl PRIVATE PkgConfig::SDBUS)

add_executable(xkb-layout-demo src/xkb_demo.c)
target_link_libraries(xkb-layout-demo PRIVATE PkgConfig::XKB)

add_executable(gen-stream src/gen_stream.c)

enable_testing()
add_executable(test_rules tests/test_rules.c)
target_link_libraries(test_rules PRIVATE relaycore)
add_test(NAME rules COMMAND test_rules)

# The replay test is the rules proven against a recorded evdev stream rather
# than against the unit harness, so a regression in the device layer shows up
# too.
add_test(NAME replay
         COMMAND ${CMAKE_COMMAND}
                 -DGEN=$<TARGET_FILE:gen-stream>
                 -DRELAY=$<TARGET_FILE:vorssaint-relay>
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_expected.txt
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_replay.cmake)

install(TARGETS vorssaint-helper RUNTIME DESTINATION libexec)
install(FILES data/org.vorssaint.helper.policy DESTINATION share/polkit-1/actions)
install(FILES data/org.vorssaint.Helper1.conf DESTINATION share/dbus-1/system.d)
install(FILES data/org.vorssaint.Helper1.service DESTINATION share/dbus-1/system-services)
install(FILES data/vorssaint-helper.service DESTINATION lib/systemd/system)
install(FILES data/70-vorssaint-uinput.rules DESTINATION lib/udev/rules.d)
