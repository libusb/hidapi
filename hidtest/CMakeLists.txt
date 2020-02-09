project(hidtest C)

set(HIDAPI_HIDTEST_TARGETS)
if(NOT WIN32 AND NOT APPLE AND CMAKE_SYSTEM_NAME MATCHES "Linux")
    if(TARGET hidapi_hidraw)
        add_executable(hidtest_hidraw test.c)
        target_link_libraries(hidtest_hidraw hidapi_hidraw)
        list(APPEND HIDAPI_HIDTEST_TARGETS hidtest_hidraw)
    endif()
    if(TARGET hidapi_libusb)
        add_executable(hidtest_libusb test.c)
        target_link_libraries(hidtest_libusb hidapi_libusb)
        list(APPEND HIDAPI_HIDTEST_TARGETS hidtest_libusb)
    endif()
else()
    add_executable(hidtest test.c)
    target_link_libraries(hidtest hidapi)
    list(APPEND HIDAPI_HIDTEST_TARGETS hidtest)
endif()

install(TARGETS ${HIDAPI_HIDTEST_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
