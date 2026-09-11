cmake_minimum_required(VERSION 3.16)
project(vorssaint-qt-spike LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
qt_standard_project_setup()

# The bridge stub stands in for the Swift CoreBridge; same C ABI either way.
add_library(corebridge STATIC ../corebridge-stub/corebridge.c)
target_include_directories(corebridge PUBLIC ../corebridge-stub)
target_link_libraries(corebridge PUBLIC m pthread)

qt_add_executable(vorssaint-qt-spike main.cpp CoreModel.cpp qml.qrc)
target_link_libraries(vorssaint-qt-spike PRIVATE
    corebridge Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets)
