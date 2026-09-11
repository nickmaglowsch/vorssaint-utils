cmake_minimum_required(VERSION 3.16)
project(wp02_capture C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  glib-2.0
  gio-2.0
  gio-unix-2.0
  libpipewire-0.3
  libspa-0.2
  libavcodec
  libavformat
  libavutil
  libswscale
  libswresample
)

add_executable(capture capture.c)
target_link_libraries(capture PRIVATE PkgConfig::DEPS m)
target_compile_options(capture PRIVATE -Wall -Wextra -Wno-unused-parameter)
