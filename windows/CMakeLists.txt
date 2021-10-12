add_library(hidapi_winapi
    ${HIDAPI_PUBLIC_HEADERS}
    hid.c
)
target_link_libraries(hidapi_winapi
    PUBLIC hidapi_include
    PRIVATE setupapi
)

set_target_properties(hidapi_winapi
    PROPERTIES
        EXPORT_NAME "winapi"
        OUTPUT_NAME "hidapi"
        VERSION ${PROJECT_VERSION}
        PUBLIC_HEADER "${HIDAPI_PUBLIC_HEADERS}"
)

# compatibility with find_package()
add_library(hidapi::winapi ALIAS hidapi_winapi)
# compatibility with raw library link
add_library(hidapi ALIAS hidapi_winapi)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_winapi EXPORT hidapi
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hidapi"
    )
endif()

hidapi_configure_pc("${PROJECT_ROOT}/pc/hidapi.pc.in")

add_executable(offline_unit_test ./test/offline_unit_test.c)
target_link_libraries(offline_unit_test
        PUBLIC hidapi_include
        PRIVATE setupapi
    )
add_test(ReconstructorTest_046D_C52F_0001_000C offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0001_000C.pp_data)
add_test (ReconstructorCompareOutput_046D_C52F_0001_000C ${CMAKE_COMMAND} -E compare_files
     046D_C52F_0001_000C_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0001_000C_expected.rpt_desc)
     
add_test(ReconstructorTest_046D_C52F_0001_FF00 offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0001_FF00.pp_data)
add_test (ReconstructorCompareOutput_046D_C52F_0001_FF00 ${CMAKE_COMMAND} -E compare_files
     046D_C52F_0001_FF00_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0001_FF00_expected.rpt_desc)

add_test(ReconstructorTest_046D_C52F_0002_0001 offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0002_0001.pp_data)
add_test (ReconstructorCompareOutput_046D_C52F_0002_0001 ${CMAKE_COMMAND} -E compare_files
     046D_C52F_0002_0001_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0002_0001_expected.rpt_desc)
     
add_test(ReconstructorTest_046D_C52F_0002_FF00 offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0002_FF00.pp_data)
add_test (ReconstructorCompareOutput_046D_C52F_0002_FF00 ${CMAKE_COMMAND} -E compare_files
     046D_C52F_0002_FF00_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046D_C52F_0002_FF00_expected.rpt_desc)
     
add_test(ReconstructorTest_17CC_1130_0000_FF01 offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/17CC_1130_0000_FF01.pp_data)
add_test (ReconstructorCompareOutput_17CC_1130_0000_FF01 ${CMAKE_COMMAND} -E compare_files
     17CC_1130_0000_FF01_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/17CC_1130_0000_FF01_expected.rpt_desc)
     
add_test(ReconstructorTest_046D_0A37_0001_000C offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046D_0A37_0001_000C.pp_data)
add_test (ReconstructorCompareOutput_046D_0A37_0001_000C ${CMAKE_COMMAND} -E compare_files
     046D_0A37_0001_000C_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046D_0A37_0001_000C_expected.rpt_desc)
     
add_test(ReconstructorTest_046A_0011_0006_0001 offline_unit_test ${CMAKE_SOURCE_DIR}/windows/test/046A_0011_0006_0001.pp_data)
add_test (ReconstructorCompareOutput_046A_0011_0006_0001 ${CMAKE_COMMAND} -E compare_files
     046A_0011_0006_0001_reconstructed.rpt_desc ${CMAKE_SOURCE_DIR}/windows/test/046A_0011_0006_0001_expected.rpt_desc)

