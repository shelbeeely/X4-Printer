add_executable(PlannerStoreTest PlannerStoreTest.cpp)
target_include_directories(PlannerStoreTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(PlannerStoreTest PRIVATE -Wall -Wextra)
add_test(NAME PlannerStoreTest COMMAND PlannerStoreTest)
