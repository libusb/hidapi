/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Linux libusb-backend implementation of the virtual HID device test
 interface, backed by the kernel's USB Raw Gadget (/dev/raw-gadget)
 on top of the dummy_hcd virtual UDC/HCD.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/*
 * Raw Gadget lets a userspace process *be* a USB device: it answers every EP0
 * control request and drives the endpoints itself. Combined with dummy_hcd
 * (which provides a virtual UDC and a virtual host controller on the same
 * machine), this enumerates a real virtual USB HID device that the HIDAPI
 * libusb backend can open - exercising the USB transfer paths that the hidraw
 * backend never touches.
 *
 * This provider implements the same pre-recorded "scenario" protocol as the
 * other providers (see test_virtual_device.h): a Feature SET_REPORT whose first
 * payload byte is a TEST_VDEV_CMD_* command makes the device send the matching
 * canned input report over its interrupt IN endpoint.
 *
 * Requirements: the 'raw_gadget' and 'dummy_hcd' kernel modules and root (to
 * open /dev/raw-gadget). When unavailable, create() returns
 * TEST_VDEV_UNAVAILABLE so the test is skipped rather than failed.
 *
 * Note: this is little-endian oriented (multi-byte USB descriptor fields are
 * assigned directly); it is meant for CI on common little-endian hosts.
 */

#include "test_virtual_device.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <linux/types.h>
#include <linux/usb/ch9.h>

/* ---- Vendored subset of <linux/usb/raw_gadget.h> (kernel >= 5.7) ---------- */
/* Vendored so this file builds even where the UAPI header is absent. The
 * structs use C99 flexible array members so their sizeof() (and thus the ioctl
 * command numbers) match the kernel's. */

#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
	__u8 driver_name[UDC_NAME_LENGTH_MAX];
	__u8 device_name[UDC_NAME_LENGTH_MAX];
	__u8 speed;
};

enum usb_raw_event_type {
	USB_RAW_EVENT_INVALID = 0,
	USB_RAW_EVENT_CONNECT = 1,
	USB_RAW_EVENT_CONTROL = 2
};

struct usb_raw_event {
	__u32 type;
	__u32 length;
	__u8 data[];
};

struct usb_raw_ep_io {
	__u16 ep;
	__u16 flags;
	__u32 length;
	__u8 data[];
};

#define USB_RAW_EPS_NUM_MAX 30
#define USB_RAW_EP_NAME_MAX 16
#define USB_RAW_EP_ADDR_ANY 0xff

struct usb_raw_ep_caps {
	unsigned int type_control : 1;
	unsigned int type_iso : 1;
	unsigned int type_bulk : 1;
	unsigned int type_int : 1;
	unsigned int dir_in : 1;
	unsigned int dir_out : 1;
};

struct usb_raw_ep_limits {
	__u16 maxpacket_limit;
	__u16 max_streams;
	__u32 reserved;
};

struct usb_raw_ep_info {
	__u8 name[USB_RAW_EP_NAME_MAX];
	__u32 addr;
	struct usb_raw_ep_caps caps;
	struct usb_raw_ep_limits limits;
};

struct usb_raw_eps_info {
	struct usb_raw_ep_info eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT        _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN         _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE   _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ    _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE   _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE  _IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE    _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ     _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE   _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW   _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO    _IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL   _IO('U', 12)

/* ---- HID class descriptor (not in ch9.h) --------------------------------- */

struct hid_class_desc {
	__u8 bLength;
	__u8 bDescriptorType;     /* 0x21 HID */
	__u16 bcdHID;
	__u8 bCountryCode;
	__u8 bNumDescriptors;
	__u8 bReportType;         /* 0x22 Report */
	__u16 wReportLength;
} __attribute__((packed));

#define HID_DT_HID    0x21
#define HID_DT_REPORT 0x22

#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_SET_REPORT 0x09

/* The same vendor-defined report descriptor as the uhid provider: one 8-byte
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
	int fd;                         /* /dev/raw-gadget */
	pthread_t ep0_thread;
	pthread_t int_in_thread;
	int ep0_started;
	int int_in_started;
	volatile int stop;
	volatile int ep0_exited;        /* ep0 thread has left its fetch loop */

