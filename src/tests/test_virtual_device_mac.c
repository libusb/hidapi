/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 macOS implementation of the virtual HID device test interface,
 backed by IOHIDUserDevice (IOKit).

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * IOHIDUserDevice is the userspace IOKit facility that creates a virtual HID
 * device visible through IOHIDManager - and therefore to the HIDAPI macOS
 * (darwin) backend - without a kext or DriverKit extension. It implements the
 * same pre-recorded "scenario" protocol as the other providers
 * (see test_virtual_device.h): a Feature SET_REPORT whose first payload byte is
 * a TEST_VDEV_CMD_* command makes the device replay the matching canned input
 * report via IOHIDUserDeviceHandleReport().
 *
 * Important: creating an IOHIDUserDevice is gated on macOS by the
 * com.apple.developer.hid.virtual.device entitlement and an interactive
 * Accessibility (TCC) consent prompt, neither of which can be satisfied
 * head-less. So on hosted CI this provider returns TEST_VDEV_UNAVAILABLE and
 * the test is skipped; it is meant to actually run on a developer machine or a
 * self-hosted runner where the consent has been granted.
 *
 * The IOHIDUserDevice* symbols are IOKit SPI (not in the public SDK), so they
 * are resolved at run time with dlsym(); if they are missing the provider also
 * self-skips rather than failing to build/link.
 */

#include "test_virtual_device.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/hid/IOHIDKeys.h>

/* ---- IOHIDUserDevice SPI (resolved via dlsym) ---------------------------- */
typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef IOReturn (*IOHIDUserDeviceReportCallback)(void *refcon,
                                                  IOHIDReportType type,
                                                  uint32_t reportID,
                                                  uint8_t *report,
                                                  CFIndex reportLength);

typedef IOHIDUserDeviceRef (*fn_create)(CFAllocatorRef, CFDictionaryRef);
typedef void (*fn_schedule)(IOHIDUserDeviceRef, CFRunLoopRef, CFStringRef);
typedef void (*fn_unschedule)(IOHIDUserDeviceRef, CFRunLoopRef, CFStringRef);
typedef void (*fn_reg_cb)(IOHIDUserDeviceRef, IOHIDUserDeviceReportCallback, void *);
typedef IOReturn (*fn_handle)(IOHIDUserDeviceRef, uint8_t *, CFIndex);

struct mac_spi {
	fn_create create;
	fn_schedule schedule;
	fn_unschedule unschedule;
	fn_reg_cb reg_get;
	fn_reg_cb reg_set;
	fn_handle handle_report;
};

/* The same vendor-defined report descriptor as the other providers: one 8-byte
 * Input, Output and Feature report, no Report ID. */
static const unsigned char k_report_descriptor[] = {
	0x06, 0x00, 0xFF,       /* Usage Page (Vendor Defined 0xFF00) */
	0x09, 0x01,             /* Usage (0x01)                       */
	0xA1, 0x01,             /* Collection (Application)           */
	0x09, 0x01,             /*   Usage (0x01)                     */
	0x15, 0x00,             /*   Logical Minimum (0)              */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255)            */
	0x75, 0x08,             /*   Report Size (8)                  */
	0x95, 0x08,             /*   Report Count (8)                 */
	0x81, 0x02,             /*   Input (Data,Var,Abs)             */
	0x09, 0x01,             /*   Usage (0x01)                     */
	0x91, 0x02,             /*   Output (Data,Var,Abs)            */
	0x09, 0x01,             /*   Usage (0x01)                     */
	0xB1, 0x02,             /*   Feature (Data,Var,Abs)           */
	0xC0                    /* End Collection                     */
};

static const unsigned char k_input_a[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
static const unsigned char k_input_b[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_B;

struct test_virtual_device {
	struct mac_spi spi;
	void *iokit_handle;             /* dlopen handle */
	IOHIDUserDeviceRef device;

	pthread_t runloop_thread;
	int thread_started;
	CFRunLoopRef runloop;

	pthread_mutex_t lock;
	pthread_cond_t cond;
	int ready;                      /* run loop scheduled and running */

	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];
};

static int load_spi(struct mac_spi *spi, void **out_handle)
{
	void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",
	                 RTLD_LAZY | RTLD_LOCAL);
	if (!h)
		return -1;

	/* dlsym() returns an object pointer; converting it to a function pointer
	   directly is undefined in ISO C (and -pedantic rejects it), so pun via a
	   union. */
#define LOAD_SYM(field, type, name)                                       \
	do {                                                              \
		union { void *obj; type fn; } _u;                        \
		_u.obj = dlsym(h, (name));                               \
		(field) = _u.fn;                                         \
	} while (0)

	LOAD_SYM(spi->create, fn_create, "IOHIDUserDeviceCreate");
	LOAD_SYM(spi->schedule, fn_schedule, "IOHIDUserDeviceScheduleWithRunLoop");
	LOAD_SYM(spi->unschedule, fn_unschedule, "IOHIDUserDeviceUnscheduleFromRunLoop");
	LOAD_SYM(spi->reg_get, fn_reg_cb, "IOHIDUserDeviceRegisterGetReportCallback");
	LOAD_SYM(spi->reg_set, fn_reg_cb, "IOHIDUserDeviceRegisterSetReportCallback");
	LOAD_SYM(spi->handle_report, fn_handle, "IOHIDUserDeviceHandleReport");

#undef LOAD_SYM

	if (!spi->create || !spi->schedule || !spi->reg_set || !spi->handle_report) {
		dlclose(h);
		return -1;
	}
	*out_handle = h;
	return 0;
}

