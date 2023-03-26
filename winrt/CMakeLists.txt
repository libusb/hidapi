enable_language(CXX)

set(SOURCES
    hid.cpp
)

if(BUILD_SHARED_LIBS)
    list(APPEND SOURCES ../windows/hidapi.rc)
endif()

add_library(hidapi_winrt
    ${HIDAPI_PUBLIC_HEADERS}
    ${SOURCES}
)
set_target_properties(hidapi_winrt
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED TRUE
)

target_compile_definitions(hidapi_winrt PRIVATE _CRT_SECURE_NO_WARNINGS)

target_link_libraries(hidapi_winrt
    PUBLIC hidapi_include
    PRIVATE windowsapp
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(hidapi_winapi
        # prevent marking functions as __declspec(dllexport) for static library build
        # #480: this should be refactored for v1.0
        PUBLIC HID_API_NO_EXPORT_DEFINE
    )
endif()

set_target_properties(hidapi_winrt
    PROPERTIES
        EXPORT_NAME "winrt"
        OUTPUT_NAME "hidapi-winrt"
        VERSION ${PROJECT_VERSION}
        PUBLIC_HEADER "${HIDAPI_PUBLIC_HEADERS}"
)

# compatibility with find_package()
add_library(hidapi::winrt ALIAS hidapi_winrt)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_winrt EXPORT hidapi
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hidapi"
    )
endif()
