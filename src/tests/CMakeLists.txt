# Backend-generic device-I/O tests and macOS hotplug lifecycle tests.
#
# The device-I/O tests use the public HIDAPI API and the backend-agnostic
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
# The libusb (raw-gadget) and Windows virtual devices need privileged out-of-band
# setup (kernel modules / a signed driver) that only the dedicated label-gated CI
# jobs perform; the macOS one needs an entitlement plus interactive consent and
# therefore runs only on a developer's entitled Mac. Whenever the virtual device
# cannot be created or does not enumerate, the test returns CTest's SKIP code
# (77) instead of failing, so ordinary builds on any host stay green.

# SKIP_RETURN_CODE (used to report a provider self-skip as SKIP rather than
# FAIL) needs CMake/CTest 3.16. With an older CMake a legitimate self-skip
# would be mis-reported as a test failure, so don't add the tests at all.
if(CMAKE_VERSION VERSION_LESS "3.16")
    message(WARNING "HIDAPI_WITH_TESTS: the virtual-device tests require CMake >= 3.16 (SKIP_RETURN_CODE); not adding them.")
    return()
endif()

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

# Define a tier-1 hotplug-API test <name> built from test_hotplug_api.c only
# (NO virtual-device provider: it needs no device and no privileges, so it runs
# in the ordinary CI matrix), linked against the HIDAPI <backend>. It
# retries a failed first registration after explicit hid_init(). It self-skips
# (77) only when that retry also fails (including a backend that reports hotplug
# as unsupported, e.g. libusb without LIBUSB_CAP_HAS_HOTPLUG).
function(hidapi_add_hotplug_api_test name backend)
    add_executable(${name} test_hotplug_api.c)
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
        TIMEOUT 120
    )
endfunction()

# Define a tier-2 (device-backed) hotplug test <name> built from
# test_hotplug.c + <provider>. Self-skips when no virtual device can be
# created here, when the backend reports hotplug as unsupported, or when the
# provider cannot toggle device presence (test_virtual_device_unplug/_replug).
# The uhid, rawgadget and Windows vhidmini providers all implement presence
# toggling; the darwin provider does not yet.
# <event_timeout_ms> is the per-event wait budget inside the test;
# <ctest_timeout> is a finite watchdog for a backend hang. The test reports a
# failure after its first event deadline, then safely cleans up and summarizes.
function(hidapi_add_hotplug_test name provider backend event_timeout_ms ctest_timeout)
    add_executable(${name} test_hotplug.c ${provider})
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
    )
    target_compile_definitions(${name} PRIVATE
        TEST_HOTPLUG_EVENT_TIMEOUT_MS=${event_timeout_ms})
    target_link_libraries(${name} PRIVATE ${backend} Threads::Threads)
    if(HIDAPI_ENABLE_ASAN AND NOT MSVC)
        target_link_options(${name} PRIVATE -fsanitize=address)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT ${ctest_timeout}
    )
endfunction()

# --- Linux: hidraw backend via /dev/uhid -----------------------------------
if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_hidraw)
    hidapi_add_vdev_test(DeviceIO_hidraw test_virtual_device_uhid.c hidapi_hidraw)
    hidapi_add_hotplug_api_test(HotplugAPI_hidraw hidapi_hidraw)
    hidapi_add_hotplug_test(Hotplug_hidraw test_virtual_device_uhid.c hidapi_hidraw 10000 300)
endif()

# --- Linux: libusb backend via /dev/raw-gadget (+ dummy_hcd) ----------------
# Built whenever the libusb backend is, so it keeps compiling; at runtime it
# self-skips unless the raw-gadget virtual device has been set up (CI job).
if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND TARGET hidapi_libusb)
    hidapi_add_vdev_test(DeviceIO_libusb test_virtual_device_rawgadget.c hidapi_libusb)
    hidapi_add_hotplug_api_test(HotplugAPI_libusb hidapi_libusb)
    # The rawgadget provider toggles presence by unbinding/rebinding the gadget
    # from the dummy_hcd UDC; this runs in the label-gated libusb-vhid-test VM
    # job. The generous budgets anticipate the full (virtual) USB stack round
    # trips.
    hidapi_add_hotplug_test(Hotplug_libusb test_virtual_device_rawgadget.c hidapi_libusb 30000 900)
    target_compile_definitions(Hotplug_libusb PRIVATE TEST_VDEV_HAS_MANUFACTURER=1)
    # These tests share dummy_udc.0, so concurrent CTest runs would otherwise
    # create, destroy, or toggle the same virtual device.
    set_tests_properties(DeviceIO_libusb Hotplug_libusb PROPERTIES
        RESOURCE_LOCK vdev_rawgadget)
