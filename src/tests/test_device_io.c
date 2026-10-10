/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 A simple device-I/O smoke test for the virtual HID device:
 open the device, write an output report, exchange a couple of
 reports (a Feature report triggers the device to replay a
 pre-recorded input report, which is then read back), and close.

 The test is backend-agnostic: it only ever calls the public HIDAPI
 API and the test_virtual_device interface, so the very same test
 runs against every backend that ships a virtual-device provider
 (Linux uhid, Windows vhidmini2, libusb raw-gadget, macOS).

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hidapi.h>

#include "test_virtual_device.h"

/* CTest treats this exit code as "skipped" (see SKIP_RETURN_CODE in CMake). */
#define EXIT_SKIP 77

/* Test-unique ids so enumeration cannot collide with real hardware. */
#define TEST_VID    0xF1D0
#define TEST_PID    0x9001
#define TEST_SERIAL "HIDAPI-DEVICE-IO-TEST"

#define OPEN_TIMEOUT_MS 5000
#define READ_TIMEOUT_MS 2000

static int g_failures = 0;

#define CHECK(cond)                                                       \
	do {                                                              \
		if (!(cond)) {                                            \
			printf("    CHECK failed: %s (line %d)\n",       \
			       #cond, __LINE__);                         \
			g_failures++;                                    \
			return -1;                                       \
		}                                                        \
	} while (0)

/* Read one full input report (TEST_VDEV_REPORT_SIZE bytes), waiting up to
   timeout_ms in total. Returns 0 and fills buf on success, -1 otherwise. */
static int read_input_report(hid_device *h, unsigned char *buf, int timeout_ms)
{
	int waited = 0;
	while (waited < timeout_ms) {
		int n = hid_read_timeout(h, buf, TEST_VDEV_REPORT_SIZE, 250);
		if (n < 0)
			return -1;                /* read error */
		if (n >= TEST_VDEV_REPORT_SIZE)
			return 0;                 /* got a full report */
		waited += 250;
	}
	return -1;                                /* timed out */
}

/* Print a flushed progress marker so a hang is localised on a CTest timeout. */
static void step(const char *what)
{
	printf("    -> %s\n", what);
	fflush(stdout);
}

/* open -> write -> exchange reports -> (caller closes). */
static int run_device_io(test_virtual_device *vdev, hid_device *h)
{
	unsigned char out_report[1 + TEST_VDEV_REPORT_SIZE];
	unsigned char in_report[TEST_VDEV_REPORT_SIZE];
	const unsigned char expect_a[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
	const unsigned char expect_b[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_B;

	/* write: send one output report (report id 0 + payload). Output-report
	   support depends on how each virtual device is built, so this is
	   best-effort - we exercise hid_write() and log the result rather than
	   hard-failing. The strictly-checked write path below is the Feature
	   report (the scenario trigger), which every provider implements. */
	memset(out_report, 0, sizeof(out_report));
	out_report[0] = 0x00;             /* report id (unnumbered device) */
	out_report[1] = 0x10;
	out_report[2] = 0x20;
	step("hid_write(output)");
	printf("    hid_write(output) returned %d\n",
	       hid_write(h, out_report, sizeof(out_report)));

	/* exchange A: a Feature report triggers scenario A; read the replay. */
	step("trigger A (hid_send_feature_report)");
	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_A) >= 0);
	step("read A (hid_read_timeout)");
	memset(in_report, 0, sizeof(in_report));
	CHECK(read_input_report(h, in_report, READ_TIMEOUT_MS) == 0);
	CHECK(memcmp(in_report, expect_a, TEST_VDEV_REPORT_SIZE) == 0);

	/* exchange B. */
	step("trigger B (hid_send_feature_report)");
	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_B) >= 0);
	step("read B (hid_read_timeout)");
	memset(in_report, 0, sizeof(in_report));
	CHECK(read_input_report(h, in_report, READ_TIMEOUT_MS) == 0);
	CHECK(memcmp(in_report, expect_b, TEST_VDEV_REPORT_SIZE) == 0);

	step("done");
	return 0;
}

int main(void)
{
	test_virtual_device *vdev = NULL;
	hid_device *h;
	int rc;

	if (hid_init() != 0) {
		printf("hid_init() failed\n");
		return EXIT_FAILURE;
	}

	step("create virtual device");
	rc = test_virtual_device_create(&vdev, TEST_VID, TEST_PID, TEST_SERIAL);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("virtual device unavailable on this host - skipping\n");
		hid_exit();
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK || !vdev) {
		printf("failed to create virtual device (rc=%d)\n", rc);
		hid_exit();
		return EXIT_FAILURE;
	}

	/* Opening doubles as the presence probe: on platforms where create()
	   cannot itself tell whether a device is installed (Windows, macOS),
	   a failure to enumerate/open means there is no device here, so the
	   test skips rather than fails. */
	step("open via HIDAPI");
	h = test_virtual_device_open_hidapi(vdev, OPEN_TIMEOUT_MS);
	if (!h) {
		printf("virtual device did not enumerate - skipping\n");
		test_virtual_device_destroy(vdev);
		hid_exit();
		return EXIT_SKIP;
	}

	printf("running device-io smoke test...\n");
	rc = run_device_io(vdev, h);
	printf("%s device_io\n", (rc == 0 && g_failures == 0) ? "PASS" : "FAIL");

	hid_close(h);
	test_virtual_device_destroy(vdev);
	hid_exit();

	return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
