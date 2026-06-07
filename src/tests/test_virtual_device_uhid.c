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
 * the HIDAPI hidraw backend can enumerate and open. The pre-recorded scenarios
 * (see test_virtual_device.h) are played back here: the uhid event pump watches
 * for a Feature SET_REPORT whose first byte is a TEST_VDEV_CMD_* command and
 * replays the matching canned input report. Requires the 'uhid' module and
 * (typically) root to open /dev/uhid; otherwise create() returns
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

	pthread_mutex_t write_lock;   /* serializes writes to the uhid fd */
};

/* A generic vendor-defined descriptor: one 8-byte Input, Output and Feature
 * report, no Report ID. With no Report ID, hid_read() returns the replayed
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

static const unsigned char k_input_a[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
static const unsigned char k_input_b[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_B;

/* write() is marked warn_unused_result by glibc; consume the result for the
 * best-effort writes (replies, teardown). */
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

/* Send one input report (size TEST_VDEV_REPORT_SIZE) to the host. */
static void emit_input(struct test_virtual_device *dev, const unsigned char *payload)
{
	struct uhid_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_INPUT2;
	ev.u.input2.size = TEST_VDEV_REPORT_SIZE;
	memcpy(ev.u.input2.data, payload, TEST_VDEV_REPORT_SIZE);

	pthread_mutex_lock(&dev->write_lock);
	write_event_best_effort(dev->fd, &ev);
	pthread_mutex_unlock(&dev->write_lock);
}

/* A Feature SET_REPORT carries a scenario command. Depending on whether the
 * report is numbered, the payload the kernel hands to uhid may be prefixed by
 * a report-number byte, so locate the command by scanning the first few bytes
 * for a recognised TEST_VDEV_CMD_* value (the report-number byte is 0 for our
 * unnumbered device, and the commands are non-zero, so this is unambiguous). */
static void handle_set_report(struct test_virtual_device *dev, const struct uhid_event *in)
{
	struct uhid_event out;
	unsigned char command = TEST_VDEV_CMD_NONE;
	uint16_t scan = in->u.set_report.size;
	uint16_t i;
	if (scan > 4)
		scan = 4;
	for (i = 0; i < scan; i++) {
		unsigned char b = in->u.set_report.data[i];
		if (b == TEST_VDEV_CMD_EMIT_A || b == TEST_VDEV_CMD_EMIT_B) {
			command = b;
			break;
		}
	}

	memset(&out, 0, sizeof(out));
	out.type = UHID_SET_REPORT_REPLY;
	out.u.set_report_reply.id = in->u.set_report.id;
	out.u.set_report_reply.err = 0;
	pthread_mutex_lock(&dev->write_lock);
	write_event_best_effort(dev->fd, &out);
	pthread_mutex_unlock(&dev->write_lock);

	switch (command) {
	case TEST_VDEV_CMD_EMIT_A:
		emit_input(dev, k_input_a);
		break;
	case TEST_VDEV_CMD_EMIT_B:
		emit_input(dev, k_input_b);
		break;
	default:
		break;
	}
}

/* Answer feature GET_REPORT requests benignly (empty payload). */
static void handle_get_report(struct test_virtual_device *dev, const struct uhid_event *in)
{
	struct uhid_event out;
	memset(&out, 0, sizeof(out));
	out.type = UHID_GET_REPORT_REPLY;
	out.u.get_report_reply.id = in->u.get_report.id;
	out.u.get_report_reply.err = 0;
	out.u.get_report_reply.size = 0;
	pthread_mutex_lock(&dev->write_lock);
	write_event_best_effort(dev->fd, &out);
	pthread_mutex_unlock(&dev->write_lock);
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
		case UHID_SET_REPORT:
			handle_set_report(dev, &ev);
			break;
		case UHID_GET_REPORT:
			handle_get_report(dev, &ev);
			break;
		default:
			/* UHID_START / UHID_OPEN / UHID_CLOSE / UHID_STOP / UHID_OUTPUT */
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
	pthread_mutex_init(&dev->write_lock, NULL);

	dev->fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (dev->fd < 0) {
		int e = errno;
		pthread_mutex_destroy(&dev->write_lock);
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
		pthread_mutex_destroy(&dev->write_lock);
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
		pthread_mutex_destroy(&dev->write_lock);
		free(dev);
		return TEST_VDEV_ERROR;
	}
	dev->pump_started = 1;

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

	/* The hidraw node and its udev attributes appear asynchronously after
	   UHID_CREATE2; poll enumeration until the device shows up. Match by the
	   (test-unique) VID/PID, preferring the entry whose serial matches. */
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

	pthread_mutex_destroy(&dev->write_lock);
	free(dev);
}