	volatile int configured;        /* SET_CONFIGURATION seen, IN ep enabled */
	int int_in_ep;                  /* raw-gadget handle for the IN endpoint */
	__u8 int_in_addr;               /* bEndpointAddress chosen from EPS_INFO */

	pthread_mutex_t lock;
	pthread_cond_t cond;
	volatile unsigned char pending; /* TEST_VDEV_CMD_* to replay, or NONE */

	unsigned short vendor_id;
	unsigned short product_id;
	char serial[64];
};

static void sleep_ms(int ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* SIGUSR1 has a no-op handler installed WITHOUT SA_RESTART so that sending it
   to a worker thread interrupts a blocking raw-gadget ioctl (EVENT_FETCH /
   EP_WRITE) with EINTR, letting the thread observe 'stop' and exit at teardown. */
static void rg_sig_noop(int sig)
{
	(void)sig;
}

static void rg_install_signal(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = rg_sig_noop;
	sigaction(SIGUSR1, &sa, NULL);
}

/* EP0/EP I/O via a heap buffer sized for the flexible-array struct (heap memory
 * has no declared type, so this is alignment- and aliasing-safe). */
static int ep_io_write(int fd, unsigned long request, int ep,
                       const void *data, int len)
{
	struct usb_raw_ep_io *io;
	int rv;
	if (len < 0)
		len = 0;
	io = (struct usb_raw_ep_io *)calloc(1, sizeof(*io) + (size_t)len);
	if (!io)
		return -1;
	io->ep = (__u16)ep;
	io->flags = 0;
	io->length = (__u32)len;
	if (data && len)
		memcpy(io->data, data, (size_t)len);
	rv = ioctl(fd, request, io);
	free(io);
	return rv;
}

static int ep0_write(int fd, const void *data, int len)
{
	return ep_io_write(fd, USB_RAW_IOCTL_EP0_WRITE, 0, data, len);
}

static int ep0_read(int fd, void *data, int maxlen)
{
	struct usb_raw_ep_io *io;
	int rv;
	if (maxlen < 0)
		maxlen = 0;
	io = (struct usb_raw_ep_io *)calloc(1, sizeof(*io) + (size_t)maxlen);
	if (!io)
		return -1;
	io->ep = 0;
	io->flags = 0;
	io->length = (__u32)maxlen;
	rv = ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
	if (rv > 0 && data) {
		int cp = (rv > maxlen) ? maxlen : rv;
		memcpy(data, io->data, (size_t)cp);
	}
	free(io);
	return rv;
}

/* Acknowledge a control transfer that has no data stage. raw-gadget completes
   the status stage with an EP0 transfer in the request's direction: EP0_WRITE
   for IN requests, EP0_READ for OUT requests (e.g. SET_CONFIGURATION). Using
   the wrong one leaves the transfer incomplete and the host times out. */
static void ep0_ack(int fd, const struct usb_ctrlrequest *ctrl)
{
	if (ctrl->bRequestType & USB_DIR_IN)
		ep0_write(fd, NULL, 0);
	else
		ep0_read(fd, NULL, 0);
}

/* Build the full configuration descriptor (config + interface + HID + ep). */
static int build_config_descriptor(unsigned char *buf, int buflen, __u8 ep_addr)
{
	struct usb_config_descriptor cfg;
	struct usb_interface_descriptor intf;
	struct hid_class_desc hid;
	struct usb_endpoint_descriptor ep;
	/* On-the-wire sizes: struct usb_endpoint_descriptor is 9 bytes (it carries
	   2 trailing audio-only fields), but a non-audio endpoint descriptor is 7
	   bytes (USB_DT_ENDPOINT_SIZE). Use the wire sizes so no stray bytes follow
	   a descriptor (which the host would parse as a bogus length-0 descriptor). */
	int total = USB_DT_CONFIG_SIZE + USB_DT_INTERFACE_SIZE +
	            (int)sizeof(hid) + USB_DT_ENDPOINT_SIZE;
	int off = 0;

	if (buflen < total)
		return -1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.bLength = USB_DT_CONFIG_SIZE;
	cfg.bDescriptorType = USB_DT_CONFIG;
	cfg.wTotalLength = (__u16)total;
	cfg.bNumInterfaces = 1;
	cfg.bConfigurationValue = 1;
	cfg.iConfiguration = 0;
	cfg.bmAttributes = 0x80;       /* bus powered */
	cfg.bMaxPower = 50;            /* 100 mA */

	memset(&intf, 0, sizeof(intf));
	intf.bLength = USB_DT_INTERFACE_SIZE;
	intf.bDescriptorType = USB_DT_INTERFACE;
	intf.bInterfaceNumber = 0;
	intf.bAlternateSetting = 0;
	intf.bNumEndpoints = 1;
	intf.bInterfaceClass = USB_CLASS_HID;     /* 0x03 */
	intf.bInterfaceSubClass = 0;
	intf.bInterfaceProtocol = 0;
	intf.iInterface = 0;

	memset(&hid, 0, sizeof(hid));
	hid.bLength = (__u8)sizeof(hid);
	hid.bDescriptorType = HID_DT_HID;
	hid.bcdHID = 0x0111;
	hid.bCountryCode = 0;
	hid.bNumDescriptors = 1;
	hid.bReportType = HID_DT_REPORT;
	hid.wReportLength = (__u16)sizeof(k_report_descriptor);

	memset(&ep, 0, sizeof(ep));
	ep.bLength = USB_DT_ENDPOINT_SIZE;
	ep.bDescriptorType = USB_DT_ENDPOINT;
	ep.bEndpointAddress = ep_addr;
	ep.bmAttributes = USB_ENDPOINT_XFER_INT;  /* 0x03 */
	ep.wMaxPacketSize = TEST_VDEV_REPORT_SIZE;
	ep.bInterval = 5;

	memcpy(buf + off, &cfg, USB_DT_CONFIG_SIZE);      off += USB_DT_CONFIG_SIZE;
	memcpy(buf + off, &intf, USB_DT_INTERFACE_SIZE);  off += USB_DT_INTERFACE_SIZE;
	memcpy(buf + off, &hid, sizeof(hid));             off += (int)sizeof(hid);
	memcpy(buf + off, &ep, USB_DT_ENDPOINT_SIZE);
	return total;
}

static int build_device_descriptor(struct test_virtual_device *dev,
                                   struct usb_device_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->bLength = USB_DT_DEVICE_SIZE;
	d->bDescriptorType = USB_DT_DEVICE;
	d->bcdUSB = 0x0200;
	d->bDeviceClass = 0;
	d->bDeviceSubClass = 0;
	d->bDeviceProtocol = 0;
	d->bMaxPacketSize0 = 64;
	d->idVendor = dev->vendor_id;
	d->idProduct = dev->product_id;
	d->bcdDevice = 0x0100;
	d->iManufacturer = 1;
	d->iProduct = 2;
	d->iSerialNumber = 3;
	d->bNumConfigurations = 1;
	return (int)sizeof(*d);
}

/* Minimal string descriptor builder (ASCII -> UTF-16LE). index 0 = langids. */
static int build_string_descriptor(struct test_virtual_device *dev, __u8 index,
                                   unsigned char *buf, int buflen)
{
	const char *s;
	int i, n;

	if (index == 0) {
		if (buflen < 4)
			return -1;
		buf[0] = 4;
		buf[1] = USB_DT_STRING;
		buf[2] = 0x09;        /* 0x0409 English (US) */
		buf[3] = 0x04;
		return 4;
	}

	switch (index) {
	case 1:  s = "HIDAPI"; break;
	case 2:  s = "HIDAPI Test Device"; break;
	case 3:  s = dev->serial; break;
	default: s = ""; break;
	}

	n = (int)strlen(s);
	if (2 + n * 2 > buflen)
		n = (buflen - 2) / 2;
	buf[0] = (__u8)(2 + n * 2);
	buf[1] = USB_DT_STRING;
	for (i = 0; i < n; i++) {
		buf[2 + i * 2] = (unsigned char)s[i];
		buf[2 + i * 2 + 1] = 0;
	}
	return 2 + n * 2;
}

static void queue_input(struct test_virtual_device *dev, unsigned char command)
{
	pthread_mutex_lock(&dev->lock);
	dev->pending = command;
	pthread_cond_signal(&dev->cond);
	pthread_mutex_unlock(&dev->lock);
}

/* Handle one EP0 control request. */
static void handle_control(struct test_virtual_device *dev,
                           const struct usb_ctrlrequest *ctrl)
{
	unsigned char buf[256];
	int wlen = (int)ctrl->wLength;
	int n;

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		switch (ctrl->bRequest) {
		case USB_REQ_GET_DESCRIPTOR: {
			__u8 type = (__u8)(ctrl->wValue >> 8);
			__u8 idx = (__u8)(ctrl->wValue & 0xFF);
			if (type == USB_DT_DEVICE) {
				struct usb_device_descriptor d;
				n = build_device_descriptor(dev, &d);
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, &d, n);
			} else if (type == USB_DT_CONFIG) {
				n = build_config_descriptor(buf, (int)sizeof(buf), dev->int_in_addr);
				if (n < 0) { ioctl(dev->fd, USB_RAW_IOCTL_EP0_STALL, 0); break; }
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, buf, n);
			} else if (type == USB_DT_STRING) {
				n = build_string_descriptor(dev, idx, buf, (int)sizeof(buf));
				if (n < 0) { ioctl(dev->fd, USB_RAW_IOCTL_EP0_STALL, 0); break; }
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, buf, n);
			} else if (type == HID_DT_REPORT) {
				n = (int)sizeof(k_report_descriptor);
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, k_report_descriptor, n);
			} else if (type == HID_DT_HID) {
				struct hid_class_desc hid;
				memset(&hid, 0, sizeof(hid));
				hid.bLength = (__u8)sizeof(hid);
				hid.bDescriptorType = HID_DT_HID;
				hid.bcdHID = 0x0111;
				hid.bNumDescriptors = 1;
				hid.bReportType = HID_DT_REPORT;
				hid.wReportLength = (__u16)sizeof(k_report_descriptor);
				n = (int)sizeof(hid);
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, &hid, n);
			} else {
				ioctl(dev->fd, USB_RAW_IOCTL_EP0_STALL, 0);
			}
			break;
		}
		case USB_REQ_SET_CONFIGURATION: {
			struct usb_endpoint_descriptor ep;
			int handle;
			memset(&ep, 0, sizeof(ep));
			ep.bLength = USB_DT_ENDPOINT_SIZE;
			ep.bDescriptorType = USB_DT_ENDPOINT;
			ep.bEndpointAddress = dev->int_in_addr;
			ep.bmAttributes = USB_ENDPOINT_XFER_INT;
			ep.wMaxPacketSize = TEST_VDEV_REPORT_SIZE;
			ep.bInterval = 5;
			handle = ioctl(dev->fd, USB_RAW_IOCTL_EP_ENABLE, &ep);
			if (handle >= 0) {
				dev->int_in_ep = handle;
				ioctl(dev->fd, USB_RAW_IOCTL_CONFIGURE, 0);
				dev->configured = 1;
			}
			ep0_ack(dev->fd, ctrl);   /* status ACK */
			break;
		}
		case USB_REQ_SET_INTERFACE:
			ep0_ack(dev->fd, ctrl);
			break;
		case USB_REQ_GET_STATUS: {
			unsigned char st[2] = { 0, 0 };
			ep0_write(dev->fd, st, sizeof(st));
			break;
		}
		default:
			ep0_ack(dev->fd, ctrl);
			break;
		}
		return;
	}

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS) {
		if (ctrl->bRequestType & USB_DIR_IN) {
			/* GET_REPORT / GET_IDLE / GET_PROTOCOL: benign reply. */
			if (ctrl->bRequest == HID_REQ_GET_REPORT) {
				memset(buf, 0, TEST_VDEV_REPORT_SIZE);
				n = TEST_VDEV_REPORT_SIZE;
				if (n > wlen) n = wlen;
				ep0_write(dev->fd, buf, n);
			} else {
				unsigned char z = 0;
				ep0_write(dev->fd, &z, wlen ? 1 : 0);
			}
			return;
		}

		/* Host-to-device class requests. */
		if (ctrl->bRequest == HID_REQ_SET_REPORT) {
			unsigned char command = TEST_VDEV_CMD_NONE;
			int i, scan;
			int len = wlen;
			if (len > (int)sizeof(buf))
				len = (int)sizeof(buf);
			n = ep0_read(dev->fd, buf, len);   /* read data stage + ACK */
			scan = (n > 4) ? 4 : n;
			for (i = 0; i < scan; i++) {
				if (buf[i] == TEST_VDEV_CMD_EMIT_A ||
				    buf[i] == TEST_VDEV_CMD_EMIT_B) {
					command = buf[i];
					break;
				}
			}
			if (command != TEST_VDEV_CMD_NONE)
				queue_input(dev, command);
		} else {
			/* SET_IDLE / SET_PROTOCOL and friends: just ACK. */
			ep0_ack(dev->fd, ctrl);
		}
		return;
	}

	/* Unknown request type. */
	ep0_ack(dev->fd, ctrl);
}

