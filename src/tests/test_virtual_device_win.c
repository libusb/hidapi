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
#include <ctype.h>
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>

/*
 * The virtual-HID devnode is located by its INF hardware id, not a fixed instance
 * path. The CI job installs it with `devcon install VhidminiUm.inf
 * "root\VhidminiUm"`, but PnP derives the devnode's *instance id* from the driver's
 * setup class (Class=HIDClass in the INF -> observed instance ROOT\HIDCLASS\0000),
 * not from the hardware id, so the instance path is not knowable a priori.
 * locate_vhid_devnode() instead scans the ROOT enumerator for the devnode whose
 * hardware id contains this token (matched case-insensitively).
 *
 * Presence toggling (unplug/replug) disables/enables that devnode via cfgmgr32:
 * disabling it tears down the HIDClass child PDO so the GUID_DEVINTERFACE_HID
 * interface disappears (the winapi backend sees a removal); enabling it re-creates
 * the interface (an arrival).
 */
#define VHID_HARDWARE_ID_MATCH "VHIDMINIUM"

struct test_virtual_device {
	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];
	ULONG feature_len;      /* FeatureReportByteLength of the opened device */
};

/* Case-insensitive: does haystack contain needle (needle already uppercase)? */
static int contains_ci_upper(const char *haystack, const char *needle_upper)
{
	size_t nlen = strlen(needle_upper);
	const char *p;

	if (nlen == 0)
		return 1;
	for (p = haystack; *p != '\0'; ++p) {
		size_t i = 0;
		while (i < nlen && p[i] != '\0' &&
		       (char)toupper((unsigned char)p[i]) == needle_upper[i])
			++i;
		if (i == nlen)
			return 1;
	}
	return 0;
}

/* Locate the root-enumerated virtual-HID devnode by matching its INF hardware id
   (VHID_HARDWARE_ID_MATCH), robust to the PnP-generated instance path and index.
   Every devnode under the ROOT enumerator is scanned - enabled or disabled, since
   a disabled root devnode is still enumerated and "configured" - so both unplug's
   disable and replug's re-enable resolve the same node. Returns CR_SUCCESS with
   *out_devinst set; CR_NO_SUCH_DEVNODE if no such devnode exists (driver/device
   not installed here); otherwise the failing CONFIGRET. */
