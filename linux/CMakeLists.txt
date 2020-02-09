cmake_minimum_required(VERSION 3.6.3 FATAL_ERROR)

add_library(hidapi_hidraw
    hid.c
)
target_link_libraries(hidapi_hidraw PUBLIC hidapi_include)

include(FindPkgConfig)
include(FindThreads)

pkg_check_modules(libudev REQUIRED IMPORTED_TARGET libudev)
target_link_libraries(hidapi_hidraw PRIVATE PkgConfig::libudev Threads::Threads)

set_target_properties(hidapi_hidraw
    PROPERTIES
        EXPORT_NAME "hidraw"
        OUTPUT_NAME "hidapi-hidraw"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_hidraw
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

if(NOT HIDAPI_WITH_LIBUSB)
    add_library(hidapi ALIAS hidapi_hidraw)
endif()