static void process_eps_info(struct test_virtual_device *dev)
{
	struct usb_raw_eps_info info;
	int num, i;
	memset(&info, 0, sizeof(info));
	num = ioctl(dev->fd, USB_RAW_IOCTL_EPS_INFO, &info);
	if (num < 0)
		return;
	for (i = 0; i < num && i < USB_RAW_EPS_NUM_MAX; i++) {
		if (info.eps[i].caps.type_int && info.eps[i].caps.dir_in) {
			__u32 addr = info.eps[i].addr;
			if (addr == USB_RAW_EP_ADDR_ANY)
				addr = 1;                       /* pick ep number 1 */
			dev->int_in_addr = (__u8)(0x80 | (addr & 0x0F));
			return;
		}
	}
}

static void *ep0_thread_fn(void *arg)
{
	struct test_virtual_device *dev = (struct test_virtual_device *)arg;
	struct usb_raw_event *ev;
	size_t evsz = sizeof(*ev) + sizeof(struct usb_ctrlrequest);

	ev = (struct usb_raw_event *)calloc(1, evsz);
	if (!ev)
		return NULL;

	while (!dev->stop) {
		int rv;
		ev->type = 0;
		ev->length = sizeof(struct usb_ctrlrequest);
		rv = ioctl(dev->fd, USB_RAW_IOCTL_EVENT_FETCH, ev);
		if (rv < 0) {
			if (errno == EINTR)
				continue;
			sleep_ms(10);
			continue;
		}

		if (ev->type == USB_RAW_EVENT_CONNECT) {
			process_eps_info(dev);
		} else if (ev->type == USB_RAW_EVENT_CONTROL) {
			struct usb_ctrlrequest ctrl;
			memcpy(&ctrl, ev->data, sizeof(ctrl));
			handle_control(dev, &ctrl);
		}
	}

	free(ev);
	dev->ep0_exited = 1;
	return NULL;
}

