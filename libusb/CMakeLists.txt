cmake_minimum_required(VERSION 3.6.3 FATAL_ERROR)

list(APPEND HIDAPI_PUBLIC_HEADERS "hidapi_libusb.h")

add_library(hidapi_libusb
    ${HIDAPI_PUBLIC_HEADERS}
    hid.c
)
target_link_libraries(hidapi_libusb PUBLIC hidapi_include)

if(TARGET usb-1.0)
    target_link_libraries(hidapi_libusb PRIVATE usb-1.0)
else()
    include(FindPkgConfig)
    pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.9)
    target_link_libraries(hidapi_libusb PRIVATE PkgConfig::libusb)
endif()

find_package(Threads REQUIRED)
target_link_libraries(hidapi_libusb PRIVATE Threads::Threads)

if(HIDAPI_NO_ICONV)
    target_compile_definitions(hidapi_libusb PRIVATE NO_ICONV)
else()
    if(NOT ANDROID AND NOT CMAKE_VERSION VERSION_LESS 3.11)
        find_package(Iconv REQUIRED)
        include(CheckCSourceCompiles)
        target_link_libraries(hidapi_libusb PRIVATE Iconv::Iconv)
        set(CMAKE_REQUIRED_LIBRARIES "Iconv::Iconv")
        # check for error: "conflicting types for 'iconv'"
        check_c_source_compiles("#include<iconv.h>
            extern size_t iconv (iconv_t cd, const char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft);
            int main() {}"
        HIDAPI_ICONV_CONST)
        if(HIDAPI_ICONV_CONST)
            target_compile_definitions(hidapi_libusb PRIVATE "ICONV_CONST=const")
        endif()
    endif()
    # otherwise there is 3 options:
    # 1) On Android Iconv is disabled on the code level anyway, so no issue;
    # 2) iconv is provided by Standard C library and the build will be just fine;
    # 4) The _user_ has to provide additiona compilation options for this project/target.
endif()

set_target_properties(hidapi_libusb
    PROPERTIES
        EXPORT_NAME "libusb"
        OUTPUT_NAME "hidapi-libusb"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        PUBLIC_HEADER "${HIDAPI_PUBLIC_HEADERS}"
)

# compatibility with find_package()
add_library(hidapi::libusb ALIAS hidapi_libusb)
# compatibility with raw library link
add_library(hidapi-libusb ALIAS hidapi_libusb)

if(HIDAPI_INSTALL_TARGETS)
    install(TARGETS hidapi_libusb EXPORT hidapi
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hidapi"
    )
endif()

hidapi_configure_pc("${PROJECT_ROOT}/pc/hidapi-libusb.pc.in")
