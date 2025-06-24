add_executable(max_input_report_size_test max_input_report_size_test.c)
set_target_properties(max_input_report_size_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
)

if(TARGET usb-1.0)
    target_link_libraries(max_input_report_size_test PRIVATE usb-1.0)
else()
    include(FindPkgConfig)
    pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.9)
    target_link_libraries(max_input_report_size_test PRIVATE PkgConfig::libusb)
endif()

target_link_libraries(max_input_report_size_test PUBLIC hidapi_include)

# Each test case requires 2 files:
# <name>.pp_data - textual representation of HIDP_PREPARSED_DATA;
# <name>_expected.rpt_desc - reconstructed HID Report Descriptor out of .pp_data file;
#
# (Non-required by test):
# <name>_real.dpt_desc - the original report rescriptor used to create a test case;
set(HID_DESCRIPTOR_RECONSTRUCT_TEST_CASES
     046D_C52F_0001_000C
     046D_C52F_0001_FF00
     046D_C52F_0002_0001
     046D_C52F_0002_FF00
     17CC_1130_0000_FF01
     046D_0A37_0001_000C
     046A_0011_0006_0001
     046D_C077_0002_0001
     046D_C283_0004_0001
     046D_B010_0006_0001
     046D_B010_0002_FF00
     046D_B010_0002_0001
     046D_B010_0001_FF00
     046D_B010_0001_000C
     046D_C534_0001_000C
     046D_C534_0001_FF00
     046D_C534_0002_0001
     046D_C534_0002_FF00
     046D_C534_0006_0001
     046D_C534_0080_0001
     047F_C056_0001_000C
     047F_C056_0003_FFA0
     047F_C056_0005_000B
     045E_02FF_0005_0001
     1532_00A3_0002_0001
)

set(CMAKE_VERSION_SUPPORTS_ENVIRONMENT_MODIFICATION "3.22")

foreach(TEST_CASE ${HID_DESCRIPTOR_RECONSTRUCT_TEST_CASES})
     set(TEST_PP_DATA "${CMAKE_CURRENT_LIST_DIR}/../../windows/test/data/${TEST_CASE}.pp_data")
     if(NOT EXISTS "${TEST_PP_DATA}")
          message(FATAL_ERROR "Missing '${TEST_PP_DATA}' file for '${TEST_CASE}' test case")
     endif()
     set(TEST_EXPECTED_DESCRIPTOR "${CMAKE_CURRENT_LIST_DIR}/../../windows/test/data/${TEST_CASE}_expected.rpt_desc")
     if(NOT EXISTS "${TEST_EXPECTED_DESCRIPTOR}")
          message(FATAL_ERROR "Missing '${TEST_EXPECTED_DESCRIPTOR}' file for '${TEST_CASE}' test case")
     endif()

     add_test(NAME "LibUsbHidMaxInputReportSizeTest_${TEST_CASE}"
          COMMAND max_input_report_size_test "${TEST_PP_DATA}" "${TEST_EXPECTED_DESCRIPTOR}"
          WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
     )
endforeach()