/* Delivers a queued canned input report over the interrupt IN endpoint. The
 * EP_WRITE blocks until the host reads it, so this runs on its own thread. */
static void *int_in_thread_fn(void *arg)
{
	struct test_virtual_device *dev = (struct test_virtual_device *)arg;

	for (;;) {
		unsigned char command;
		const unsigned char *payload;

		pthread_mutex_lock(&dev->lock);
		while (!dev->stop && dev->pending == TEST_VDEV_CMD_NONE) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 100 * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&dev->cond, &dev->lock, &ts);
		}
		if (dev->stop) {
			pthread_mutex_unlock(&dev->lock);
			break;
		}
		command = dev->pending;
		dev->pending = TEST_VDEV_CMD_NONE;
		pthread_mutex_unlock(&dev->lock);

		if (!dev->configured || dev->int_in_ep < 0)
			continue;

		payload = (command == TEST_VDEV_CMD_EMIT_B) ? k_input_b : k_input_a;
		/* Best effort: may fail if the host isn't reading; ignore. */
		(void)ep_io_write(dev->fd, USB_RAW_IOCTL_EP_WRITE, dev->int_in_ep,
		                  payload, TEST_VDEV_REPORT_SIZE);
	}
	return NULL;
}

int test_virtual_device_create(test_virtual_device **out_dev,
                               unsigned short vendor_id,
                               unsigned short product_id,
                               const char *serial)
{
	struct test_virtual_device *dev;
	struct usb_raw_init init;
	int rc;

	if (!out_dev)
		return TEST_VDEV_ERROR;
	*out_dev = NULL;

	dev = (struct test_virtual_device *)calloc(1, sizeof(*dev));
	if (!dev)
		return TEST_VDEV_ERROR;

	dev->fd = -1;
	dev->int_in_ep = -1;
	dev->int_in_addr = 0x81;
	dev->pending = TEST_VDEV_CMD_NONE;
	dev->vendor_id = vendor_id;
	dev->product_id = product_id;
	snprintf(dev->serial, sizeof(dev->serial), "%s", serial ? serial : "");
	pthread_mutex_init(&dev->lock, NULL);
	pthread_cond_init(&dev->cond, NULL);

	dev->fd = open("/dev/raw-gadget", O_RDWR);
	if (dev->fd < 0) {
		int e = errno;
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		if (e == ENOENT || e == EACCES || e == EPERM || e == ENODEV)
			return TEST_VDEV_UNAVAILABLE;
		return TEST_VDEV_ERROR;
	}

	memset(&init, 0, sizeof(init));
	/* dummy_hcd registers a UDC named "dummy_udc.0". */
	snprintf((char *)init.driver_name, sizeof(init.driver_name), "dummy_udc");
	snprintf((char *)init.device_name, sizeof(init.device_name), "dummy_udc.0");
	init.speed = USB_SPEED_HIGH;
	if (ioctl(dev->fd, USB_RAW_IOCTL_INIT, &init) < 0 ||
	    ioctl(dev->fd, USB_RAW_IOCTL_RUN, 0) < 0) {
		/* No dummy_hcd UDC present -> nothing to emulate on; skip. */
		close(dev->fd);
		pthread_cond_destroy(&dev->cond);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		return TEST_VDEV_UNAVAILABLE;
	}

	rg_install_signal();

	rc = pthread_create(&dev->ep0_thread, NULL, ep0_thread_fn, dev);
	if (rc != 0)
		goto fail_threads;
	dev->ep0_started = 1;

	rc = pthread_create(&dev->int_in_thread, NULL, int_in_thread_fn, dev);
	if (rc != 0)
		goto fail_threads;
	dev->int_in_started = 1;

	*out_dev = dev;
	return TEST_VDEV_OK;

fail_threads:
	dev->stop = 1;
	pthread_mutex_lock(&dev->lock);
	pthread_cond_broadcast(&dev->cond);
	pthread_mutex_unlock(&dev->lock);
	if (dev->ep0_started) {
		int spins = 0;
		while (!dev->ep0_exited && spins++ < 500) {
			pthread_kill(dev->ep0_thread, SIGUSR1);
			sleep_ms(10);
		}
		pthread_join(dev->ep0_thread, NULL);
	}
	close(dev->fd);
	pthread_cond_destroy(&dev->cond);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
	return TEST_VDEV_ERROR;
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
	if (!dev)
		return;

	dev->stop = 1;
	pthread_mutex_lock(&dev->lock);
	pthread_cond_broadcast(&dev->cond);
	pthread_mutex_unlock(&dev->lock);

	/* The ep0 thread is parked in the blocking EVENT_FETCH ioctl (and the
	   int_in thread may be in EP_WRITE); interrupt them with SIGUSR1 until the
	   ep0 thread reports it has left its loop, so the joins below don't hang. */
	{
		int spins = 0;
		while (dev->ep0_started && !dev->ep0_exited && spins++ < 500) {
			pthread_kill(dev->ep0_thread, SIGUSR1);
			if (dev->int_in_started)
				pthread_kill(dev->int_in_thread, SIGUSR1);
			sleep_ms(10);
		}
	}

	if (dev->int_in_started) {
		pthread_join(dev->int_in_thread, NULL);
		dev->int_in_started = 0;
	}
	if (dev->ep0_started) {
		pthread_join(dev->ep0_thread, NULL);
		dev->ep0_started = 0;
	}

	if (dev->fd >= 0) {
		close(dev->fd);
		dev->fd = -1;
	}

	pthread_cond_destroy(&dev->cond);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
}
