add_executable(hid_report_reconstructor_test hid_report_reconstructor_test.c)
set_target_properties(hid_report_reconstructor_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
)

target_link_libraries(hid_report_reconstructor_test
     PRIVATE hidapi_include hidapi_winapi
)

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
)

foreach(TEST_CASE ${HID_DESCRIPTOR_RECONSTRUCT_TEST_CASES})
     set(TEST_PP_DATA "${CMAKE_CURRENT_LIST_DIR}/data/${TEST_CASE}.pp_data")
     if(NOT EXISTS "${TEST_PP_DATA}")
          message(FATAL_ERROR "Missing '${TEST_PP_DATA}' file for '${TEST_CASE}' test case")
     endif()
     set(TEST_EXPECTED_DESCRIPTOR "${CMAKE_CURRENT_LIST_DIR}/data/${TEST_CASE}_expected.rpt_desc")
     if(NOT EXISTS "${TEST_EXPECTED_DESCRIPTOR}")
          message(FATAL_ERROR "Missing '${TEST_EXPECTED_DESCRIPTOR}' file for '${TEST_CASE}' test case")
     endif()

     add_test(NAME "WinHidReportReconstructTest_${TEST_CASE}"
          COMMAND hid_report_reconstructor_test "${TEST_PP_DATA}" "${TEST_EXPECTED_DESCRIPTOR}"
          WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
     )
endforeach()
