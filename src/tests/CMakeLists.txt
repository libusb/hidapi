# Backend-generic HIDAPI (unit-)tests, run against a virtual HID device.
#
# The test scenarios (test_read_interrupt.c, ...) are written against the
# public HIDAPI API and the backend-agnostic test_virtual_device interface, so
# the same scenarios can be reused for any backend once a virtual-device
# provider exists for it.
#
# Today the only provider is Linux uhid (test_virtual_device_uhid.c), which
# exercises the hidraw backend. On any other platform/backend there is no
# provider yet, so nothing is built here.
#
# The tests are registered with CTest and return CTest's SKIP code (77) at
# runtime when no virtual device can be created (e.g. the 'uhid' module isn't
# loaded or there are insufficient privileges), so they never turn into
# spurious failures on such hosts.

set(HIDAPI_TEST_VDEV_PROVIDER)
set(HIDAPI_TEST_BACKEND)

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_hidraw)
    set(HIDAPI_TEST_VDEV_PROVIDER test_virtual_device_uhid.c)
    set(HIDAPI_TEST_BACKEND hidapi_hidraw)
endif()

if(NOT HIDAPI_TEST_VDEV_PROVIDER)
    # No virtual-device provider for this platform/backend (yet).
    return()
endif()

find_package(Threads REQUIRED)

add_executable(test_read_interrupt
    test_read_interrupt.c
    ${HIDAPI_TEST_VDEV_PROVIDER}
)
set_target_properties(test_read_interrupt
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
)
target_link_libraries(test_read_interrupt PRIVATE ${HIDAPI_TEST_BACKEND} Threads::Threads)

add_test(NAME ReadInterrupt COMMAND test_read_interrupt)
set_tests_properties(ReadInterrupt
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 60
)
