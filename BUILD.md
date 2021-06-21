# Building HIDAPI from Source

## Table of content

* [Intro](#intro)
* [Prerequisites](#prerequisites)
    * [Linux](#linux)
    * [FreeBSD](#freebsd)
    * [Mac](#mac)
    * [Windows](#windows)
* [Integrating hidapi directly into your source tree](#integrating-hidapi-directly-into-your-source-tree)
* [Building the manual way on Unix platforms](#building-the-manual-way-on-unix-platforms)
* [Building on Windows](#building-on-windows)

## Intro

For various reasons you may need to build HIDAPI on your own.

It can be done in several different ways:
- using [Autotools](BUILD.autotools.md);
- using [CMake](BUILD.cmake.md);
- using [manual makefiles](#building-the-manual-way-on-unix-platforms).

**Autotools** build system is historically first mature build system for
HIDAPI. Most common usage of it is in its separate README: [BUILD.autotools.md](BUILD.autotools.md).

**CMake** build system is de facto an industry standard for many open-source and proprietary projects and solutions.
HIDAPI is one of the projects which uses the power of CMake for its advantage.
More documentation is available in its separate README: [BUILD.cmake.md](BUILD.cmake.md).

If you don't know where to start to build HIDAPI, we recommend starting with [CMake](BUILD.cmake.md) build.

## Prerequisites:

Regardless of what build system system you choose to use, there are specific dependencies for each platform/backend.

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

## Building the manual way on Unix platforms

Manual Makefiles are provided mostly to give the user an idea what it takes
to build a program which embeds HIDAPI directly inside of it. These should
really be used as examples only. If you want to build a system-wide shared
library, use one of the build systems mentioned above.

To build HIDAPI using the manual Makefiles, change to the directory
of your platform and run make. For example, on Linux run:
```sh
cd linux/
make -f Makefile-manual
```

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
