add_executable(ReadInterrupt_freebsd test_read_interrupt.c)
hidapi_configure_sanitizers(ReadInterrupt_freebsd)
set_target_properties(ReadInterrupt_freebsd PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED TRUE
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED TRUE
)
if(HIDAPI_BUILD_AS_CXX)
    set_source_files_properties(test_read_interrupt.c PROPERTIES LANGUAGE CXX)
endif()

# The test includes the backend implementation directly to exercise allocation
# and descriptor cleanup with a socket transport; do not link a second copy.
target_link_libraries(ReadInterrupt_freebsd PRIVATE
    hidapi_include PkgConfig::libusb Threads::Threads
)

add_test(NAME ReadInterrupt_freebsd COMMAND ReadInterrupt_freebsd)
set_tests_properties(ReadInterrupt_freebsd PROPERTIES TIMEOUT 60)
