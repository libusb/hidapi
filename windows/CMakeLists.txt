list(APPEND HIDAPI_PUBLIC_HEADERS "hidapi_winapi.h")

set(SOURCES
    hid.c
    hidapi_cfgmgr32.h
    hidapi_descriptor_reconstruct.c
    hidapi_descriptor_reconstruct.h
    hidapi_hidclass.h
    hidapi_hidpi.h
    hidapi_hidsdi.h
)

if(BUILD_SHARED_LIBS)
    list(APPEND SOURCES hidapi.rc)
endif()

add_library(hidapi_winapi
    ${HIDAPI_PUBLIC_HEADERS}
    ${SOURCES}
)
if(HIDAPI_BUILD_AS_CXX)
    set_source_files_properties(hid.c hidapi_descriptor_reconstruct.c PROPERTIES LANGUAGE CXX)
endif()
target_link_libraries(hidapi_winapi
    PUBLIC hidapi_include
)

target_compile_definitions(hidapi_winapi
    # CMake compiles hidapi_descriptor_reconstruct.c as a separate translation unit,
    # so hid.c must not #include it (to avoid duplicate symbol LNK4006 in static builds).
    PRIVATE HIDAPI_CMAKE_V0_BUILD
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(hidapi_winapi
        # prevent marking functions as __declspec(dllexport) for static library build
        # #480: this should be refactored for v1.0
        PUBLIC HID_API_NO_EXPORT_DEFINE
    )
endif()

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

if(HIDAPI_WITH_TESTS)
     add_subdirectory(test)
endif()

if(DEFINED HIDAPI_BUILD_PP_DATA_DUMP AND HIDAPI_BUILD_PP_DATA_DUMP)
    add_subdirectory(pp_data_dump)
endif()
