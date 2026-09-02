add_executable(ScheduleTest ScheduleTest.cpp ${TIMEBOARD_SRC}/model/Schedule.cpp)
target_include_directories(ScheduleTest PRIVATE ${TIMEBOARD_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(ScheduleTest PRIVATE -Wall -Wextra)
add_test(NAME ScheduleTest COMMAND ScheduleTest)
