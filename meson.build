project('hidapi', 'c', version: files('VERSION'),meson_version: '>=0.60')

iconv_dep = dependency('iconv', required: get_option('libusb'))
libusb_dep = dependency('libusb-1.0', required: get_option('libusb'))
udev_dep = dependency('libudev', required: get_option('hidraw'))

incdirs = include_directories('hidapi')

install_headers('hidapi/hidapi.h', subdir: 'hidapi')

pconf = import('pkgconfig')

if libusb_dep.found() and iconv_dep.found()
  libhidapi = library('hidapi-libusb',
    'libusb/hid.c',
    dependencies: [ iconv_dep, libusb_dep ],
    include_directories: incdirs,
    version: meson.project_version(),
    install: true
  )

  pconf.generate(
    libhidapi,
    description: 'C Library for USB HID device access from Linux, Mac OS X, FreeBSD, and Windows. This is the libusb implementation.',
    url: 'https://github.com/libusb/hidapi',
    subdirs: 'hidapi',
  )
endif

if udev_dep.found() and host_machine.system() == 'linux'
  hidraw_lib = library('hidapi-hidraw',
    'linux/hid.c',
    dependencies: udev_dep,
    include_directories: incdirs,
    version: meson.project_version(),
    install: true
  )

  pconf.generate(
    hidraw_lib,
    description: 'C Library for USB/Bluetooth HID device access from Linux, Mac OS X, FreeBSD, and Windows. This is the hidraw implementation.',
    url: 'https://github.com/libusb/hidapi',
    subdirs: 'hidapi',
  )

  install_headers('libusb/hidapi_libusb.h', subdir: 'hidapi')
endif
