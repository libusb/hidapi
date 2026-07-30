/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026, All Rights Reserved.

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
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "hidapi.h"


typedef unsigned int counter_(void);

static int read_counter(const char *name, unsigned int *value)
{
	counter_ *counter;
	void *symbol = dlsym(RTLD_DEFAULT, name);

	if (!symbol) {
		fprintf(stderr, "unable to resolve test counter %s: %s\n", name, dlerror());
		return -1;
	}
	if (sizeof(counter) != sizeof(symbol)) {
		fprintf(stderr, "test counter pointer size mismatch\n");
		return -1;
	}
	memcpy(&counter, &symbol, sizeof(symbol));
	*value = counter();
	return 0;
}

static int verify_balanced_cleanup(unsigned int initial_opens,
                                   unsigned int initial_closes,
                                   unsigned int expected_cycles)
{
	unsigned int final_opens;
	unsigned int final_closes;

	if (read_counter("hidapi_test_udev_open_count", &final_opens) < 0 ||
	    read_counter("hidapi_test_udev_close_count", &final_closes) < 0)
		return 1;

	if (final_opens - initial_opens != expected_cycles ||
	    final_closes - initial_closes != expected_cycles) {
		fprintf(stderr,
		        "unbalanced libudev lifecycle: expected %u cycles, observed %u opens and %u closes\n",
		        expected_cycles,
		        final_opens - initial_opens,
		        final_closes - initial_closes);
		return 1;
	}

	return 0;
}

static int test_reload(void)
{
	int i;
	unsigned int initial_opens;
	unsigned int initial_closes;

	if (read_counter("hidapi_test_udev_open_count", &initial_opens) < 0 ||
	    read_counter("hidapi_test_udev_close_count", &initial_closes) < 0)
		return 1;

	for (i = 0; i < 3; ++i) {
		if (hid_init() != 0) {
			const wchar_t *error = hid_error(NULL);
			fwprintf(stderr, L"hid_init failed: %ls\n", error ? error : L"(null)");
			return 1;
		}
		if (hid_exit() != 0) {
			fprintf(stderr, "hid_exit failed\n");
			return 1;
		}
	}

	return verify_balanced_cleanup(initial_opens, initial_closes, 3);
}

static int test_failure_recovery(void)
{
	int i;
	unsigned int initial_opens;
	unsigned int initial_closes;

	if (read_counter("hidapi_test_udev_open_count", &initial_opens) < 0 ||
	    read_counter("hidapi_test_udev_close_count", &initial_closes) < 0)
		return 1;

	for (i = 0; i < 2; ++i) {
		const wchar_t *error;

		if (hid_init() != -1) {
			fprintf(stderr, "hid_init unexpectedly succeeded with an incomplete libudev\n");
			return 1;
		}

		error = hid_error(NULL);
		if (!error || !wcsstr(error, L"udev_list_entry_get_name")) {
			fwprintf(stderr, L"hid_init returned an unexpected error: %ls\n", error ? error : L"(null)");
			return 1;
		}
	}

	if (hid_init() != 0) {
		const wchar_t *error = hid_error(NULL);
		fwprintf(stderr, L"hid_init did not recover: %ls\n", error ? error : L"(null)");
		return 1;
	}
	if (hid_exit() != 0) {
		fprintf(stderr, "hid_exit failed after recovery\n");
		return 1;
	}

	if (hid_init() != 0) {
		const wchar_t *error = hid_error(NULL);
		fwprintf(stderr, L"hid_init failed after reload: %ls\n", error ? error : L"(null)");
		return 1;
	}
	if (hid_exit() != 0) {
		fprintf(stderr, "final hid_exit failed\n");
		return 1;
	}

	return verify_balanced_cleanup(initial_opens, initial_closes, 4);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s reload|failure-recovery\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "reload") == 0)
		return test_reload();
	if (strcmp(argv[1], "failure-recovery") == 0)
		return test_failure_recovery();

	fprintf(stderr, "unknown test mode: %s\n", argv[1]);
	return 2;
}