endif()

# --- Windows: winapi backend via a modified vhidmini2 UMDF driver -----------
if(WIN32 AND TARGET hidapi_winapi)
    hidapi_add_vdev_test(DeviceIO_winapi test_virtual_device_win.c hidapi_winapi)
    hidapi_add_hotplug_api_test(HotplugAPI_winapi hidapi_winapi)
    # The vhidmini provider toggles presence by disabling/enabling the HID child
    # devnode; this runs in the label-gated win-vhid-test job.
    hidapi_add_hotplug_test(Hotplug_winapi test_virtual_device_win.c hidapi_winapi 30000 900)
    # These tests share the installed vhidmini HID child, whose presence the
    # hotplug test toggles.
    set_tests_properties(DeviceIO_winapi Hotplug_winapi PROPERTIES
        RESOURCE_LOCK vdev_vhidmini)
    # hid: HidD_GetPreparsedData / HidP_GetCaps (device caps).
    # cfgmgr32: CM_Locate_DevNodeA / CM_Get_Child / CM_Get_Sibling /
    # CM_Get_Device_IDA / CM_Disable_DevNode / CM_Enable_DevNode, used by the
    # provider to locate the root-enumerated function devnode and toggle its HID
    # child devnode's presence (unplug/replug).
    # Set on the target (not via #pragma comment(lib), which MinGW ignores) so
    # every winapi toolchain -- MSVC, clang-cl and MinGW -- links it.
    target_link_libraries(DeviceIO_winapi PRIVATE hid cfgmgr32)
    target_link_libraries(Hotplug_winapi PRIVATE hid cfgmgr32)
    # Run from the directory holding the hidapi DLL so a shared build can find
    # it at launch (there is no rpath on Windows).
    set_tests_properties(DeviceIO_winapi HotplugAPI_winapi Hotplug_winapi PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:hidapi_winapi>")
    # With ASan (MSVC) the test exe needs the ASan runtime DLL, which lives next
    # to the MSVC tools; add it to PATH (CMake >= 3.22).
    if(HIDAPI_ENABLE_ASAN AND MSVC AND NOT CMAKE_VERSION VERSION_LESS "3.22")
        get_filename_component(MSVC_BUILD_TOOLS_DIR "${CMAKE_LINKER}" DIRECTORY)
        set_property(TEST DeviceIO_winapi HotplugAPI_winapi Hotplug_winapi PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_append:${MSVC_BUILD_TOOLS_DIR}")
    endif()
endif()

# --- macOS: IOKit backend via IOHIDUserDevice ------------------------------
# Self-skips where the virtual-device entitlement / user consent isn't
# available (e.g. hosted CI runners); usable locally / on a self-hosted Mac.
if(APPLE AND TARGET hidapi_darwin)
    hidapi_add_vdev_test(DeviceIO_darwin test_virtual_device_mac.c hidapi_darwin)
    hidapi_add_hotplug_api_test(HotplugAPI_darwin hidapi_darwin)
    # Self-skips until the IOHIDUserDevice provider implements unplug/replug.
    hidapi_add_hotplug_test(Hotplug_darwin test_virtual_device_mac.c hidapi_darwin 30000 300)
    target_link_libraries(DeviceIO_darwin PRIVATE
        "-framework IOKit" "-framework CoreFoundation")
    target_link_libraries(Hotplug_darwin PRIVATE
        "-framework IOKit" "-framework CoreFoundation")

    add_executable(HotplugLifecycle_darwin test_hotplug_mac.c)
    set_target_properties(HotplugLifecycle_darwin PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
    )
    target_link_libraries(HotplugLifecycle_darwin PRIVATE hidapi_darwin Threads::Threads)
    if(HIDAPI_ENABLE_ASAN)
        target_link_options(HotplugLifecycle_darwin PRIVATE -fsanitize=address)
    endif()
    add_test(NAME HotplugLifecycle_darwin COMMAND HotplugLifecycle_darwin)
    set_tests_properties(HotplugLifecycle_darwin PROPERTIES TIMEOUT 60)
endif()
