/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Linux implementation of the virtual HID device test interface,
 backed by the kernel's userspace-HID interface (/dev/uhid).

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * Creating a uhid device makes the kernel expose a real /dev/hidrawN node that
 * the HIDAPI hidraw backend can enumerate and open, so the tests drive the
 * public HIDAPI Linux code paths against a deterministic device with no
 * physical hardware. Requires the 'uhid' module and (typically) root to open
 * /dev/uhid; when that isn't possible, test_virtual_device_create() returns
 * TEST_VDEV_UNAVAILABLE so the test is skipped rather than failed.
 */

#include "test_virtual_device.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <linux/uhid.h>

struct test_virtual_device {
	int fd;                       /* /dev/uhid file descriptor */
	pthread_t pump_thread;
	int pump_started;
	volatile int stop;            /* tells the event pump to exit */

	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];              /* uhid 'uniq' == HIDAPI serial_number */

	pthread_mutex_t lock;         /* protects buffers below + serializes fd writes */

	unsigned char feature[64];    /* bytes returned for a feature GET_REPORT */
	size_t feature_len;

	unsigned char last_output[64];/* last OUTPUT/SET_REPORT payload received */
	size_t last_output_len;
};

/* A generic vendor-defined descriptor: one 8-byte Input, Output and Feature
 * report, no Report ID. With no Report ID, hid_read() returns the injected
 * input bytes verbatim, which keeps the tests easy to reason about. */
static const unsigned char k_report_descriptor[] = {
	0x06, 0x00, 0xFF,       /* Usage Page (Vendor Defined 0xFF00)      */
	0x09, 0x01,             /* Usage (0x01)                            */
	0xA1, 0x01,             /* Collection (Application)                */
	0x09, 0x01,             /*   Usage (0x01)                          */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                 */
	0x75, 0x08,             /*   Report Size (8)                       */
	0x95, 0x08,             /*   Report Count (8)                      */
	0x81, 0x02,             /*   Input (Data,Var,Abs)                  */
	0x09, 0x01,             /*   Usage (0x01)                          */
	0x91, 0x02,             /*   Output (Data,Var,Abs)                 */
	0x09, 0x01,             /*   Usage (0x01)                          */
	0xB1, 0x02,             /*   Feature (Data,Var,Abs)                */
	0xC0                    /* End Collection                          */
};

/* write() is marked warn_unused_result by glibc; this wrapper consumes the
 * result for the best-effort writes where there's nothing useful to do on
 * failure (replies, teardown). */
static void write_event_best_effort(int fd, const struct uhid_event *ev)
{
	ssize_t r = write(fd, ev, sizeof(*ev));
	(void)r;
}

static void sleep_ms(int ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* Serialize writes to the uhid fd (the main thread injects input while the
 * pump thread writes GET/SET_REPORT replies). */
static int uhid_write_event(struct test_virtual_device *dev, const struct uhid_event *ev)
{
	ssize_t ret;
	pthread_mutex_lock(&dev->lock);
	ret = write(dev->fd, ev, sizeof(*ev));
	pthread_mutex_unlock(&dev->lock);
	return ret < 0 ? TEST_VDEV_ERROR : TEST_VDEV_OK;
}

static void handle_get_report(struct test_virtual_device *dev, const struct uhid_event *in)
{
	struct uhid_event out;
	memset(&out, 0, sizeof(out));
	out.type = UHID_GET_REPORT_REPLY;
	out.u.get_report_reply.id = in->u.get_report.id;

	pthread_mutex_lock(&dev->lock);
	if (in->u.get_report.rtype == UHID_FEATURE_REPORT && dev->feature_len > 0) {
		uint16_t n = (uint16_t)dev->feature_len;
		out.u.get_report_reply.err = 0;
		out.u.get_report_reply.size = n;
		memcpy(out.u.get_report_reply.data, dev->feature, n);
	} else {
		out.u.get_report_reply.err = 0;
		out.u.get_report_reply.size = 0;
	}
	write_event_best_effort(dev->fd, &out);
	pthread_mutex_unlock(&dev->lock);
}

static void handle_set_report(struct test_virtual_device *dev, const struct uhid_event *in)
{
	struct uhid_event out;
	uint16_t n = in->u.set_report.size;
	if (n > sizeof(dev->last_output))
		n = (uint16_t)sizeof(dev->last_output);

	pthread_mutex_lock(&dev->lock);
	memcpy(dev->last_output, in->u.set_report.data, n);
	dev->last_output_len = n;

	memset(&out, 0, sizeof(out));
	out.type = UHID_SET_REPORT_REPLY;
	out.u.set_report_reply.id = in->u.set_report.id;
	out.u.set_report_reply.err = 0;
	write_event_best_effort(dev->fd, &out);
	pthread_mutex_unlock(&dev->lock);
}

static void handle_output(struct test_virtual_device *dev, const struct uhid_event *in)
{
	uint16_t n = in->u.output.size;
	if (n > sizeof(dev->last_output))
		n = (uint16_t)sizeof(dev->last_output);
	pthread_mutex_lock(&dev->lock);
	memcpy(dev->last_output, in->u.output.data, n);
	dev->last_output_len = n;
	pthread_mutex_unlock(&dev->lock);
}

static void *pump_thread_fn(void *arg)
{
	struct test_virtual_device *dev = (struct test_virtual_device *)arg;

	while (!dev->stop) {
		struct pollfd pfd;
		int pret;
		ssize_t rret;
		struct uhid_event ev;

		pfd.fd = dev->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		pret = poll(&pfd, 1, 100);
		if (pret <= 0)
			continue;       /* timeout (re-check stop) or EINTR */

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;

		memset(&ev, 0, sizeof(ev));
		rret = read(dev->fd, &ev, sizeof(ev));
		if (rret < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}

		switch (ev.type) {
		case UHID_GET_REPORT:
			handle_get_report(dev, &ev);
			break;
		case UHID_SET_REPORT:
			handle_set_report(dev, &ev);
			break;
		case UHID_OUTPUT:
			handle_output(dev, &ev);
			break;
		default:
			/* UHID_START / UHID_OPEN / UHID_CLOSE / UHID_STOP: ignore */
			break;
		}
	}

	return NULL;
}

int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial)
{
	struct test_virtual_device *dev;
	struct uhid_event ev;
	int rc;

	if (!out_dev)
		return TEST_VDEV_ERROR;
	*out_dev = NULL;

	dev = (struct test_virtual_device *)calloc(1, sizeof(*dev));
	if (!dev)
		return TEST_VDEV_ERROR;

	dev->fd = -1;
	dev->vendor_id = vendor_id;
	dev->product_id = product_id;
	snprintf(dev->serial, sizeof(dev->serial), "%s", serial ? serial : "");
	pthread_mutex_init(&dev->lock, NULL);

	dev->fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (dev->fd < 0) {
		int e = errno;
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		if (e == ENOENT || e == EACCES || e == EPERM || e == ENODEV)
			return TEST_VDEV_UNAVAILABLE;
		return TEST_VDEV_ERROR;
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "HIDAPI Test Device");
	snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "%s", dev->serial);
	memcpy(ev.u.create2.rd_data, k_report_descriptor, sizeof(k_report_descriptor));
	ev.u.create2.rd_size = (uint16_t)sizeof(k_report_descriptor);
	ev.u.create2.bus = 0x03;          /* BUS_USB */
	ev.u.create2.vendor = vendor_id;
	ev.u.create2.product = product_id;
	ev.u.create2.version = 0;
	ev.u.create2.country = 0;

	if (write(dev->fd, &ev, sizeof(ev)) < 0) {
		int e = errno;
		close(dev->fd);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		if (e == EACCES || e == EPERM)
			return TEST_VDEV_UNAVAILABLE;
		return TEST_VDEV_ERROR;
	}

	rc = pthread_create(&dev->pump_thread, NULL, pump_thread_fn, dev);
	if (rc != 0) {
		memset(&ev, 0, sizeof(ev));
		ev.type = UHID_DESTROY;
		write_event_best_effort(dev->fd, &ev);
		close(dev->fd);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_ERROR;
	}
	dev->pump_started = 1;

	*out_dev = dev;
	return TEST_VDEV_OK;
}

