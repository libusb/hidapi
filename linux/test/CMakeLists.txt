add_executable(hidapi_hidraw_loader_test
    hidapi_hidraw_loader_test.c
)
target_link_libraries(hidapi_hidraw_loader_test PRIVATE hidapi_hidraw)
target_link_libraries(hidapi_hidraw_loader_test PRIVATE ${CMAKE_DL_LIBS})

add_library(hidapi_test_incomplete_udev SHARED
    incomplete_udev.c
)

add_library(hidapi_test_dlopen_interposer SHARED
    dlopen_interposer.c
)
target_link_libraries(hidapi_test_dlopen_interposer PRIVATE ${CMAKE_DL_LIBS})

add_dependencies(hidapi_hidraw_loader_test
    hidapi_test_incomplete_udev
    hidapi_test_dlopen_interposer
)

add_test(
    NAME hidapi-hidraw-loader-reload
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "ASAN_OPTIONS=verify_asan_link_order=0"
        "LD_PRELOAD=$<TARGET_FILE:hidapi_test_dlopen_interposer>"
        "HIDAPI_TEST_FAIL_ATTEMPTS=0"
        "$<TARGET_FILE:hidapi_hidraw_loader_test>" reload
)

add_test(
    NAME hidapi-hidraw-loader-failure-recovery
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "ASAN_OPTIONS=verify_asan_link_order=0"
        "LD_PRELOAD=$<TARGET_FILE:hidapi_test_dlopen_interposer>"
        "HIDAPI_TEST_FAKE_UDEV=$<TARGET_FILE:hidapi_test_incomplete_udev>"
        "HIDAPI_TEST_FAIL_ATTEMPTS=2"
        "$<TARGET_FILE:hidapi_hidraw_loader_test>" failure-recovery
)

set_tests_properties(
    hidapi-hidraw-loader-reload
    hidapi-hidraw-loader-failure-recovery
    PROPERTIES TIMEOUT 30
)
