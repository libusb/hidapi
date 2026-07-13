/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Test support: an opaque "virtual HID device" used by the
 HIDAPI unit tests.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * This is a backend-agnostic interface for a virtual HID device that the unit
 * tests drive through the public HIDAPI API. The test scenarios are written
 * only against this interface, so the same scenarios can be reused for any
 * backend by providing a platform-specific implementation:
 *
 *   - Linux:   test_virtual_device_uhid.c  (kernel /dev/uhid -> hidraw)
 *   - others:  (future) e.g. a Windows/macOS provider
 *
 * Only the Linux (uhid) provider exists today; see src/tests/CMakeLists.txt.
 */

#ifndef HIDAPI_TEST_VIRTUAL_DEVICE_H__
#define HIDAPI_TEST_VIRTUAL_DEVICE_H__

#include <stddef.h>

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

/* Opaque virtual HID device; the implementation owns the concrete struct. */
typedef struct test_virtual_device test_virtual_device;

/*
 * Create a virtual HID device, discoverable by HIDAPI with the given ids and
 * serial number. On success, *out_dev receives the device and TEST_VDEV_OK is
 * returned. Returns TEST_VDEV_UNAVAILABLE when the platform's virtual-device
 * mechanism isn't usable (missing kernel support, insufficient privileges,
 * ...) so the caller can report the test as skipped, or TEST_VDEV_ERROR on a
 * hard failure.
 */
int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial);

/* Inject an input report. The bytes are delivered verbatim to hid_read*(). */
int test_virtual_device_send_input(test_virtual_device *dev,
                                   const unsigned char *data, size_t length);

/* Set the bytes the device returns for a feature GET_REPORT request. */
void test_virtual_device_set_feature(test_virtual_device *dev,
                                     const unsigned char *data, size_t length);

/* Copy out the most recent OUTPUT/SET_REPORT payload the device received.
 * Returns the number of bytes copied (0 if none yet). */
size_t test_virtual_device_last_output(test_virtual_device *dev,
                                       unsigned char *data, size_t length);

/*
 * Wait (up to timeout_ms) for the virtual device to appear in HIDAPI
 * enumeration, then hid_open() it. Returns NULL on timeout/failure.
 */
hid_device *test_virtual_device_open_hidapi(test_virtual_device *dev, int timeout_ms);

/* Destroy the virtual device and free all resources. */
void test_virtual_device_destroy(test_virtual_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* HIDAPI_TEST_VIRTUAL_DEVICE_H__ */
