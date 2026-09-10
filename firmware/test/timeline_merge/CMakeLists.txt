add_executable(TimelineMergeTest TimelineMergeTest.cpp)
target_include_directories(TimelineMergeTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(TimelineMergeTest PRIVATE -Wall -Wextra)
add_test(NAME TimelineMergeTest COMMAND TimelineMergeTest)
