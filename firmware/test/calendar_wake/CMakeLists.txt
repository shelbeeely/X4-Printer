add_executable(WakeScheduleTest
  WakeScheduleTest.cpp
  ${FIRMWARE_SRC}/calendar/WakeSchedule.cpp
)
target_include_directories(WakeScheduleTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(WakeScheduleTest PRIVATE -Wall -Wextra)
add_test(NAME WakeScheduleTest COMMAND WakeScheduleTest)
