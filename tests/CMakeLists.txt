cmake_minimum_required(VERSION 3.1.3...3.25 FATAL_ERROR)

add_executable(test_hidapi_input_ring test_hidapi_input_ring.c)
target_include_directories(test_hidapi_input_ring PRIVATE "${PROJECT_SOURCE_DIR}/core")
add_test(NAME hidapi_input_ring COMMAND test_hidapi_input_ring)
