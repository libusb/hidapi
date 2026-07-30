/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022, All Rights Reserved.

 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU General Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        https://github.com/libusb/hidapi .
********************************************************/

#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>


typedef void *dlopen_(const char *filename, int flags);
typedef int dlclose_(void *handle);

static dlopen_ *real_dlopen;
static dlclose_ *real_dlclose;
static void *udev_handle;
static unsigned int udev_attempts;
static unsigned int udev_open_count;
static unsigned int udev_close_count;

static int resolve_real_functions(void)
{
	void *symbol;

	if (!real_dlopen) {
		symbol = dlsym(RTLD_NEXT, "dlopen");
		if (!symbol || sizeof(real_dlopen) != sizeof(symbol))
			return -1;
		memcpy(&real_dlopen, &symbol, sizeof(symbol));
	}

	if (!real_dlclose) {
		symbol = dlsym(RTLD_NEXT, "dlclose");
		if (!symbol || sizeof(real_dlclose) != sizeof(symbol))
			return -1;
		memcpy(&real_dlclose, &symbol, sizeof(symbol));
	}

	return 0;
}

static unsigned int failure_attempt_limit(void)
{
	const char *value = getenv("HIDAPI_TEST_FAIL_ATTEMPTS");
	char *end;
	unsigned long limit;

	if (!value)
		return 0;

	limit = strtoul(value, &end, 10);
	if (*value == '\0' || *end != '\0' || limit > UINT_MAX)
		return 0;

	return (unsigned int) limit;
}

void *dlopen(const char *filename, int flags)
{
	void *handle;
	const int is_udev = filename && strcmp(filename, "libudev.so.1") == 0;

	if (resolve_real_functions() < 0)
		return NULL;

	if (is_udev && udev_attempts++ < failure_attempt_limit()) {
		const char *fake_udev = getenv("HIDAPI_TEST_FAKE_UDEV");

		if (!fake_udev)
			return NULL;
		handle = real_dlopen(fake_udev, flags);
	}
	else {
		handle = real_dlopen(filename, flags);
	}

	if (is_udev && handle) {
		udev_handle = handle;
		++udev_open_count;
	}

	return handle;
}

int dlclose(void *handle)
{
	if (resolve_real_functions() < 0)
		return -1;

	if (handle == udev_handle) {
		udev_handle = NULL;
		++udev_close_count;
	}

	return real_dlclose(handle);
}

unsigned int hidapi_test_udev_open_count(void)
{
	return udev_open_count;
}

unsigned int hidapi_test_udev_close_count(void)
{
	return udev_close_count;
}
