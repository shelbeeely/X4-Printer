# Host-side unit tests for freestanding (no Arduino/FreeInk dependency)
# firmware logic. Build & run:
#
#   cd firmware/test && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
#
# Deliberately has zero external dependencies (no GoogleTest fetch) so it
# runs offline with just a C++17 compiler + CMake — see XtcFormatTest.cpp's
# header comment for why.

cmake_minimum_required(VERSION 3.16)
project(xteink_firmware_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FIRMWARE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

enable_testing()

add_executable(XtcFormatTest xtc_format/XtcFormatTest.cpp)
target_include_directories(XtcFormatTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(XtcFormatTest PRIVATE -Wall -Wextra)
add_test(
  NAME XtcFormatTest
  COMMAND XtcFormatTest "${CMAKE_CURRENT_SOURCE_DIR}/xtc_format/fixtures/sample.xtc"
)

add_subdirectory(job_store)
add_subdirectory(approval_outbox)
