cmake_minimum_required(VERSION 3.20)
project(hidapi_no_crt_init_probe C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT HIDAPI_SOURCE_DIR)
    message(FATAL_ERROR "HIDAPI_SOURCE_DIR is required")
endif()
if(NOT HIDAPI_LIBRARY)
    message(FATAL_ERROR "HIDAPI_LIBRARY is required")
endif()

set(PROBE_INIT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/init_probe.c")
set(PROBE_MEM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/mem_probe.c")
set(PROBE_INCLUDES "${HIDAPI_SOURCE_DIR}/hidapi")

add_library(hidapi_no_crt STATIC IMPORTED)
set_target_properties(hidapi_no_crt PROPERTIES
    IMPORTED_LOCATION "${HIDAPI_LIBRARY}"
)

add_executable(no_crt_init_probe "${PROBE_INIT_SOURCE}")
target_include_directories(no_crt_init_probe PRIVATE "${PROBE_INCLUDES}")
target_compile_definitions(no_crt_init_probe PRIVATE HID_API_NO_EXPORT_DEFINE)
target_link_libraries(no_crt_init_probe PRIVATE
    hidapi_no_crt
    kernel32
    user32
    shlwapi
)

add_executable(no_crt_mem_probe "${PROBE_MEM_SOURCE}")
target_include_directories(no_crt_mem_probe PRIVATE "${PROBE_INCLUDES}")
target_compile_definitions(no_crt_mem_probe PRIVATE
    HIDAPI_NO_CRT
    HID_API_NO_EXPORT_DEFINE
)
target_link_libraries(no_crt_mem_probe PRIVATE
    kernel32
    user32
    shlwapi
)

if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(no_crt_init_probe PRIVATE /GS- /Gs9999999)
    target_compile_options(no_crt_mem_probe PRIVATE /GS- /Gs9999999)
    target_link_options(no_crt_init_probe PRIVATE
        /NODEFAULTLIB
        /ENTRY:mainCRTStartup
        /SUBSYSTEM:CONSOLE
    )
    target_link_options(no_crt_mem_probe PRIVATE
        /NODEFAULTLIB
        /ENTRY:mainCRTStartup
        /SUBSYSTEM:CONSOLE
    )
elseif(MINGW)
    target_compile_options(no_crt_init_probe PRIVATE -mno-stack-arg-probe)
    target_compile_options(no_crt_mem_probe PRIVATE -mno-stack-arg-probe)
    target_link_options(no_crt_init_probe PRIVATE
        -nostdlib
        -Wl,--entry=mainCRTStartup
        -Wl,--subsystem,console
    )
    target_link_options(no_crt_mem_probe PRIVATE
        -nostdlib
        -Wl,--entry=mainCRTStartup
        -Wl,--subsystem,console
    )
else()
    message(FATAL_ERROR "Unsupported no-CRT toolchain")
endif()
