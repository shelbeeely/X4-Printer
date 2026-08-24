add_executable(ApprovalOutboxTest ApprovalOutboxTest.cpp)
target_include_directories(ApprovalOutboxTest PRIVATE ${FIRMWARE_SRC} ${CMAKE_SOURCE_DIR})
target_compile_options(ApprovalOutboxTest PRIVATE -Wall -Wextra)
add_test(NAME ApprovalOutboxTest COMMAND ApprovalOutboxTest)
