cmake_minimum_required(VERSION 3.16)
project(vorssaint-gtk-spike LANGUAGES C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKDEPS REQUIRED IMPORTED_TARGET gtk4 libadwaita-1 json-glib-1.0 gio-2.0)

# The bridge stub stands in for the Swift CoreBridge; same C ABI either way.
add_library(corebridge STATIC ../corebridge-stub/corebridge.c)
target_include_directories(corebridge PUBLIC ../corebridge-stub)
target_link_libraries(corebridge PUBLIC m pthread)

add_executable(vorssaint-gtk-spike
    main.c core_model.c panel.c preferences.c overlay.c sni.c)
target_link_libraries(vorssaint-gtk-spike PRIVATE corebridge PkgConfig::GTKDEPS)
