# Building HIDAPI from Source

## Table of contents

* [Prerequisites](#prerequisites)
    * [Linux](#linux)
    * [FreeBSD](#freebsd)
    * [Mac](#mac)
    * [Windows](#windows)
* [Building](#building)
* [Integrating hidapi directly into your source tree](#integrating-hidapi-directly-into-your-source-tree)

## Prerequisites:

### Linux:

Depending on which backend you're going to build, you'll need to install
additional development packages. For `linux/hidraw` backend you need
development package for `libudev`. For `libusb` backend, naturally, you need
`libusb` development package.

On Debian/Ubuntu systems these can be installed by running:
```sh
# required only by hidraw backend
sudo apt install libudev-dev
# required only by libusb backend
sudo apt install libusb-1.0-0-dev
```

### FreeBSD:

On FreeBSD you will need to install libiconv. This is done by running
the following:
```sh
pkg_add -r libiconv
```

### Mac:

On Mac make sure you have XCode installed and its Command Line Tools.

### Windows:

On Windows you just need a compiler. You may use Visual Studio or Cygwin/MinGW,
depending on which environment is best for your needs.

## Building

hidapi uses the CMake build system. Refer to [BUILD.cmake.md](BUILD.cmake.md) for details.

## Integrating HIDAPI directly into your source tree

Instead of using one of the provided build systems, you may want to integrate
HIDAPI directly into your source tree.
Generally it is not encouraged to do so, but if you must, all you need to do:
- add a single source file `hid.c` (for a specific backend);
- setup include directory to `<HIDAPI repo root>/hidapi`;
- add link libraries, that are specific for each backend.

Check the manual makefiles for a simple example/reference of what are the dependencies of each specific backend.

NOTE: if your have a CMake-based project, you're likely be able to use
HIDAPI directly as a subdirectory. Check [BUILD.cmake.md](BUILD.cmake.md) for details.