/* SET_REPORT callback == the scenario trigger. */
static IOReturn set_report_cb(void *refcon, IOHIDReportType type,
                              uint32_t reportID, uint8_t *report,
                              CFIndex reportLength)
{
	struct test_virtual_device *dev = (struct test_virtual_device *)refcon;
	unsigned char command = TEST_VDEV_CMD_NONE;
	CFIndex scan = reportLength > 4 ? 4 : reportLength;
	CFIndex i;
	(void)reportID;

	/* Only a *Feature* SET_REPORT is a scenario trigger (per the shared
	   contract and matching the other providers). hid_write() Output reports
	   also arrive through this callback on macOS and must not replay. */
	if (type != kIOHIDReportTypeFeature)
		return kIOReturnSuccess;

	for (i = 0; i < scan; i++) {
		if (report[i] == TEST_VDEV_CMD_EMIT_A || report[i] == TEST_VDEV_CMD_EMIT_B) {
			command = report[i];
			break;
		}
	}

	if (command == TEST_VDEV_CMD_EMIT_A) {
		uint8_t buf[TEST_VDEV_REPORT_SIZE];
		memcpy(buf, k_input_a, sizeof(buf));
		dev->spi.handle_report(dev->device, buf, sizeof(buf));
	} else if (command == TEST_VDEV_CMD_EMIT_B) {
		uint8_t buf[TEST_VDEV_REPORT_SIZE];
		memcpy(buf, k_input_b, sizeof(buf));
		dev->spi.handle_report(dev->device, buf, sizeof(buf));
	}
	return kIOReturnSuccess;
}

/* GET_REPORT callback: benign (zero-filled). */
static IOReturn get_report_cb(void *refcon, IOHIDReportType type,
                              uint32_t reportID, uint8_t *report,
                              CFIndex reportLength)
{
	(void)refcon;
	(void)type;
	(void)reportID;
	if (report && reportLength > 0)
		memset(report, 0, reportLength);
	return kIOReturnSuccess;
}

static void *runloop_thread_fn(void *arg)
{
	struct test_virtual_device *dev = (struct test_virtual_device *)arg;

	dev->runloop = CFRunLoopGetCurrent();
	dev->spi.schedule(dev->device, dev->runloop, kCFRunLoopDefaultMode);

	pthread_mutex_lock(&dev->lock);
	dev->ready = 1;
	pthread_cond_signal(&dev->cond);
	pthread_mutex_unlock(&dev->lock);

	CFRunLoopRun();

	if (dev->spi.unschedule)
		dev->spi.unschedule(dev->device, dev->runloop, kCFRunLoopDefaultMode);
	return NULL;
}

static void dict_set_number(CFMutableDictionaryRef d, CFStringRef key, int value)
{
	CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
	if (n) {
		CFDictionarySetValue(d, key, n);
		CFRelease(n);
	}
}

