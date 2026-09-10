add_executable(CategoryStyleTest CategoryStyleTest.cpp ${FIRMWARE_SRC}/ui/CategoryStyle.cpp)
target_include_directories(CategoryStyleTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(CategoryStyleTest PRIVATE -Wall -Wextra)
add_test(
  NAME CategoryStyleTest
  COMMAND CategoryStyleTest "${CMAKE_CURRENT_SOURCE_DIR}/../../freeink-sdk/libs/assets/Icons/lucide/icons"
)
