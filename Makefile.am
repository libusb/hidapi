
ACLOCAL_AMFLAGS = -I m4

if OS_FREEBSD
pkgconfigdir=$(prefix)/libdata/pkgconfig
else
pkgconfigdir=$(libdir)/pkgconfig
endif

pkgconfig_DATA=pc/hidapi.pc

if OS_LINUX
pkgconfig_DATA += pc/hidapi-hidraw.pc pc/hidapi-libusb.pc
endif

if OS_DARWIN
pkgconfig_DATA += pc/hidapi-darwin.pc
endif

if OS_FREEBSD
pkgconfig_DATA += pc/hidapi-libusb.pc
endif

if OS_KFREEBSD
pkgconfig_DATA += pc/hidapi-libusb.pc
endif

if OS_HAIKU
pkgconfig_DATA += pc/hidapi-libusb.pc
endif

if OS_WINDOWS
pkgconfig_DATA += pc/hidapi-winapi.pc
endif

SUBDIRS=

if OS_LINUX
SUBDIRS += linux libusb
endif

if OS_DARWIN
SUBDIRS += mac
endif

if OS_FREEBSD
SUBDIRS += libusb
endif

if OS_KFREEBSD
SUBDIRS += libusb
endif

if OS_HAIKU
SUBDIRS += libusb
endif

if OS_WINDOWS
SUBDIRS += windows
endif

SUBDIRS += hidtest

if BUILD_TESTGUI
SUBDIRS += testgui
endif

EXTRA_DIST = \
 udev \
 doxygen \
 tests/pkg-config/configure.ac \
 tests/pkg-config/Makefile.am \
 tests/pkg-config/test.c

dist_doc_DATA = \
 README.md \
 AUTHORS.txt \
 LICENSE-bsd.txt \
 LICENSE-gpl3.txt \
 LICENSE-orig.txt \
 LICENSE.txt

SCMCLEAN_TARGETS= \
 aclocal.m4 \
 config.guess \
 config.sub \
 configure \
 config.h.in \
 depcomp \
 install-sh \
 ltmain.sh \
 missing \
 mac/Makefile.in \
 testgui/Makefile.in \
 libusb/Makefile.in \
 Makefile.in \
 linux/Makefile.in \
 windows/Makefile.in \
 m4/libtool.m4 \
 m4/lt~obsolete.m4 \
 m4/ltoptions.m4 \
 m4/ltsugar.m4 \
 m4/ltversion.m4

SCMCLEAN_DIR_TARGETS = \
 autom4te.cache

scm-clean: distclean
	rm -f $(SCMCLEAN_TARGETS)
	rm -Rf $(SCMCLEAN_DIR_TARGETS)
