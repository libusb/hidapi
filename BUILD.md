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

## Building on Windows

To build the HIDAPI DLL on Windows using Visual Studio, build the `.sln` file
in the `windows/` directory.

To build HIDAPI using MinGW or Cygwin using Autotools, use a general Autotools
 [instruction](BUILD.autotools.md).

Any windows builds (MSVC or MinGW/Cygwin) are also supported by [CMake](BUILD.cmake.md).

HIDAPI can also be built using the Windows DDK (now also called the Windows
Driver Kit or WDK). This method was originally required for the HIDAPI build
but not anymore. However, some users still prefer this method. It is not as
well supported anymore but should still work. Patches are welcome if it does
not. To build using the DDK:

   1. Install the Windows Driver Kit (WDK) from Microsoft.
   2. From the Start menu, in the Windows Driver Kits folder, select Build
      Environments, then your operating system, then the x86 Free Build
      Environment (or one that is appropriate for your system).
   3. From the console, change directory to the `windows/ddk_build/` directory,
      which is part of the HIDAPI distribution.
   4. Type build.
   5. You can find the output files (DLL and LIB) in a subdirectory created
      by the build system which is appropriate for your environment. On
      Windows XP, this directory is `objfre_wxp_x86/i386`.