int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial)
{
	struct test_virtual_device *dev;
	CFMutableDictionaryRef props;
	CFDataRef rd;
	CFStringRef serial_cf;
	int rc;

	if (!out_dev)
		return TEST_VDEV_ERROR;
	*out_dev = NULL;

	dev = (struct test_virtual_device *)calloc(1, sizeof(*dev));
	if (!dev)
		return TEST_VDEV_ERROR;

	dev->vendor_id = vendor_id;
	dev->product_id = product_id;
	snprintf(dev->serial, sizeof(dev->serial), "%s", serial ? serial : "");
	pthread_mutex_init(&dev->lock, NULL);
	pthread_cond_init(&dev->cond, NULL);

	if (load_spi(&dev->spi, &dev->iokit_handle) != 0) {
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_UNAVAILABLE;     /* SPI not present -> skip */
	}

	props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	                                  &kCFTypeDictionaryKeyCallBacks,
	                                  &kCFTypeDictionaryValueCallBacks);
	rd = CFDataCreate(kCFAllocatorDefault, k_report_descriptor,
	                  sizeof(k_report_descriptor));
	serial_cf = CFStringCreateWithCString(kCFAllocatorDefault, dev->serial,
	                                       kCFStringEncodingUTF8);
	if (!props || !rd || !serial_cf) {
		if (props) CFRelease(props);
		if (rd) CFRelease(rd);
		if (serial_cf) CFRelease(serial_cf);
		dlclose(dev->iokit_handle);
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_ERROR;
	}

	CFDictionarySetValue(props, CFSTR("ReportDescriptor"), rd);
	dict_set_number(props, CFSTR(kIOHIDVendorIDKey), vendor_id);
	dict_set_number(props, CFSTR(kIOHIDProductIDKey), product_id);
	CFDictionarySetValue(props, CFSTR(kIOHIDSerialNumberKey), serial_cf);
	CFDictionarySetValue(props, CFSTR(kIOHIDProductKey),
	                     CFSTR("HIDAPI Test Device"));

	dev->device = dev->spi.create(kCFAllocatorDefault, props);

	CFRelease(props);
	CFRelease(rd);
	CFRelease(serial_cf);

	if (!dev->device) {
		/* Entitlement / consent denied (the usual case on hosted CI). */
		dlclose(dev->iokit_handle);
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_UNAVAILABLE;
	}

	dev->spi.reg_set(dev->device, set_report_cb, dev);
	if (dev->spi.reg_get)
		dev->spi.reg_get(dev->device, get_report_cb, dev);

	rc = pthread_create(&dev->runloop_thread, NULL, runloop_thread_fn, dev);
	if (rc != 0) {
		CFRelease(dev->device);
		dlclose(dev->iokit_handle);
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_ERROR;
	}
	dev->thread_started = 1;

	/* Wait until the run loop has scheduled the device. */
	pthread_mutex_lock(&dev->lock);
	while (!dev->ready)
		pthread_cond_wait(&dev->cond, &dev->lock);
	pthread_mutex_unlock(&dev->lock);

	*out_dev = dev;
	return TEST_VDEV_OK;
}

hid_device *test_virtual_device_open_hidapi(test_virtual_device *dev, int timeout_ms)
{
	wchar_t wserial[64];
	int waited = 0;
	size_t i;

	if (!dev)
		return NULL;

	for (i = 0; i + 1 < (sizeof(wserial) / sizeof(wserial[0])) && dev->serial[i]; i++)
		wserial[i] = (wchar_t)(unsigned char)dev->serial[i];
	wserial[i] = L'\0';

	for (;;) {
		struct hid_device_info *infos = hid_enumerate(dev->vendor_id, dev->product_id);
		struct hid_device_info *cur;
		struct hid_device_info *first = NULL;
		struct hid_device_info *match = NULL;
		hid_device *h = NULL;

		for (cur = infos; cur; cur = cur->next) {
			if (!first)
				first = cur;
			if (cur->serial_number && wcscmp(cur->serial_number, wserial) == 0) {
				match = cur;
				break;
			}
		}
		if (match || first)
			h = hid_open_path((match ? match : first)->path);
		hid_free_enumeration(infos);
		if (h)
			return h;

		if (waited >= timeout_ms)
			return NULL;
		struct timespec ts = { 0, 50 * 1000000L };
		nanosleep(&ts, NULL);
		waited += 50;
	}
}

int test_virtual_device_trigger(test_virtual_device *dev, hid_device *handle,
                                unsigned char command)
{
	unsigned char feature[1 + TEST_VDEV_REPORT_SIZE];
	(void)dev;
	memset(feature, 0, sizeof(feature));
	feature[0] = 0x00;        /* Report ID (the device has no numbered reports) */
	feature[1] = command;     /* scenario command, first byte of the payload */
	return hid_send_feature_report(handle, feature, sizeof(feature));
}

void test_virtual_device_destroy(test_virtual_device *dev)
{
	if (!dev)
		return;

	if (dev->thread_started) {
		if (dev->runloop)
			CFRunLoopStop(dev->runloop);
		pthread_join(dev->runloop_thread, NULL);
		dev->thread_started = 0;
	}

	if (dev->device) {
		CFRelease(dev->device);
		dev->device = NULL;
	}
	if (dev->iokit_handle) {
		dlclose(dev->iokit_handle);
		dev->iokit_handle = NULL;
	}

	pthread_cond_destroy(&dev->cond);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
}
