add_executable(PomodoroTimerTest PomodoroTimerTest.cpp ${TIMEBOARD_SRC}/pomodoro/PomodoroTimer.cpp)
target_include_directories(PomodoroTimerTest PRIVATE ${TIMEBOARD_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(PomodoroTimerTest PRIVATE -Wall -Wextra)
add_test(NAME PomodoroTimerTest COMMAND PomodoroTimerTest)
