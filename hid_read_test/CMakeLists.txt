cmake_minimum_required(VERSION 3.8...3.25 FATAL_ERROR)
project(hid_read_test CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # built as a standalone project
    if(POLICY CMP0074)
        cmake_policy(SET CMP0074 NEW)
    endif()

    find_package(hidapi 0.16 REQUIRED)
    message(STATUS "Using HIDAPI: ${hidapi_VERSION}")
else()
    message(STATUS "Building hid_read_test")
endif()

find_package(Threads REQUIRED)

set(HIDAPI_HID_READ_TEST_TARGETS)
if(NOT WIN32 AND NOT APPLE AND CMAKE_SYSTEM_NAME MATCHES "Linux")
    if(TARGET hidapi::hidraw)
        add_executable(hid_read_test_hidraw main.cpp)
        target_compile_features(hid_read_test_hidraw PRIVATE cxx_std_11)
        target_link_libraries(hid_read_test_hidraw PRIVATE hidapi::hidraw Threads::Threads)
        list(APPEND HIDAPI_HID_READ_TEST_TARGETS hid_read_test_hidraw)
    endif()
    if(TARGET hidapi::libusb)
        add_executable(hid_read_test_libusb main.cpp)
        target_compile_features(hid_read_test_libusb PRIVATE cxx_std_11)
        target_compile_definitions(hid_read_test_libusb PRIVATE USING_HIDAPI_LIBUSB)
        target_link_libraries(hid_read_test_libusb PRIVATE hidapi::libusb Threads::Threads)
        list(APPEND HIDAPI_HID_READ_TEST_TARGETS hid_read_test_libusb)
    endif()
else()
    add_executable(hid_read_test main.cpp)
    target_compile_features(hid_read_test PRIVATE cxx_std_11)
    target_link_libraries(hid_read_test PRIVATE hidapi::hidapi Threads::Threads)
    list(APPEND HIDAPI_HID_READ_TEST_TARGETS hid_read_test)
endif()

install(TARGETS ${HIDAPI_HID_READ_TEST_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
