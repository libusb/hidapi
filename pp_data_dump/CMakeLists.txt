project(pp_data_dump C)

set(HIDAPI_pp_data_dump_TARGETS)
if(WIN32)
    add_executable(pp_data_dump pp_data_dump.c)  
    target_link_libraries(pp_data_dump
        PUBLIC hidapi_include
        PRIVATE setupapi
    )
    list(APPEND HIDAPI_pp_data_dump_TARGETS pp_data_dump)
else()    
      message(FATAL_ERROR "Can only work on Windows ")
endif()

install(TARGETS ${HIDAPI_pp_data_dump_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

hidapi_configure_pc("../pc/hidapi.pc.in")