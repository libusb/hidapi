# Backend-generic HIDAPI (unit-)tests, run against a virtual HID device.
#
# The tests are written against the public HIDAPI API and the backend-agnostic
# test_virtual_device interface (test_virtual_device.h), so the same test runs
# against any backend for which a virtual-device provider exists. Each provider
# implements the pre-recorded "scenario" protocol: the test triggers a scenario
# by sending a Feature report, and the device replays a canned input report.
#
# Providers:
#   - Linux   / hidraw : test_virtual_device_uhid.c      (kernel /dev/uhid)
#   - Linux   / libusb : test_virtual_device_rawgadget.c (/dev/raw-gadget + dummy_hcd)
#   - Windows / winapi : test_virtual_device_win.c       (modified vhidmini2 UMDF2 driver)
#   - macOS   / darwin : test_virtual_device_mac.c       (IOHIDUserDevice)
#
# The libusb (raw-gadget), Windows and macOS virtual devices need privileged
# out-of-band setup (kernel modules / a signed driver / an entitlement) that is
# only performed by the dedicated CI jobs. Whenever the virtual device cannot be
# created or does not enumerate, the test returns CTest's SKIP code (77) instead
# of failing, so ordinary builds on any host stay green.

find_package(Threads REQUIRED)

# Define a device-I/O test <name> built from test_device_io.c + <provider>,
# linked against the HIDAPI <backend>, and registered with CTest (it self-skips
# via return code 77 when no virtual device is present).
function(hidapi_add_vdev_test name provider backend)
    add_executable(${name} test_device_io.c ${provider})
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
    )
    target_link_libraries(${name} PRIVATE ${backend} Threads::Threads)
    if(HIDAPI_ENABLE_ASAN AND NOT MSVC)
        target_link_options(${name} PRIVATE -fsanitize=address)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 60
    )
endfunction()

# --- Linux: hidraw backend via /dev/uhid -----------------------------------
if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_hidraw)
    hidapi_add_vdev_test(DeviceIO_hidraw test_virtual_device_uhid.c hidapi_hidraw)
endif()

# --- Linux: libusb backend via /dev/raw-gadget (+ dummy_hcd) ----------------
# Built whenever the libusb backend is, so it keeps compiling; at runtime it
# self-skips unless the raw-gadget virtual device has been set up (CI job).
if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_libusb)
    hidapi_add_vdev_test(DeviceIO_libusb test_virtual_device_rawgadget.c hidapi_libusb)
endif()

# --- Windows: winapi backend via a modified vhidmini2 UMDF driver -----------
if(WIN32 AND TARGET hidapi_winapi)
    hidapi_add_vdev_test(DeviceIO_winapi test_virtual_device_win.c hidapi_winapi)
    # HidD_GetPreparsedData / HidP_GetCaps used by the Windows provider.
    target_link_libraries(DeviceIO_winapi PRIVATE hid)
    # Run from the directory holding the hidapi DLL so a shared build can find
    # it at launch (there is no rpath on Windows).
    set_tests_properties(DeviceIO_winapi PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:hidapi_winapi>")
    # With ASan (MSVC) the test exe needs the ASan runtime DLL, which lives next
    # to the MSVC tools; add it to PATH (CMake >= 3.22).
    if(HIDAPI_ENABLE_ASAN AND MSVC AND NOT CMAKE_VERSION VERSION_LESS "3.22")
        get_filename_component(MSVC_BUILD_TOOLS_DIR "${CMAKE_LINKER}" DIRECTORY)
        set_property(TEST DeviceIO_winapi PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_append:${MSVC_BUILD_TOOLS_DIR}")
    endif()
endif()

# --- macOS: IOKit backend via IOHIDUserDevice ------------------------------
# Self-skips where the virtual-device entitlement / user consent isn't
# available (e.g. hosted CI runners); usable locally / on a self-hosted Mac.
if(APPLE AND TARGET hidapi_darwin)
    hidapi_add_vdev_test(DeviceIO_darwin test_virtual_device_mac.c hidapi_darwin)
    target_link_libraries(DeviceIO_darwin PRIVATE
        "-framework IOKit" "-framework CoreFoundation")
endif()
