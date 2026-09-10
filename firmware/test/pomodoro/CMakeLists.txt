add_executable(PomodoroSessionTest PomodoroSessionTest.cpp)
target_include_directories(PomodoroSessionTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(PomodoroSessionTest PRIVATE -Wall -Wextra)
add_test(NAME PomodoroSessionTest COMMAND PomodoroSessionTest)
