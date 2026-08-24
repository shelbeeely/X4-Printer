add_executable(JobStoreTest JobStoreTest.cpp)
target_include_directories(JobStoreTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(JobStoreTest PRIVATE -Wall -Wextra)
add_test(NAME JobStoreTest COMMAND JobStoreTest)
