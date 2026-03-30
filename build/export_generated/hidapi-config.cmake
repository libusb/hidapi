
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was hidapi-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

set(hidapi_VERSION_MAJOR "0")
set(hidapi_VERSION_MINOR "16")
set(hidapi_VERSION_PATCH "0")
set(hidapi_VERSION "0.16.0")
set(hidapi_VERSION_STR "0.16.0")

set(hidapi_FOUND FALSE)

set(HIDAPI_NEED_EXPORT_THREADS FALSE)
set(HIDAPI_NEED_EXPORT_LIBUSB FALSE)
set(HIDAPI_NEED_EXPORT_LIBUDEV FALSE)
set(HIDAPI_NEED_EXPORT_ICONV FALSE)

if(HIDAPI_NEED_EXPORT_THREADS)
  if(CMAKE_VERSION VERSION_LESS 3.4.3)
    message(FATAL_ERROR "This file relies on consumers using CMake 3.4.3 or greater.")
  endif()
  find_package(Threads REQUIRED)
endif()

if(HIDAPI_NEED_EXPORT_LIBUSB OR HIDAPI_NEED_EXPORT_LIBUDEV)
  if(CMAKE_VERSION VERSION_LESS 3.6.3)
    message(FATAL_ERROR "This file relies on consumers using CMake 3.6.3 or greater.")
  endif()
  find_package(PkgConfig)
  if(HIDAPI_NEED_EXPORT_LIBUSB)
    pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.9)
  endif()
  if(HIDAPI_NEED_EXPORT_LIBUDEV)
    pkg_check_modules(libudev REQUIRED IMPORTED_TARGET libudev)
  endif()
endif()

if(HIDAPI_NEED_EXPORT_ICONV)
  if(CMAKE_VERSION VERSION_LESS 3.11)
    message(WARNING "HIDAPI requires CMake target Iconv::Iconv, make sure to provide it")
  else()
    find_package(Iconv REQUIRED)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/libhidapi.cmake")

set(hidapi_FOUND TRUE)

foreach(_component hidraw;libusb)
  if(TARGET hidapi::${_component})
    set(hidapi_${_component}_FOUND TRUE)
  endif()
endforeach()

check_required_components(hidapi)

if(NOT TARGET hidapi::hidapi)
  add_library(hidapi::hidapi INTERFACE IMPORTED)
  set_target_properties(hidapi::hidapi PROPERTIES
    INTERFACE_LINK_LIBRARIES hidapi::hidraw
  )
endif()
