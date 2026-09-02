# Host-side unit tests for TimeBoard's freestanding (no Arduino/M5Unified
# dependency) logic modules — model::Schedule, pomodoro::PomodoroTimer,
# orientation::toggle. Build & run:
#
#   cd timeboard/test && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
#
# Same zero-external-deps, hand-rolled-CHECK-macro style as
# ../../firmware/test/CMakeLists.txt — this is a separate CMake project,
# not a subdirectory of it, since timeboard/ is an independent
# device/firmware from the main X4-Printer one it lives alongside.

cmake_minimum_required(VERSION 3.16)
project(timeboard_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TIMEBOARD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

enable_testing()

add_subdirectory(schedule)
add_subdirectory(pomodoro)
add_subdirectory(orientation)