static CONFIGRET locate_vhid_devnode(DEVINST *out_devinst)
{
	CONFIGRET cr;
	ULONG list_len = 0;
	char *list;
	char *inst;

	cr = CM_Get_Device_ID_List_SizeA(&list_len, "ROOT",
	                                 CM_GETIDLIST_FILTER_ENUMERATOR);
	if (cr != CR_SUCCESS)
		return cr;
	if (list_len < 2)
		return CR_NO_SUCH_DEVNODE;

	list = (char *)malloc(list_len);
	if (!list)
		return CR_OUT_OF_MEMORY;

	cr = CM_Get_Device_ID_ListA("ROOT", list, list_len,
	                            CM_GETIDLIST_FILTER_ENUMERATOR);
	if (cr != CR_SUCCESS) {
		free(list);
		return cr;
	}

	/* The list is a REG_MULTI_SZ of instance ids; walk each one. */
	for (inst = list; *inst != '\0'; inst += strlen(inst) + 1) {
		DEVINST devinst;
		char hwids[512];
		char *h;
		ULONG hwlen = (ULONG)sizeof(hwids);

		if (CM_Locate_DevNodeA(&devinst, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
			continue;
		if (CM_Get_DevNode_Registry_PropertyA(devinst, CM_DRP_HARDWAREID, NULL,
		                                      hwids, &hwlen, 0) != CR_SUCCESS)
			continue;
		/* CM_DRP_HARDWAREID is itself a REG_MULTI_SZ; match any of its ids. */
		for (h = hwids; *h != '\0'; h += strlen(h) + 1) {
			if (contains_ci_upper(h, VHID_HARDWARE_ID_MATCH)) {
				*out_devinst = devinst;
				free(list);
				return CR_SUCCESS;
			}
		}
	}

	free(list);
	return CR_NO_SUCH_DEVNODE;
}

/* Find the HID child PDO of the vhidmini function devnode. Presence toggling acts
   on this leaf (not the function device): disabling/enabling it raises the HID
   interface removal/arrival the winapi backend watches, while leaving the UMDF
   host running - disabling the function device instead re-creates the child in a
   non-started state, so its HID interface never comes back. Prefers the child
   whose instance id is under the HID enumerator; falls back to the first child. */
static CONFIGRET find_hid_child(DEVINST func, DEVINST *out_child)
{
	DEVINST child, first;
	CONFIGRET cr;

	cr = CM_Get_Child(&child, func, 0);
	if (cr != CR_SUCCESS)
		return cr; /* CR_NO_SUCH_DEVNODE if the function device has no child */

	first = child; /* fallback: the function device's first child */
	for (;;) {
		char cid[MAX_DEVICE_ID_LEN];

		if (CM_Get_Device_IDA(child, cid, (ULONG)sizeof(cid), 0) == CR_SUCCESS &&
		    strncmp(cid, "HID\\", 4) == 0) {
			*out_child = child;
			return CR_SUCCESS;
		}
		if (CM_Get_Sibling(&child, child, 0) != CR_SUCCESS)
			break;
	}

	*out_child = first;
	return CR_SUCCESS;
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

	/* Windows cannot create HID devices on the fly; the CI job pre-installs a
	   single static vhidmini device whose identity is fixed (see
	   src/tests/windows/driver). Report UNAVAILABLE for any requested device that
	   is not actually present here - e.g. the mid-pass-stop test's second device -
	   so such tests skip cleanly instead of waiting for one that can never appear. */
	{
		struct hid_device_info *infos = hid_enumerate(vendor_id, product_id);
		if (!infos)
			return TEST_VDEV_UNAVAILABLE;
		hid_free_enumeration(infos);
	}

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
	   test unplugged (disabled the HID child) and exited before replugging it,
	   re-enable the child best-effort so a later run on the same host is not left
	   with a disabled device. Locating/enabling an absent, already-enabled, or
	   access-denied node is harmless, so the CONFIGRETs are intentionally ignored
	   here. */
	DEVINST func, child;
	if (locate_vhid_devnode(&func) == CR_SUCCESS &&
	    find_hid_child(func, &child) == CR_SUCCESS)
		(void)CM_Enable_DevNode(child, 0);
	free(dev);
}

/* Unplug = disable the HID child PDO. Its GUID_DEVINTERFACE_HID interface
 * disappears and the winapi backend's PnP notification fires a removal (the test
 * then sees the device LEFT / gone from hid_enumerate), while the UMDF function
 * device keeps running. This is also the hotplug test's capability probe, so a
 * function devnode that cannot be located (driver/device not installed) or a
 * child that cannot be disabled for lack of elevation (CR_ACCESS_DENIED) returns
 * UNAVAILABLE, which makes the test skip cleanly instead of failing. */
int test_virtual_device_unplug(test_virtual_device *dev)
{
	DEVINST func, child;
	CONFIGRET cr;

	(void)dev; /* the devnode is installed out-of-band by the CI job */

	cr = locate_vhid_devnode(&func);
	if (cr == CR_NO_SUCH_DEVNODE) {
		fprintf(stderr, "[win-vdev] no ROOT devnode with hardware id '%s' "
		                "(driver/device not installed) -> hotplug test skips\n",
		        VHID_HARDWARE_ID_MATCH);
		return TEST_VDEV_UNAVAILABLE; /* driver/device not installed here */
	}
	if (cr != CR_SUCCESS) {
		fprintf(stderr, "[win-vdev] locate failed: CONFIGRET 0x%lX\n",
		        (unsigned long)cr);
		return TEST_VDEV_ERROR;
	}

	cr = find_hid_child(func, &child);
	if (cr != CR_SUCCESS)
		return TEST_VDEV_OK; /* no HID child -> already absent, nothing to disable */

	cr = CM_Disable_DevNode(child, 0);
	if (cr == CR_SUCCESS)
		return TEST_VDEV_OK;
	if (cr == CR_ACCESS_DENIED) {
		fprintf(stderr, "[win-vdev] CM_Disable_DevNode(child) -> CR_ACCESS_DENIED "
		                "(not elevated) -> hotplug test skips\n");
		return TEST_VDEV_UNAVAILABLE; /* not elevated -> skip, don't fail */
	}
	fprintf(stderr, "[win-vdev] CM_Disable_DevNode(child) failed: CONFIGRET 0x%lX\n",
	        (unsigned long)cr);
	return TEST_VDEV_ERROR;
}

/* Replug = re-enable the HID child PDO disabled by unplug(). Its HID interface is
 * re-created, so the backend sees an arrival (the device is back in
 * hid_enumerate). Failure here is a hard error, not a skip: if unplug() disabled
 * the child we must be able to re-enable it. */
int test_virtual_device_replug(test_virtual_device *dev)
{
	DEVINST func, child;
	CONFIGRET cr;

	(void)dev;

	cr = locate_vhid_devnode(&func);
	if (cr != CR_SUCCESS) {
		fprintf(stderr, "[win-vdev] replug locate failed: CONFIGRET 0x%lX\n",
		        (unsigned long)cr);
		return TEST_VDEV_ERROR;
	}

	cr = find_hid_child(func, &child);
	if (cr != CR_SUCCESS) {
		/* The child devnode is gone entirely (not merely disabled); ask the
		   function device to re-report it, then retry. */
		(void)CM_Reenumerate_DevNode(func, CM_REENUMERATE_SYNCHRONOUS);
		cr = find_hid_child(func, &child);
		if (cr != CR_SUCCESS) {
			fprintf(stderr, "[win-vdev] replug: no HID child to enable: CONFIGRET 0x%lX\n",
			        (unsigned long)cr);
			return TEST_VDEV_ERROR;
		}
	}

	cr = CM_Enable_DevNode(child, 0);
	if (cr != CR_SUCCESS) {
		fprintf(stderr, "[win-vdev] CM_Enable_DevNode(child) failed: CONFIGRET 0x%lX\n",
		        (unsigned long)cr);
		return TEST_VDEV_ERROR;
	}

	return TEST_VDEV_OK;
}
