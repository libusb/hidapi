# Apply HIDAPI_ENABLE_ASAN to a library or executable target.
# Call it where the target is defined. It instruments C and C++ sources and,
# for an instrumented static library, propagates the sanitizer runtime to
# consumers (including installed packages).
function(hidapi_configure_sanitizers target)
    if(NOT HIDAPI_ENABLE_ASAN)
        return()
    endif()

    target_compile_options(${target} PRIVATE -fsanitize=address)
    if(MSVC)
        # The default "/INCREMENTAL" causes a warning when "-fsanitize=address" is present.
        set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS_DEBUG " /INCREMENTAL:NO")
        set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS_RELWITHDEBINFO " /INCREMENTAL:NO")
    else()
        get_target_property(target_type ${target} TYPE)
        if(target_type STREQUAL "STATIC_LIBRARY")
            # Consumers of an instrumented static library need the runtime.
            if(CMAKE_VERSION VERSION_LESS "3.13")
                set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES -fsanitize=address)
            else()
                target_link_options(${target} INTERFACE -fsanitize=address)
            endif()
        elseif(CMAKE_VERSION VERSION_LESS "3.13")
            set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS " -fsanitize=address")
        else()
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()