int test_virtual_device_send_input(test_virtual_device *dev,
                                   const unsigned char *data, size_t length)
{
	struct uhid_event ev;

	if (!dev || dev->fd < 0)
		return TEST_VDEV_ERROR;
	if (length > sizeof(ev.u.input2.data))
		return TEST_VDEV_ERROR;

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_INPUT2;
	ev.u.input2.size = (uint16_t)length;
	if (length > 0)
		memcpy(ev.u.input2.data, data, length);

	return uhid_write_event(dev, &ev);
}

void test_virtual_device_set_feature(test_virtual_device *dev,
                                     const unsigned char *data, size_t length)
{
	if (!dev)
		return;
	if (length > sizeof(dev->feature))
		length = sizeof(dev->feature);
	pthread_mutex_lock(&dev->lock);
	memcpy(dev->feature, data, length);
	dev->feature_len = length;
	pthread_mutex_unlock(&dev->lock);
}

size_t test_virtual_device_last_output(test_virtual_device *dev,
                                       unsigned char *data, size_t length)
{
	size_t n;
	if (!dev)
		return 0;
	pthread_mutex_lock(&dev->lock);
	n = dev->last_output_len < length ? dev->last_output_len : length;
	memcpy(data, dev->last_output, n);
	pthread_mutex_unlock(&dev->lock);
	return n;
}

hid_device *test_virtual_device_open_hidapi(test_virtual_device *dev, int timeout_ms)
{
	wchar_t wserial[64];
	int waited = 0;
	size_t i;

	if (!dev)
		return NULL;

	/* Widen the ASCII serial for comparison against hid_device_info. */
	for (i = 0; i + 1 < (sizeof(wserial) / sizeof(wserial[0])) && dev->serial[i]; i++)
		wserial[i] = (wchar_t)(unsigned char)dev->serial[i];
	wserial[i] = L'\0';

	/* The hidraw node and its udev attributes appear asynchronously after
	   UHID_CREATE2; poll enumeration until the device shows up. Match by the
	   (test-unique) VID/PID, preferring the entry whose serial matches in
	   case the host happens to have another device with the same ids. */
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
		sleep_ms(50);
		waited += 50;
	}
}

void test_virtual_device_destroy(test_virtual_device *dev)
{
	struct uhid_event ev;

	if (!dev)
		return;

	if (dev->pump_started) {
		dev->stop = 1;
		pthread_join(dev->pump_thread, NULL);
		dev->pump_started = 0;
	}

	if (dev->fd >= 0) {
		memset(&ev, 0, sizeof(ev));
		ev.type = UHID_DESTROY;
		write_event_best_effort(dev->fd, &ev);
		close(dev->fd);
		dev->fd = -1;
	}

	pthread_mutex_destroy(&dev->lock);
	free(dev);
}
