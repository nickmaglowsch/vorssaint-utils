# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Vorssaint
#
# The Linux app shell (WP-20 slice). Promoted from `spikes/wp01-toolkit/
# qt-quick` by `git mv`, so the three screens, `CoreModel` and the tray item
# are the ones WP-01 measured; what changed is the name, the icon, the
# `.desktop` file, the CLI flags, the real CPU sampler and the default bridge.
#
#   cmake -S linux/shell -B build -DCMAKE_BUILD_TYPE=Release \
#         -DVORSSAINT_BRIDGE_LIB=/path/to/libvorssaintbridge.a \
#         -DVORSSAINT_SWIFT_RUNTIME_FLAGS="-L/usr/lib/swift/linux -lswiftCore ..."
#
# The bridge library is the archive `docs/linux-port/BRIDGE.md` describes and
# the `shell-appimage` job in `.github/workflows/linux-port-ci.yml` builds.
cmake_minimum_required(VERSION 3.16)
project(vorssaint-shell LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
# Qt added qt_standard_project_setup() in 6.3 and Ubuntu 22.04 -- the oldest
# glibc the port builds on (WP-04) -- ships 6.2.4. It only sets AUTOMOC/AUTOUIC
# and a QML output directory, both of which this file sets itself.
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.3)
    qt_standard_project_setup()
endif()

# The bridge: the real Swift CoreBridge (WP-18) by default, the WP-01 C stub
# only when explicitly asked for. The spike defaulted the other way round
# because no Swift toolchain existed on the machine it was measured on; the
# product does not ship a stub by accident.
option(VORSSAINT_REAL_BRIDGE "Link the real Swift CoreBridge instead of the C stub" ON)
set(VORSSAINT_BRIDGE_LIB "" CACHE FILEPATH
    "Static library exporting vs_subscribe/vs_command/vs_snapshot/vs_free")
set(VORSSAINT_SWIFT_RUNTIME_FLAGS "" CACHE STRING
    "Link flags swift-autolink-extract produced for that archive")
set(VORSSAINT_BRIDGE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
    CACHE PATH "Directory holding corebridge.h")

if(VORSSAINT_REAL_BRIDGE)
    if(NOT VORSSAINT_BRIDGE_LIB)
        message(FATAL_ERROR
            "VORSSAINT_REAL_BRIDGE=ON needs -DVORSSAINT_BRIDGE_LIB=<libvorssaintbridge.a>. "
            "Pass -DVORSSAINT_REAL_BRIDGE=OFF to build against the WP-01 C stub instead.")
    endif()
    # Swift's runtime is what the archive needs behind it; swiftc is the only
    # thing that knows where it lives, so the caller passes the flags it
    # printed rather than this file guessing at a path. Those flags must
    # include `swiftrt.o` as well as the `-l` flags: it is what registers the
    # executable's protocol-conformance sections with the Swift runtime, and
    # a link that leaves it out crashes in `swift_getWitnessTable` on the
    # first dynamic conformance lookup rather than failing to link.
    add_library(corebridge INTERFACE)
    target_include_directories(corebridge INTERFACE "${VORSSAINT_BRIDGE_HEADER_DIR}")
    separate_arguments(VORSSAINT_SWIFT_RUNTIME_FLAGS_LIST NATIVE_COMMAND
                       "${VORSSAINT_SWIFT_RUNTIME_FLAGS}")
    target_link_libraries(corebridge INTERFACE
        "${VORSSAINT_BRIDGE_LIB}" ${VORSSAINT_SWIFT_RUNTIME_FLAGS_LIST} m pthread)
    message(STATUS "corebridge: real Swift bridge ${VORSSAINT_BRIDGE_LIB}")
else()
    add_library(corebridge STATIC ../../spikes/wp01-toolkit/corebridge-stub/corebridge.c)
    target_include_directories(corebridge PUBLIC ../../spikes/wp01-toolkit/corebridge-stub)
    target_link_libraries(corebridge PUBLIC m pthread)
    message(STATUS "corebridge: WP-01 C stub (synthetic data)")
endif()

# ---------------------------------------------------------------------------
# The CPU sampler. `linux/platform/sensors` is the port's one reader of
# /proc/stat (PLATFORM.md 4: never a second reader of the same hardware), so
# the shell builds that library rather than opening /proc itself. It is built
# here directly instead of `add_subdirectory(../platform)` because that tree
# also builds the capture and audio backends, which need PipeWire and the
# wlroots protocols and have nothing to do with a panel.
# ---------------------------------------------------------------------------
option(VORSSAINT_SENSORS "Sample real CPU through linux/platform/sensors" ON)
if(VORSSAINT_SENSORS)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDBUS IMPORTED_TARGET libsystemd)
    endif()
    if(NOT SDBUS_FOUND)
        message(WARNING "libsystemd not found: building without the CPU sampler. "
                        "The panel will say its data is a placeholder.")
        set(VORSSAINT_SENSORS OFF)
    endif()
endif()

if(VORSSAINT_SENSORS)
    set(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../platform")
    add_library(vorssaint_platform_headers INTERFACE)
    target_include_directories(vorssaint_platform_headers INTERFACE "${PLATFORM_DIR}/include")
    add_library(vs_platform_common STATIC "${PLATFORM_DIR}/vs_result.c")
    target_link_libraries(vs_platform_common PUBLIC vorssaint_platform_headers)
    add_subdirectory("${PLATFORM_DIR}/sensors" "${CMAKE_CURRENT_BINARY_DIR}/sensors")
    message(STATUS "sensors: real /proc/stat sampler via vs_sensors")
endif()

qt_add_executable(vorssaint main.cpp CoreModel.cpp MetricsSampler.cpp qml.qrc)
target_link_libraries(vorssaint PRIVATE
    corebridge Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets)
if(NOT VORSSAINT_REAL_BRIDGE)
    # The panel has to be able to say so: against the stub, the series it
    # draws is the stub's generated one and no reading the sampler takes ever
    # reaches it.
    target_compile_definitions(vorssaint PRIVATE VORSSAINT_STUB_BRIDGE=1)
endif()
if(VORSSAINT_SENSORS)
    target_link_libraries(vorssaint PRIVATE vs_sensors)
    target_compile_definitions(vorssaint PRIVATE VORSSAINT_HAVE_SENSORS=1)
endif()

include(GNUInstallDirs)
install(TARGETS vorssaint RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/com.vorssaint.VorssaintLinux.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/com.vorssaint.VorssaintLinux.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
