# Backend-generic HIDAPI (unit-)tests, run against a virtual HID device.
#
# The test scenarios (test_read_interrupt.c, ...) are written against the
# public HIDAPI API and the backend-agnostic test_virtual_device interface, so
# the same scenarios can be reused for any backend once a virtual-device
# provider exists for it.
#
# Providers exist for Linux uhid (test_virtual_device_uhid.c -> hidraw) and
# Windows vhidmini2 (test_virtual_device_win.c -> winapi; the matching UMDF2
# driver under windows/driver must be built, signed and installed
# separately). On any other platform/backend there is no provider yet, so
# nothing is built here.
#
# The tests are registered with CTest and return CTest's SKIP code (77) at
# runtime when no virtual device can be created (e.g. the 'uhid' module isn't
# loaded, the Windows driver isn't installed, or there are insufficient
# privileges), so they never turn into spurious failures on such hosts.

set(HIDAPI_TEST_VDEV_PROVIDER)
set(HIDAPI_TEST_BACKEND)

if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_hidraw)
    set(HIDAPI_TEST_VDEV_PROVIDER test_virtual_device_uhid.c)
    set(HIDAPI_TEST_BACKEND hidapi_hidraw)
elseif(WIN32 AND TARGET hidapi_winapi)
    set(HIDAPI_TEST_VDEV_PROVIDER test_virtual_device_win.c)
    set(HIDAPI_TEST_BACKEND hidapi_winapi)
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
if(WIN32)
    # HidD_GetPreparsedData / HidP_GetCaps used by the Windows provider.
    target_link_libraries(test_read_interrupt PRIVATE hid)
endif()

add_test(NAME ReadInterrupt COMMAND test_read_interrupt)
set_tests_properties(ReadInterrupt
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 60
)
if(WIN32)
    # Run from the directory containing the hidapi DLL so a shared-library
    # build can locate it at launch (there is no rpath on Windows).
    set_tests_properties(ReadInterrupt PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${HIDAPI_TEST_BACKEND}>")

    # When built with ASan (MSVC), the test exe needs the ASan runtime DLL,
    # which lives next to the MSVC tools; add it to PATH (CMake >= 3.22).
    if(HIDAPI_ENABLE_ASAN AND MSVC AND NOT CMAKE_VERSION VERSION_LESS "3.22")
        get_filename_component(MSVC_BUILD_TOOLS_DIR "${CMAKE_LINKER}" DIRECTORY)
        set_property(TEST ReadInterrupt PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_append:${MSVC_BUILD_TOOLS_DIR}")
    endif()
endif()
