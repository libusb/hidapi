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
#include <cfgmgr32.h>

/*
 * Instance id of the root-enumerated virtual-HID devnode. The CI job creates it
 * with `devcon install VhidminiUm.inf "root\VhidminiUm"`, which installs the
 * first (single) instance of hardware id `root\VhidminiUm` as `ROOT\VHIDMINIUM\0000`.
 * Presence toggling (unplug/replug) is done by disabling/enabling this devnode
 * via cfgmgr32: disabling it tears down the HIDClass child PDO so the
 * GUID_DEVINTERFACE_HID interface disappears (the winapi backend sees a removal);
 * enabling it re-creates the interface (an arrival).
 */
#define VHID_INSTANCE_ID "ROOT\\VHIDMINIUM\\0000"

struct test_virtual_device {
	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];
	ULONG feature_len;      /* FeatureReportByteLength of the opened device */
};

/* Locate the root-enumerated virtual-HID devnode (re-located on every call:
   simplest and robust, and cheap next to the PnP state changes it precedes).
   Returns the CONFIGRET from CM_Locate_DevNodeA verbatim, with *devinst set on
   CR_SUCCESS. CR_NO_SUCH_DEVNODE means the driver/device is not installed on
   this host. A disabled (but not removed) root devnode is still "configured",
   so CM_LOCATE_DEVNODE_NORMAL locates it for the re-enable in replug(). */
static CONFIGRET locate_vhid_devnode(DEVINST *devinst)
{
	char devid[] = VHID_INSTANCE_ID; /* mutable buffer: DEVINSTID_A is non-const */
	return CM_Locate_DevNodeA(devinst, devid, CM_LOCATE_DEVNODE_NORMAL);
}

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
	/* The harness (CI) uninstalls the driver/device after the test. But if a
	   test unplugged (disabled the devnode) and exited before replugging it,
	   re-enable it best-effort so a later run on the same host is not left with
	   a disabled devnode. Locating/enabling an absent, already-enabled, or
	   access-denied node is harmless, so the CONFIGRETs are intentionally
	   ignored here. */
	DEVINST devinst;
	if (locate_vhid_devnode(&devinst) == CR_SUCCESS)
		(void)CM_Enable_DevNode(devinst, 0);
	free(dev);
}

/* Unplug = disable the root devnode. This tears down the HIDClass child PDO, so
 * the GUID_DEVINTERFACE_HID interface disappears and the winapi backend's PnP
 * notification fires a removal (the test then sees the device LEFT / gone from
 * hid_enumerate). This is also the hotplug test's capability probe, so a devnode
 * that cannot be located (driver/device not installed) or that cannot be
 * disabled for lack of elevation (CR_ACCESS_DENIED) returns UNAVAILABLE, which
 * makes the test skip cleanly instead of failing. */
int test_virtual_device_unplug(test_virtual_device *dev)
{
	DEVINST devinst;
	CONFIGRET cr;

	(void)dev; /* the devnode is installed out-of-band by the CI job */

	cr = locate_vhid_devnode(&devinst);
	if (cr == CR_NO_SUCH_DEVNODE)
		return TEST_VDEV_UNAVAILABLE; /* driver/device not installed here */
	if (cr != CR_SUCCESS)
		return TEST_VDEV_ERROR;

	cr = CM_Disable_DevNode(devinst, 0);
	if (cr == CR_SUCCESS)
		return TEST_VDEV_OK;
	if (cr == CR_ACCESS_DENIED)
		return TEST_VDEV_UNAVAILABLE; /* not elevated -> skip, don't fail */
	return TEST_VDEV_ERROR;
}

/* Replug = re-enable the root devnode. The HIDClass child PDO and its HID
 * interface are re-created, so the backend sees an arrival (the device is back
 * in hid_enumerate). Failure here is a hard error, not a skip: if unplug()
 * disabled the devnode we must be able to re-enable it. */
int test_virtual_device_replug(test_virtual_device *dev)
{
	DEVINST devinst;

	(void)dev;

	if (locate_vhid_devnode(&devinst) != CR_SUCCESS)
		return TEST_VDEV_ERROR;

	return (CM_Enable_DevNode(devinst, 0) == CR_SUCCESS)
	           ? TEST_VDEV_OK
	           : TEST_VDEV_ERROR;
}
