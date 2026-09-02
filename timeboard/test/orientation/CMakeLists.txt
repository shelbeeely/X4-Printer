add_executable(OrientationTest OrientationTest.cpp)
target_include_directories(OrientationTest PRIVATE ${TIMEBOARD_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(OrientationTest PRIVATE -Wall -Wextra)
add_test(NAME OrientationTest COMMAND OrientationTest)
