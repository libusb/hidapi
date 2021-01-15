cmake_minimum_required(VERSION 3.1.3 FATAL_ERROR)
project(hidapi LANGUAGES C)

set(DEFAULT_CMAKE_BUILD_TYPES "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
if(NOT DEFINED CMAKE_BUILD_TYPE OR NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "${DEFAULT_CMAKE_BUILD_TYPES}" FORCE)
endif()
# This part is for convenience, when used one of the standard build types with cmake-gui
list(FIND DEFAULT_CMAKE_BUILD_TYPES "${CMAKE_BUILD_TYPE}" _build_type_index)
if(${_build_type_index} GREATER -1)
    # set it optionally, so a custom CMAKE_BUILD_TYPE can be used as well, if needed
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${DEFAULT_CMAKE_BUILD_TYPES})
endif()
unset(_build_type_index)
#

if(APPLE)
    option(CMAKE_FRAMEWORK "Build macOS/iOS Framework version of the library" OFF)
elseif(NOT WIN32)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        option(HIDAPI_WITH_HIDRAW "Build HIDRAW-based implementation of HIDAPI" ON)
        option(HIDAPI_WITH_LIBUSB "Build LIBUSB-based implementation of HIDAPI" ON)
    endif()
endif()

option(BUILD_SHARED_LIBS "Build shared version of the libraries, otherwise build statically" ON)

set(HIDAPI_INSTALL_TARGETS ON)

add_subdirectory(src)

set(BUILD_HIDTEST_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_HIDTEST_DEFAULT ON)
endif()
option(HIDAPI_BUILD_HIDTEST "Build small console test application hidtest" ${BUILD_HIDTEST_DEFAULT})
if(HIDAPI_BUILD_HIDTEST)
    add_subdirectory(hidtest)
endif()
