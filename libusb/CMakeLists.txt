cmake_minimum_required(VERSION 3.6.3 FATAL_ERROR)

add_library(hidapi_libusb
    hid.c
)
target_link_libraries(hidapi_libusb PUBLIC hidapi_include)

include(FindPkgConfig)
include(FindThreads)

pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.9)
target_link_libraries(hidapi_libusb PRIVATE PkgConfig::libusb Threads::Threads)

set(HIDAPI_LIBNAME "hidapi")
if(HIDAPI_LIBNAME_VERBOSE)
    set(HIDAPI_LIBNAME "${HIDAPI_LIBNAME}-libusb")
endif()

set_target_properties(hidapi_libusb
    PROPERTIES
        EXPORT_NAME "libusb"
        OUTPUT_NAME "${HIDAPI_LIBNAME}"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_libusb
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

add_library(hidapi ALIAS hidapi_libusb)
