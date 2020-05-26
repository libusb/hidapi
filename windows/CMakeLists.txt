add_library(hidapi_winapi
    hid.c
)
target_link_libraries(hidapi_winapi
    PUBLIC hidapi_include
    PRIVATE Setupapi
)

set_target_properties(hidapi_winapi
    PROPERTIES
        EXPORT_NAME "winapi"
        OUTPUT_NAME "hidapi"
        VERSION ${PROJECT_VERSION}
)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_winapi
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

add_library(hidapi ALIAS hidapi_winapi)
