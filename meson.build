project('hidapi')

cmake = import('cmake')

hidapi_build_options = cmake.subproject_options()
hidapi_build_options.set_install(true)

hidapi_build = cmake.subproject('hidapi_build_cmake', options: hidapi_build_options)

if (hidapi_build.target_list().contains('hidapi_winapi'))
	hidapi_dep = hidapi_build.dependency('hidapi_winapi')
elif (hidapi_build.target_list().contains('hidapi_darwin'))
	hidapi_dep = hidapi_build.dependency('hidapi_darwin')
elif (hidapi_build.target_list().contains('hidapi_hidraw'))
	hidapi_dep = hidapi_build.dependency('hidapi_hidraw')
elif (hidapi_build.target_list().contains('hidapi_libusb'))
	hidapi_dep = hidapi_build.dependency('hidapi_libusb')
endif

if (hidapi_build.target_list().contains('hidapi_hidraw'))
	hidapi_hidraw_dep = hidapi_build.dependency('hidapi_hidraw')
endif
if (hidapi_build.target_list().contains('hidapi_libusb'))
	hidapi_libusb_dep = hidapi_build.dependency('hidapi_libusb')
endif
