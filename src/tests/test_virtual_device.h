/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Test support: an opaque "virtual HID device" used by the
 HIDAPI unit tests.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * A backend-agnostic virtual HID device for the unit tests. The device has a
 * fixed report descriptor (one Input, Output and Feature report of
 * TEST_VDEV_REPORT_SIZE bytes, no Report ID) and a small set of *pre-recorded
 * scenarios* baked into the device itself.
 *
 * Rather than letting the test inject arbitrary input (which would require
 * platform-specific plumbing in the test), the test triggers a scenario using
 * the ordinary public HIDAPI API - it sends a Feature report whose first byte
 * is a TEST_VDEV_CMD_* command - and the device replays the corresponding
 * pre-recorded input report(s). This keeps the test code 100% platform-neutral
 * (it only ever calls public hid_*() functions); all device behaviour lives in
 * the per-backend provider:
 *
 *   - Linux:   test_virtual_device_uhid.c   (kernel /dev/uhid -> hidraw)
 *   - Windows: test_virtual_device_win.c     (modified vhidmini2 UMDF driver)
 *   - others:  (future)
 *
 * The provider only needs to implement create / open / destroy; the scenario
 * playback is part of the virtual device (the uhid event pump, or the driver).
 */

#ifndef HIDAPI_TEST_VIRTUAL_DEVICE_H__
#define HIDAPI_TEST_VIRTUAL_DEVICE_H__

#include <stddef.h>
#include <string.h>

#include <hidapi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* test_virtual_device_create() return codes. */
#define TEST_VDEV_OK            0
#define TEST_VDEV_ERROR       (-1)
#define TEST_VDEV_UNAVAILABLE (-2)  /* backend can't create a device here -> skip */

/* Input/Output/Feature report payload size of the virtual device. */
#define TEST_VDEV_REPORT_SIZE 8

/*
 * Scenario commands. The test selects a scenario by sending a Feature report
 * whose first payload byte is one of these. The device then replays the
 * matching pre-recorded input report(s). These values, and the payloads below,
 * are the contract shared between the test and every provider (including the
 * Windows driver, which hard-codes the same constants).
 */
#define TEST_VDEV_CMD_NONE   0x00
#define TEST_VDEV_CMD_EMIT_A 0x01   /* device emits TEST_VDEV_INPUT_A once */
#define TEST_VDEV_CMD_EMIT_B 0x02   /* device emits TEST_VDEV_INPUT_B once */

/* Pre-recorded input report payloads (each TEST_VDEV_REPORT_SIZE bytes). */
#define TEST_VDEV_INPUT_A { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 }
#define TEST_VDEV_INPUT_B { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8 }

/* Opaque virtual HID device; the provider owns the concrete struct. */
typedef struct test_virtual_device test_virtual_device;

/*
 * Create a virtual HID device discoverable by HIDAPI with the given ids and
 * serial. On success *out_dev receives the device and TEST_VDEV_OK is returned.
 * Returns TEST_VDEV_UNAVAILABLE when the platform's virtual-device mechanism
 * isn't usable (missing kernel support, insufficient privileges, ...) so the
 * caller can report the test as skipped, or TEST_VDEV_ERROR on a hard failure.
 */
int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial);

/*
 * Wait (up to timeout_ms) for the virtual device to appear in HIDAPI
 * enumeration, then hid_open() it. Returns NULL on timeout/failure.
 */
hid_device *test_virtual_device_open_hidapi(test_virtual_device *dev, int timeout_ms);

/* Destroy the virtual device and free all resources. */
void test_virtual_device_destroy(test_virtual_device *dev);

/*
 * Trigger a pre-recorded scenario on the device by sending the given command
 * as the first byte of a Feature report, using the ordinary public HIDAPI
 * hid_send_feature_report(). The exact wire length of a feature report is
 * platform-specific (Windows requires the buffer to be exactly the device's
 * FeatureReportByteLength), so this is implemented per-provider.
 *
 * Returns >= 0 on success, -1 on error.
 */
int test_virtual_device_trigger(test_virtual_device *dev, hid_device *handle,
                                unsigned char command);

#ifdef __cplusplus
}
#endif

#endif /* HIDAPI_TEST_VIRTUAL_DEVICE_H__ */
