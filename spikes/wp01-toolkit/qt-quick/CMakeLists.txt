cmake_minimum_required(VERSION 3.16)
project(vorssaint-qt-spike LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
qt_standard_project_setup()

# The bridge: the WP-01 C stub by default, or the real Swift CoreBridge
# (WP-18) when pointed at a static library built from VorssaintLinux.
#
#   cmake -B build -DVORSSAINT_REAL_BRIDGE=ON \
#         -DVORSSAINT_BRIDGE_LIB=/path/to/libvorssaintbridge.a
#
# Not one line of CoreModel.cpp or of the QML changes between the two, which
# is the whole claim of PLAN.md 4.2 and the reason this option is one `if`.
# How the archive is produced is in docs/linux-port/BRIDGE.md; the `bridge-abi`
# job in .github/workflows/linux-port-ci.yml is what actually builds it,
# because no Swift toolchain exists on the machines this spike was measured on.
option(VORSSAINT_REAL_BRIDGE "Link the real Swift CoreBridge instead of the C stub" OFF)
set(VORSSAINT_BRIDGE_LIB "" CACHE FILEPATH
    "Static library exporting vs_subscribe/vs_command/vs_snapshot/vs_free")
set(VORSSAINT_BRIDGE_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../linux/shell/include"
    CACHE PATH "Directory holding the real corebridge.h")

if(VORSSAINT_REAL_BRIDGE)
    if(NOT VORSSAINT_BRIDGE_LIB)
        message(FATAL_ERROR
            "VORSSAINT_REAL_BRIDGE=ON needs -DVORSSAINT_BRIDGE_LIB=<libvorssaintbridge.a>")
    endif()
    # Swift's runtime is what the archive needs behind it; swiftc is the only
    # thing that knows where it lives, so the caller passes the flags it
    # printed rather than this file guessing at a path.
    add_library(corebridge INTERFACE)
    target_include_directories(corebridge INTERFACE "${VORSSAINT_BRIDGE_HEADER_DIR}")
    target_link_libraries(corebridge INTERFACE
        "${VORSSAINT_BRIDGE_LIB}" ${VORSSAINT_SWIFT_RUNTIME_FLAGS} m pthread)
    message(STATUS "corebridge: real Swift bridge ${VORSSAINT_BRIDGE_LIB}")
else()
    add_library(corebridge STATIC ../corebridge-stub/corebridge.c)
    target_include_directories(corebridge PUBLIC ../corebridge-stub)
    target_link_libraries(corebridge PUBLIC m pthread)
    message(STATUS "corebridge: WP-01 C stub")
endif()

qt_add_executable(vorssaint-qt-spike main.cpp CoreModel.cpp qml.qrc)
target_link_libraries(vorssaint-qt-spike PRIVATE
    corebridge Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets)
