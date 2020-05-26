cmake_minimum_required(VERSION 3.1.3 FATAL_ERROR)
project(hidapi LANGUAGES C)

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Debug;Release;MinSizeRel;RelWithDebInfo" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")

if(APPLE)
    option(CMAKE_FRAMEWORK "Build macOS/iOS Framework version of the library" OFF)
elseif(NOT WIN32)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        option(HIDAPI_WITH_HIDRAW "Build HIDRAW-based implementation of HIDAPI" ON)
    endif()
    option(HIDAPI_WITH_LIBUSB "Build LIBUSB-based implementation of HIDAPI" ON)
endif()

option(BUILD_SHARED_LIBS "When set: build shared version of the libraries, otherwise - static" ON)

set(HIDAPI_INSTALL_TARGETS ON)

add_subdirectory(src)

set(BUILD_HIDTEST_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_HIDTEST_DEFAULT ON)
endif()
option(BUILD_HIDTEST "Build small console test application hidtest" ${BUILD_HIDTEST_DEFAULT})
if(BUILD_HIDTEST)
    add_subdirectory(hidtest)
endif()
