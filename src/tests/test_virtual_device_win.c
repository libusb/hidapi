/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Windows implementation of the virtual HID device test interface.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * On Windows there is no userspace facility to create a HID device on the fly
 * (unlike Linux /dev/uhid). A small UMDF virtual HID driver
 * (src/tests/windows/driver, a modified vhidmini2) is built, signed and
 * installed out-of-band by the CI job before the test runs, and removed
 * afterwards. That driver implements the same pre-recorded scenario protocol
 * as the Linux uhid provider (see test_virtual_device.h).
 *
 * create() just records the ids; presence is confirmed by open_hidapi(), which
 * also caches the device's feature-report length so that trigger() can send a
 * feature report of exactly the size Windows requires.
 */

#include "test_virtual_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

struct test_virtual_device {
	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];
	ULONG feature_len;      /* FeatureReportByteLength of the opened device */
};

int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial)
{
	struct test_virtual_device *dev;

	if (!out_dev)
		return TEST_VDEV_ERROR;
	*out_dev = NULL;

	dev = (struct test_virtual_device *)calloc(1, sizeof(*dev));
	if (!dev)
		return TEST_VDEV_ERROR;

	dev->vendor_id = vendor_id;
	dev->product_id = product_id;
	if (serial)
		strncpy_s(dev->serial, sizeof(dev->serial), serial, _TRUNCATE);

	/* The device (if any) is installed by the harness; presence is verified
	   by open_hidapi(). */
	*out_dev = dev;
	return TEST_VDEV_OK;
}

/* Query the device's HID caps (report byte lengths) directly from Windows. */
static ULONG query_feature_len(const char *path)
{
	HANDLE h;
	PHIDP_PREPARSED_DATA pp = NULL;
	ULONG feat = 0;

	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
	                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;

	if (HidD_GetPreparsedData(h, &pp)) {
		HIDP_CAPS caps;
		if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
			printf("    device caps: input=%u output=%u feature=%u\n",
			       (unsigned)caps.InputReportByteLength,
			       (unsigned)caps.OutputReportByteLength,
			       (unsigned)caps.FeatureReportByteLength);
			feat = caps.FeatureReportByteLength;
		}
		HidD_FreePreparsedData(pp);
	}
	CloseHandle(h);
	return feat;
}

hid_device *test_virtual_device_open_hidapi(test_virtual_device *dev, int timeout_ms)
{
	int waited = 0;

	if (!dev)
		return NULL;

	for (;;) {
		struct hid_device_info *infos = hid_enumerate(dev->vendor_id, dev->product_id);
		hid_device *h = NULL;

		if (infos) {
			if (dev->feature_len == 0)
				dev->feature_len = query_feature_len(infos->path);
			h = hid_open_path(infos->path);
		}
		hid_free_enumeration(infos);
		if (h)
			return h;

		if (waited >= timeout_ms)
			return NULL;
		Sleep(50);
		waited += 50;
	}
}

int test_virtual_device_trigger(test_virtual_device *dev, hid_device *handle,
                                unsigned char command)
{
	unsigned char feature[256];
	size_t len = (1 + TEST_VDEV_REPORT_SIZE);

	/* Windows requires the feature buffer to be exactly FeatureReportByteLength. */
	if (dev && dev->feature_len > 0 && dev->feature_len <= sizeof(feature))
		len = dev->feature_len;

	memset(feature, 0, sizeof(feature));
	feature[0] = 0x00;        /* Report ID (the device has no numbered reports) */
	feature[1] = command;     /* scenario command, first byte of the payload */
	return hid_send_feature_report(handle, feature, len);
}

void test_virtual_device_destroy(test_virtual_device *dev)
{
	/* The harness uninstalls the driver/device after the test. */
	free(dev);
}
