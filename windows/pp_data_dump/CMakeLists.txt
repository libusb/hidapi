project(pp_data_dump C)

set(HIDAPI_pp_data_dump_TARGETS)
if(WIN32)
    add_executable(pp_data_dump pp_data_dump.c)    
    set_target_properties(pp_data_dump
        PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED TRUE
    )
    target_link_libraries(pp_data_dump
    PRIVATE hidapi_include hidapi_winapi
)
    list(APPEND HIDAPI_pp_data_dump_TARGETS pp_data_dump)
else()    
      message(FATAL_ERROR "Can only work on Windows ")
endif()

install(TARGETS ${HIDAPI_pp_data_dump_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
