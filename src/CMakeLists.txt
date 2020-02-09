get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Read version from file
file(READ "${PROJECT_ROOT}/VERSION" VERSION_STR)
string(REGEX MATCH "^([0-9]+\\.[0-9]+\\.[0-9]+)(.*)" VERSION_STR "${VERSION_STR}")

set(VERSION "${CMAKE_MATCH_1}")
set(VERSION_SUFFIX "${CMAKE_MATCH_2}")
#

project(hidapi VERSION "${VERSION}" LANGUAGES C)

# Defaults and required options

if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
endif()
if(NOT DEFINED HIDAPI_WITH_HIDRAW)
    set(HIDAPI_WITH_HIDRAW ON)
endif()
if(NOT DEFINED HIDAPI_WITH_LIBUSB)
    set(HIDAPI_WITH_LIBUSB ON)
endif()
if(NOT DEFINED HIDAPI_INSTALL_TARGETS)
    set(HIDAPI_INSTALL_TARGETS OFF)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The library

add_library(hidapi_include INTERFACE)
target_include_directories(hidapi_include INTERFACE "${PROJECT_ROOT}/hidapi")

if(HIDAPI_INSTALL_TARGETS)
    include(GNUInstallDirs)
    install(FILES "${PROJECT_ROOT}/hidapi/hidapi.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hidapi")
endif()

if(WIN32)
    add_subdirectory("${PROJECT_ROOT}/windows" windows)
elseif(APPLE)
    add_subdirectory("${PROJECT_ROOT}/mac" mac)
else()
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        if(HIDAPI_WITH_HIDRAW)
            add_subdirectory("${PROJECT_ROOT}/linux" linux)
        endif()
        set(HIDAPI_LIBNAME_VERBOSE ON)
    else()
        set(HIDAPI_LIBNAME_VERBOSE OFF)
    endif()
    if(HIDAPI_WITH_LIBUSB)
        add_subdirectory("${PROJECT_ROOT}/libusb" libusb)
    endif()
endif()
