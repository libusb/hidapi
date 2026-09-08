/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022, All Rights Reserved.

 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU General Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        https://github.com/libusb/hidapi .
********************************************************/

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
/* Unix */
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <linux/types.h>
#include <linux/hid.h>
#include <stdarg.h>
#include <wchar.h>

/* Linux */
#include <linux/hidraw.h>
#include <linux/input.h>
#include <libudev.h>

#include "hidapi.h"

struct udev_device;

#ifdef HIDAPI_ALLOW_BUILD_WORKAROUND_KERNEL_2_6_39
/* This definitions first appeared in Linux Kernel 2.6.39 in linux/hidraw.h.
    hidapi doesn't support kernels older than that,
    so we don't define macros below explicitly, to fail builds on old kernels.
    For those who really need this as a workaround (e.g. to be able to build on old build machines),
    can workaround by defining the macro above.
*/
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)
#endif

#endif


// HIDIOCGINPUT and HIDIOCSOUTPUT are not defined in Linux kernel headers < 5.11.
// These definitions are from hidraw.h in Linux >= 5.11.
// https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f43d3870cafa2a0f3854c1819c8385733db8f9ae
#ifndef HIDIOCGINPUT
#define HIDIOCGINPUT(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x0A, len)
#endif
#ifndef HIDIOCSOUTPUT
#define HIDIOCSOUTPUT(len)   _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x0B, len)
#endif

/* The value of the first callback handle to be given upon registration */
/* Can be any arbitrary positive integer */
#define FIRST_HOTPLUG_CALLBACK_HANDLE 1

struct hid_device_ {
	int device_handle;
	int blocking;
	wchar_t *last_error_str;
	wchar_t *last_read_error_str;
	struct hid_device_info* device_info;
};

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

static wchar_t *last_global_error_str = NULL;


static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (dev == NULL) {
		return NULL;
	}

	dev->device_handle = -1;
	dev->blocking = 1;
	dev->last_error_str = NULL;
	dev->last_read_error_str = NULL;
	dev->device_info = NULL;

	return dev;
}


/* The caller must free the returned string with free(). */
static wchar_t *utf8_to_wchar_t(const char *utf8)
{
	wchar_t *ret = NULL;

	if (utf8) {
		size_t wlen = mbstowcs(NULL, utf8, 0);
		if ((size_t) -1 == wlen) {
			return wcsdup(L"");
		}
		ret = (wchar_t*) calloc(wlen+1, sizeof(wchar_t));
		if (ret == NULL) {
			/* as much as we can do at this point */
			return NULL;
		}
		mbstowcs(ret, utf8, wlen+1);
		ret[wlen] = 0x0000;
	}

	return ret;
}


/* Serializes concurrent writers of the error strings: hotplug registration and
 * deregistration may fail on several application threads at once, including
 * during hid_exit(). Internal hotplug processing never writes the global string
 * (see quiet); application calls such as hid_enumerate() from a callback remain
 * subject to the application's serialization. Reading via hid_error()
 * remains subject to the documented thread-safety rules. */
static pthread_mutex_t error_str_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Makes a copy of the given error message (and decoded according to the
 * currently locale) into the wide string pointer pointed by error_str.
 * The last stored error string is freed.
 * Use register_error_str(NULL) to free the error message completely. */
static void register_error_str(wchar_t **error_str, const char *msg)
{
	wchar_t *new_str = utf8_to_wchar_t(msg);

	pthread_mutex_lock(&error_str_mutex);
	free(*error_str);
	*error_str = new_str;
	pthread_mutex_unlock(&error_str_mutex);
}

/* Semilar to register_error_str, but allows passing a format string with va_list args into this function. */
static void register_error_str_vformat(wchar_t **error_str, const char *format, va_list args)
{
	char msg[256];
	vsnprintf(msg, sizeof(msg), format, args);

	register_error_str(error_str, msg);
}

/* Set the last global error to be reported by hid_error(NULL).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored global error message is freed.
 * Use register_global_error(NULL) to indicate "no error". */
static void register_global_error(const char *msg)
{
	register_error_str(&last_global_error_str, msg);
}

/* Similar to register_global_error, but allows passing a format string into this function. */
static void register_global_error_format(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&last_global_error_str, format, args);
	va_end(args);
}

/* Set the last error for a device to be reported by hid_error(dev).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored device error message is freed.
 * Use register_device_error(dev, NULL) to indicate "no error". */
static void register_device_error(hid_device *dev, const char *msg)
{
	register_error_str(&dev->last_error_str, msg);
}

/* Similar to register_device_error, but you can pass a format string into this function. */
static void register_device_error_format(hid_device *dev, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&dev->last_error_str, format, args);
	va_end(args);
}

/*
 * Gets the size of the HID item at the given position
 * Returns 1 if successful, 0 if an invalid key
 * Sets data_len and key_size when successful
 */
static int get_hid_item_size(const __u8 *report_descriptor, __u32 size, unsigned int pos, int *data_len, int *key_size)
{
	int key = report_descriptor[pos];
	int size_code;

	/*
	 * This is a Long Item. The next byte contains the
	 * length of the data section (value) for this key.
	 * See the HID specification, version 1.11, section
	 * 6.2.2.3, titled "Long Items."
	 */
	if ((key & 0xf0) == 0xf0) {
		if (pos + 1 < size)
		{
			*data_len = report_descriptor[pos + 1];
			*key_size = 3;
			return 1;
		}
		*data_len = 0; /* malformed report */
		*key_size = 0;
	}

	/*
	 * This is a Short Item. The bottom two bits of the
	 * key contain the size code for the data section
	 * (value) for this key. Refer to the HID
	 * specification, version 1.11, section 6.2.2.2,
	 * titled "Short Items."
	 */
	size_code = key & 0x3;
	switch (size_code) {
	case 0:
	case 1:
	case 2:
		*data_len = size_code;
		*key_size = 1;
		return 1;
	case 3:
		*data_len = 4;
		*key_size = 1;
		return 1;
	default:
		/* Can't ever happen since size_code is & 0x3 */
		*data_len = 0;
		*key_size = 0;
		break;
	};

	/* malformed report */
	return 0;
}

/*
 * Get bytes from a HID Report Descriptor.
 * Only call with a num_bytes of 0, 1, 2, or 4.
 */
static __u32 get_hid_report_bytes(const __u8 *rpt, size_t len, size_t num_bytes, size_t cur)
{
	/* Return if there aren't enough bytes. */
	if (cur + num_bytes >= len)
		return 0;

	if (num_bytes == 0)
		return 0;
	else if (num_bytes == 1)
		return rpt[cur + 1];
	else if (num_bytes == 2)
		return (rpt[cur + 2] * 256 + rpt[cur + 1]);
	else if (num_bytes == 4)
		return (
			rpt[cur + 4] * 0x01000000 +
			rpt[cur + 3] * 0x00010000 +
			rpt[cur + 2] * 0x00000100 +
			rpt[cur + 1] * 0x00000001
		);
	else
		return 0;
}

/*
 * Iterates until the end of a Collection.
 * Assumes that *pos is exactly at the beginning of a Collection.
 * Skips all nested Collection, i.e. iterates until the end of current level Collection.
 *
 * The return value is non-0 when an end of current Collection is found,
 * 0 when error is occurred (broken Descriptor, end of a Collection is found before its begin,
 *  or no Collection is found at all).
 */
static int hid_iterate_over_collection(const __u8 *report_descriptor, __u32 size, unsigned int *pos, int *data_len, int *key_size)
{
	int collection_level = 0;

	while (*pos < size) {
		int key = report_descriptor[*pos];
		int key_cmd = key & 0xfc;

		/* Determine data_len and key_size */
		if (!get_hid_item_size(report_descriptor, size, *pos, data_len, key_size))
			return 0; /* malformed report */

		switch (key_cmd) {
		case 0xa0: /* Collection 6.2.2.4 (Main) */
			collection_level++;
			break;
		case 0xc0: /* End Collection 6.2.2.4 (Main) */
			collection_level--;
			break;
		}

		if (collection_level < 0) {
			/* Broken descriptor or someone is using this function wrong,
			 * i.e. should be called exactly at the collection start */
			return 0;
		}

		if (collection_level == 0) {
			/* Found it!
			 * Also possible when called not at the collection start, but should not happen if used correctly */
			return 1;
		}

		*pos += *data_len + *key_size;
	}

	return 0; /* Did not find the end of a Collection */
}

struct hid_usage_iterator {
	unsigned int pos;
	int usage_page_found;
	unsigned short usage_page;
};

/*
 * Retrieves the device's Usage Page and Usage from the report descriptor.
 * The algorithm returns the current Usage Page/Usage pair whenever a new
 * Collection is found and a Usage Local Item is currently in scope.
 * Usage Local Items are consumed by each Main Item (See. 6.2.2.8).
 * The algorithm should give similar results as Apple's:
 *   https://developer.apple.com/documentation/iokit/kiohiddeviceusagepairskey?language=objc
 * Physical Collections are also matched (macOS does the same).
 *
 * This function can be called repeatedly until it returns non-0
 * Usage is found. pos is the starting point (initially 0) and will be updated
 * to the next search position.
 *
 * The return value is 0 when a pair is found.
 * 1 when finished processing descriptor.
 * -1 on a malformed report.
 */
static int get_next_hid_usage(const __u8 *report_descriptor, __u32 size, struct hid_usage_iterator *ctx, unsigned short *usage_page, unsigned short *usage)
{
	int data_len, key_size;
	int initial = ctx->pos == 0; /* Used to handle case where no top-level application collection is defined */

	int usage_found = 0;

	while (ctx->pos < size) {
		int key = report_descriptor[ctx->pos];
		int key_cmd = key & 0xfc;

		/* Determine data_len and key_size */
		if (!get_hid_item_size(report_descriptor, size, ctx->pos, &data_len, &key_size))
			return -1; /* malformed report */

		switch (key_cmd) {
		case 0x4: /* Usage Page 6.2.2.7 (Global) */
			ctx->usage_page = get_hid_report_bytes(report_descriptor, size, data_len, ctx->pos);
			ctx->usage_page_found = 1;
			break;

		case 0x8: /* Usage 6.2.2.8 (Local) */
			if (data_len == 4) { /* Usages 5.5 / Usage Page 6.2.2.7 */
				ctx->usage_page = get_hid_report_bytes(report_descriptor, size, 2, ctx->pos + 2);
				ctx->usage_page_found = 1;
				*usage = get_hid_report_bytes(report_descriptor, size, 2, ctx->pos);
				usage_found = 1;
			}
			else {
				*usage = get_hid_report_bytes(report_descriptor, size, data_len, ctx->pos);
				usage_found = 1;
			}
			break;

		case 0xa0: /* Collection 6.2.2.4 (Main) */
			if (!hid_iterate_over_collection(report_descriptor, size, &ctx->pos, &data_len, &key_size)) {
				return -1;
			}

			/* A pair is valid - to be reported when Collection is found */
			if (usage_found && ctx->usage_page_found) {
				*usage_page = ctx->usage_page;
				return 0;
			}

			break;
		}

		/* Skip over this key and its associated data */
		ctx->pos += data_len + key_size;
	}

	/* If no top-level application collection is found and usage page/usage pair is found, pair is valid
	   https://docs.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections */
	if (initial && usage_found && ctx->usage_page_found) {
		*usage_page = ctx->usage_page;
		return 0; /* success */
	}

	return 1; /* finished processing */
}

/*
 * Retrieves the hidraw report descriptor from a file.
 * When using this form, <sysfs_path>/device/report_descriptor, elevated privileges are not required.
 * quiet: don't touch the global error string - for the callers on HIDAPI's
 * internal monitor thread, which never writes it (see hidapi.h).
 */
static int get_hid_report_descriptor(const char *rpt_path, struct hidraw_report_descriptor *rpt_desc, int quiet)
{
	int rpt_handle;
	ssize_t res;

	rpt_handle = open(rpt_path, O_RDONLY | O_CLOEXEC);
	if (rpt_handle < 0) {
		if (!quiet) {
			register_global_error_format("open failed (%s): %s", rpt_path, strerror(errno));
		}
		return -1;
	}

	/*
	 * Read in the Report Descriptor
	 * The sysfs file has a maximum size of 4096 (which is the same as HID_MAX_DESCRIPTOR_SIZE) so we should always
	 * be ok when reading the descriptor.
	 * In practice if the HID descriptor is any larger I suspect many other things will break.
	 */
	memset(rpt_desc, 0x0, sizeof(*rpt_desc));
	res = read(rpt_handle, rpt_desc->value, HID_MAX_DESCRIPTOR_SIZE);
	if (res < 0) {
		if (!quiet) {
			register_global_error_format("read failed (%s): %s", rpt_path, strerror(errno));
		}
	}
	rpt_desc->size = (__u32) res;

	close(rpt_handle);
	return (int) res;
}

/* return size of the descriptor, -1 if unavailable, or -2 on allocation failure
   (quiet: see get_hid_report_descriptor) */
static int get_hid_report_descriptor_from_sysfs(const char *sysfs_path, struct hidraw_report_descriptor *rpt_desc, int quiet)
{
	int res = -1;
	/* Construct <sysfs_path>/device/report_descriptor */
	size_t rpt_path_len = strlen(sysfs_path) + 25 + 1;
	char* rpt_path = (char*) calloc(1, rpt_path_len);
	if (!rpt_path)
		return -2;
	snprintf(rpt_path, rpt_path_len, "%s/device/report_descriptor", sysfs_path);

	res = get_hid_report_descriptor(rpt_path, rpt_desc, quiet);
	free(rpt_path);

	return res;
}

/* return non-zero if successfully parsed */
static int parse_hid_vid_pid_from_uevent(const char *uevent, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id, int quiet)
{
	char tmp[1024];
	size_t uevent_len = strlen(uevent);
	if (uevent_len > sizeof(tmp) - 1)
		uevent_len = sizeof(tmp) - 1;
	memcpy(tmp, uevent, uevent_len);
	tmp[uevent_len] = '\0';

	char *saveptr = NULL;
	char *line;
	char *key;
	char *value;

	line = strtok_r(tmp, "\n", &saveptr);
	while (line != NULL) {
		/* line: "KEY=value" */
		key = line;
		value = strchr(line, '=');
		if (!value) {
			goto next_line;
		}
		*value = '\0';
		value++;

		if (strcmp(key, "HID_ID") == 0) {
			/**
			 *        type vendor   product
			 * HID_ID=0003:000005AC:00008242
			 **/
			int ret = sscanf(value, "%x:%hx:%hx", bus_type, vendor_id, product_id);
			if (ret == 3) {
				return 1;
			}
		}

next_line:
		line = strtok_r(NULL, "\n", &saveptr);
	}

	if (!quiet) {
		register_global_error("Couldn't find/parse HID_ID");
	}
	return 0;
}

/* return non-zero if successfully parsed */
static int parse_hid_vid_pid_from_uevent_path(const char *uevent_path, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id, int quiet)
{
	int handle;
	ssize_t res;

	handle = open(uevent_path, O_RDONLY | O_CLOEXEC);
	if (handle < 0) {
		if (!quiet) {
			register_global_error_format("open failed (%s): %s", uevent_path, strerror(errno));
		}
		return 0;
	}

	char buf[1024];
	res = read(handle, buf, sizeof(buf) - 1); /* -1 for '\0' at the end */
	close(handle);

	if (res < 0) {
		if (!quiet) {
			register_global_error_format("read failed (%s): %s", uevent_path, strerror(errno));
		}
		return 0;
	}

	buf[res] = '\0';
	return parse_hid_vid_pid_from_uevent(buf, bus_type, vendor_id, product_id, quiet);
}

/* return non-zero if successfully read/parsed */
static int parse_hid_vid_pid_from_sysfs(const char *sysfs_path, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id, int quiet)
{
	int res = 0;
	/* Construct <sysfs_path>/device/uevent */
	size_t uevent_path_len = strlen(sysfs_path) + 14 + 1;
	char* uevent_path = (char*) calloc(1, uevent_path_len);
	if (!uevent_path) {
		if (!quiet) {
			register_global_error("Couldn't allocate the HID uevent path");
		}
		return 0;
	}
	snprintf(uevent_path, uevent_path_len, "%s/device/uevent", sysfs_path);

	res = parse_hid_vid_pid_from_uevent_path(uevent_path, bus_type, vendor_id, product_id, quiet);
	free(uevent_path);

	return res;
}

static int get_hid_report_descriptor_from_hidraw(hid_device *dev, struct hidraw_report_descriptor *rpt_desc)
{
	int desc_size = 0;

	/* Get Report Descriptor Size */
	int res = ioctl(dev->device_handle, HIDIOCGRDESCSIZE, &desc_size);
	if (res < 0) {
		register_device_error_format(dev, "ioctl(GRDESCSIZE): %s", strerror(errno));
		return res;
	}

	/* Get Report Descriptor */
	memset(rpt_desc, 0x0, sizeof(*rpt_desc));
	rpt_desc->size = desc_size;
	res = ioctl(dev->device_handle, HIDIOCGRDESC, rpt_desc);
	if (res < 0) {
		register_device_error_format(dev, "ioctl(GRDESC): %s", strerror(errno));
	}

	return res;
}

/*
 * The caller is responsible for free()ing the (newly-allocated) character
 * strings pointed to by serial_number_utf8 and product_name_utf8 after use.
 */
static int parse_uevent_info(const char *uevent, unsigned *bus_type,
	unsigned short *vendor_id, unsigned short *product_id,
	char **serial_number_utf8, char **product_name_utf8)
{
	char tmp[1024];

	if (!uevent) {
		return 0;
	}

	size_t uevent_len = strlen(uevent);
	if (uevent_len > sizeof(tmp) - 1)
		uevent_len = sizeof(tmp) - 1;
	memcpy(tmp, uevent, uevent_len);
	tmp[uevent_len] = '\0';

	char *saveptr = NULL;
	char *line;
	char *key;
	char *value;

	int found_id = 0;
	int found_serial = 0;
	int found_name = 0;

	line = strtok_r(tmp, "\n", &saveptr);
	while (line != NULL) {
		/* line: "KEY=value" */
		key = line;
		value = strchr(line, '=');
		if (!value) {
			goto next_line;
		}
		*value = '\0';
		value++;

		if (strcmp(key, "HID_ID") == 0) {
			/**
			 *        type vendor   product
			 * HID_ID=0003:000005AC:00008242
			 **/
			int ret = sscanf(value, "%x:%hx:%hx", bus_type, vendor_id, product_id);
			if (ret == 3) {
				found_id = 1;
			}
		} else if (strcmp(key, "HID_NAME") == 0) {
			/* The caller has to free the product name */
			free(*product_name_utf8);
			*product_name_utf8 = strdup(value);
			found_name = 1;
		} else if (strcmp(key, "HID_UNIQ") == 0) {
			/* The caller has to free the serial number */
			free(*serial_number_utf8);
			*serial_number_utf8 = strdup(value);
			found_serial = 1;
		}

next_line:
		line = strtok_r(NULL, "\n", &saveptr);
	}

	return (found_id && found_name && found_serial);
}


/* quiet: don't touch the global error string - for the callers on HIDAPI's
   internal monitor thread, which never writes it (see hidapi.h) */
/* Build the hid_device_info chain (one entry per usage) for a single udev device
   node. Returns NULL both for benign exclusions (no HID parent, an unparseable
   uevent, an unhandled bus type - exactly what hid_enumerate() would also skip)
   and for genuine resource failures. When `failure` is non-NULL it is set to 1
   ONLY in the latter case, so a caller that needs an all-or-nothing enumeration
   (the initial hotplug-registration snapshot, which must fail rather than arm the
   callbacks against an incomplete device set) can tell a device that is
   legitimately absent from one that merely failed to materialize. It is never
   cleared here, so callers accumulate it across a whole enumeration. */
static struct hid_device_info * create_device_info_for_device(struct udev_device *raw_dev, int quiet, int *failure)
{
	struct hid_device_info *root = NULL;
	struct hid_device_info *cur_dev = NULL;

	const char *sysfs_path;
	const char *dev_path;
	const char *str;
	struct udev_device *hid_dev; /* The device's HID udev node. */
	struct udev_device *usb_dev; /* The device's USB udev node. */
	struct udev_device *intf_dev; /* The device's interface (in the USB sense). */
	unsigned short dev_vid;
	unsigned short dev_pid;
	char *serial_number_utf8 = NULL;
	char *product_name_utf8 = NULL;
	unsigned bus_type;
	int result;
	/* USB-parent strings come from optional sysfs attributes and are checked
	   against their sources in that branch; other branches require both copies. */
	int strings_from_uevent = 1;
	struct hidraw_report_descriptor report_desc;

	sysfs_path = udev_device_get_syspath(raw_dev);
	dev_path = udev_device_get_devnode(raw_dev);

	hid_dev = udev_device_get_parent_with_subsystem_devtype(
		raw_dev,
		"hid",
		NULL);

	if (!hid_dev) {
		/* Unable to find parent hid device. */
		goto end;
	}

	result = parse_uevent_info(
		udev_device_get_sysattr_value(hid_dev, "uevent"),
		&bus_type,
		&dev_vid,
		&dev_pid,
		&serial_number_utf8,
		&product_name_utf8);

	if (!result) {
		/* parse_uevent_info() failed for at least one field. */
		goto end;
	}
	if (!serial_number_utf8 || !product_name_utf8) {
		if (failure) {
			*failure = 1;
		}
		goto end;
	}

	/* Filter out unhandled devices right away */
	switch (bus_type) {
		case BUS_BLUETOOTH:
		case BUS_I2C:
		case BUS_USB:
		case BUS_SPI:
		case BUS_VIRTUAL:
			break;

		default:
			goto end;
	}

	/* Create the record. */
	root = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
	if (!root) {
		/* A genuine resource failure, unlike the benign NULL returns above:
		   flag it for callers that require an all-or-nothing enumeration. */
		if (failure) {
			*failure = 1;
		}
		goto end;
	}

	cur_dev = root;

	/* Fill out the record */
	cur_dev->next = NULL;
	cur_dev->path = dev_path? strdup(dev_path): NULL;

	/* VID/PID */
	cur_dev->vendor_id = dev_vid;
	cur_dev->product_id = dev_pid;

	/* Serial Number */
	cur_dev->serial_number = utf8_to_wchar_t(serial_number_utf8);

	/* Release Number */
	cur_dev->release_number = 0x0;

	/* Interface Number */
	cur_dev->interface_number = -1;

	switch (bus_type) {
		case BUS_USB:
			/* The device pointed to by raw_dev contains information about
				the hidraw device. In order to get information about the
				USB device, get the parent device with the
				subsystem/devtype pair of "usb"/"usb_device". This will
				be several levels up the tree, but the function will find
				it. */
			usb_dev = udev_device_get_parent_with_subsystem_devtype(
					raw_dev,
					"usb",
					"usb_device");

			/* uhid USB devices
			 * Since this is a virtual hid interface, no USB information will
			 * be available. */
			if (!usb_dev) {
				/* Manufacturer and Product strings */
				cur_dev->manufacturer_string = wcsdup(L"");
				cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);
				break;
			}

			str = udev_device_get_sysattr_value(usb_dev, "manufacturer");
			cur_dev->manufacturer_string = utf8_to_wchar_t(str);
			if (str && !cur_dev->manufacturer_string && failure) {
				*failure = 1;
			}
			str = udev_device_get_sysattr_value(usb_dev, "product");
			cur_dev->product_string = utf8_to_wchar_t(str);
			if (str && !cur_dev->product_string && failure) {
				*failure = 1;
			}
			strings_from_uevent = 0;

			cur_dev->bus_type = HID_API_BUS_USB;

			str = udev_device_get_sysattr_value(usb_dev, "bcdDevice");
			cur_dev->release_number = (str)? strtol(str, NULL, 16): 0x0;

			/* Get a handle to the interface's udev node. */
			intf_dev = udev_device_get_parent_with_subsystem_devtype(
					raw_dev,
					"usb",
					"usb_interface");
			if (intf_dev) {
				str = udev_device_get_sysattr_value(intf_dev, "bInterfaceNumber");
				cur_dev->interface_number = (str)? strtol(str, NULL, 16): -1;
			}

			break;

		case BUS_BLUETOOTH:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_BLUETOOTH;

			break;
		case BUS_I2C:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_I2C;

			break;

		case BUS_SPI:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_SPI;

			break;

		case BUS_VIRTUAL:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_VIRTUAL;

			break;

		default:
			/* Unknown device type - this should never happen, as we
			 * check for USB and Bluetooth devices above */
			break;
	}

	/* A string copy that failed is a genuine resource failure too, exactly like
	   the calloc() above - flag it, or an all-or-nothing caller would accept a
	   silently degraded record (see the partial-copy check in
	   hid_internal_copy_device_info). An entry with a NULL path is especially
	   harmful in the hotplug cache: it can never be matched by its removal, nor
	   recognized as a duplicate. utf8_to_wchar_t() returns NULL only on an
	   allocation failure - an unconvertible string yields an empty one - so this
	   never fails a merely odd device. */
	if ((dev_path && !root->path)
	    || (serial_number_utf8 && !root->serial_number)
	    || (strings_from_uevent
	        && (!root->manufacturer_string
	            || (product_name_utf8 && !root->product_string)))) {
		if (failure) {
			*failure = 1;
		}
	}

	/* Usage Page and Usage */

	if (sysfs_path) {
		result = get_hid_report_descriptor_from_sysfs(sysfs_path, &report_desc, quiet);
	}
	else {
		result = -1;
	}
	if (result == -2 && failure) {
		*failure = 1;
	}

	if (result >= 0) {
		unsigned short page = 0, usage = 0;
		struct hid_usage_iterator usage_iterator;
		memset(&usage_iterator, 0, sizeof(usage_iterator));

		/*
		 * Parse the first usage and usage page
		 * out of the report descriptor.
		 */
		if (!get_next_hid_usage(report_desc.value, report_desc.size, &usage_iterator, &page, &usage)) {
			cur_dev->usage_page = page;
			cur_dev->usage = usage;
		}

		/*
		 * Parse any additional usage and usage pages
		 * out of the report descriptor.
		 */
		while (!get_next_hid_usage(report_desc.value, report_desc.size, &usage_iterator, &page, &usage)) {
			/* Create new record for additional usage pairs */
			struct hid_device_info *tmp = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
			struct hid_device_info *prev_dev = cur_dev;

			if (!tmp) {
				/* Out of memory mid-device: the returned chain would be
				   incomplete, so flag it for all-or-nothing callers. */
				if (failure) {
					*failure = 1;
				}
				break;
			}
			cur_dev->next = tmp;
			cur_dev = tmp;

			/* Update fields */
			cur_dev->path = dev_path? strdup(dev_path): NULL;
			cur_dev->vendor_id = dev_vid;
			cur_dev->product_id = dev_pid;
			cur_dev->serial_number = prev_dev->serial_number? wcsdup(prev_dev->serial_number): NULL;
			cur_dev->release_number = prev_dev->release_number;
			cur_dev->interface_number = prev_dev->interface_number;
			cur_dev->manufacturer_string = prev_dev->manufacturer_string? wcsdup(prev_dev->manufacturer_string): NULL;
			cur_dev->product_string = prev_dev->product_string? wcsdup(prev_dev->product_string): NULL;
			cur_dev->usage_page = page;
			cur_dev->usage = usage;
			cur_dev->bus_type = prev_dev->bus_type;

			/* Same all-or-nothing rule as above: every string here is copied
			   from a source known to be non-NULL, so a NULL copy is an
			   allocation failure that left a degraded entry in the chain */
			if ((dev_path && !cur_dev->path)
			    || (prev_dev->serial_number && !cur_dev->serial_number)
			    || (prev_dev->manufacturer_string && !cur_dev->manufacturer_string)
			    || (prev_dev->product_string && !cur_dev->product_string)) {
				if (failure) {
					*failure = 1;
				}
				break;
			}
		}
	}

end:
	free(serial_number_utf8);
	free(product_name_utf8);

	return root;
}

static struct hid_device_info * create_device_info_for_hid_device(hid_device *dev) {
	struct udev *udev;
	struct udev_device *udev_dev;
	struct stat s;
	int ret = -1;
	struct hid_device_info *root = NULL;

	register_device_error(dev, NULL);

	/* Get the dev_t (major/minor numbers) from the file handle. */
	ret = fstat(dev->device_handle, &s);
	if (-1 == ret) {
		register_device_error(dev, "Failed to stat device handle");
		return NULL;
	}

	/* Create the udev object */
	udev = udev_new();
	if (!udev) {
		errno = ENOMEM;
		register_device_error(dev, "Couldn't create udev context");
		return NULL;
	}

	/* Open a udev device from the dev_t. 'c' means character device. */
	udev_dev = udev_device_new_from_devnum(udev, 'c', s.st_rdev);
	if (udev_dev) {
		root = create_device_info_for_device(udev_dev, 1, NULL);
	}

	if (!root) {
		/* TODO: have a better error reporting via create_device_info_for_device */
		errno = EIO;
		register_device_error(dev, "Couldn't create hid_device_info");
	}

	udev_device_unref(udev_dev);
	udev_unref(udev);

	return root;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

/* Lifecycle of the udev monitor thread; every transition happens under the
   hotplug mutex */
enum hid_hotplug_thread_state {
	/* No monitor thread exists; nothing to join */
	HID_HOTPLUG_THREAD_NONE,
	/* The monitor thread is running */
	HID_HOTPLUG_THREAD_RUNNING,
	/* The monitor thread has returned - it published this state as its LAST
	   write to shared state under the mutex, immediately before unlocking and
	   returning - but its pthread_t has not been joined yet. The thread is
	   JOINABLE, not detached: hid_exit() (and the next register/deregister)
	   reap it (see hid_internal_hotplug_reap_thread), so once hid_exit() returns
	   no monitor-thread instruction can still be executing inside the library. A
	   detached thread could not guarantee that - after it published its exit and
	   hid_exit() returned, it could still be running its own epilogue
	   (unlock/return) in the library's text when the application calls
	   dlclose(), i.e. resume in unmapped code. Reaping first RETIRES the finished
	   generation (moves its pthread_t onto hid_hotplug_context.retired, keyed by a
	   stable id token) and then joins it with the mutex released, so a pthread_t
	   is never inspected after pthread_join() invalidated it, and a generation
	   superseded by a re-entrant registration is joined rather than dropped. */
	HID_HOTPLUG_THREAD_FINISHED
};

/* A monitor thread that has been CREATED (pthread_create succeeded) and not yet
   JOINED. The current generation lives in hid_hotplug_context.thread /
   .thread_id / .thread_state; once it publishes FINISHED it is moved onto
   hid_hotplug_context.retired (see hid_internal_hotplug_reap_thread) and joined
   from there. A generation that is superseded while still FINISHED-but-unjoined
   - a thread-specific-data destructor running on it re-enters HIDAPI and starts a
   new generation - is therefore never dropped: its pthread_t stays on this list
   until joined. Threads are identified by their `id` token and by list
   membership, NEVER by comparing a pthread_t after it was joined (that handle is
   invalid). hid_exit() joins EVERY entry on this list (and the current thread)
   before it returns. */
struct hid_hotplug_monitor_thread {
	pthread_t thread;
	/* Stable identity token, assigned at creation (hid_hotplug_context.thread_id
	   at the time). Survives the join; used for identity/diagnostics without ever
	   touching the joined pthread_t. */
	unsigned long id;
	/* A reaper has set this and dropped the mutex to pthread_join() this entry:
	   any other reaper skips it (a second join of the same pthread_t is undefined
	   behavior), and hid_exit() waits on thread_cond for the join to finish. */
	unsigned char being_joined;
	struct hid_hotplug_monitor_thread *next;
};

static struct hid_hotplug_context {
	/* UDEV context that handles the monitor */
	struct udev* udev_ctx;

	/* UDEV monitor that receives events */
	struct udev_monitor* mon;

	/* File descriptor for the UDEV monitor that allows to check for new events with poll() */
	int monitor_fd;

	/* Thread for the UDEV monitor (the current generation) */
	pthread_t thread;

	/* Stable identity token of the current generation's thread (0 = none).
	   Assigned from next_thread_id at each pthread_create so a generation can be
	   identified without ever comparing a joined pthread_t. */
	unsigned long thread_id;

	/* Source of non-zero thread_id tokens; survives hid_exit, skips 0 on wrap */
	unsigned long next_thread_id;

	enum hid_hotplug_thread_state thread_state;

	/* Pre-allocated node owned while RUNNING or FINISHED; see
	   hid_internal_hotplug_retire_current(). Freed after joining, or if creation
	   fails. */
	struct hid_hotplug_monitor_thread *thread_node;

	/* Monitor threads created but not yet joined that are no longer the current
	   generation (superseded/orphaned). Their pthread_t is moved here instead of
	   being dropped; every entry is joined before hid_exit() returns. */
	struct hid_hotplug_monitor_thread *retired;

	/* Recursive for callbacks. mutex_in_use turns nested cleanup/exit away, so
	   their unlock/lock pairs and pthread_cond_wait run at depth exactly one:
	   those operations release only one level of a recursive mutex. */
	pthread_mutex_t mutex;

	/* Broadcast whenever a retired monitor thread is joined and removed from the
	   list. hid_exit() must join every generation before returning; when the only
	   entries left are being joined by another (pre-exit) reaper, it waits here
	   for that join to complete instead of double-joining. Process-lifetime, like
	   the mutex: never destroyed. */
	pthread_cond_t thread_cond;

	/* Boolean flags */
	/* The one-time initialization of the mutex succeeded (written once, under
	   pthread_once); the mutex lives for the rest of the process */
	unsigned char mutex_ready;
	unsigned char mutex_in_use;
	unsigned char cb_list_dirty;
	/* hid_exit() is tearing the hotplug machinery down */
	unsigned char exiting;
	/* The udev monitor socket died; no more events (see hotplug_thread) */
	unsigned char monitor_dead;

	/* HIDAPI unique callback handle counter */
	hid_hotplug_callback_handle next_handle;

	/* Linked list of the hotplug callbacks */
	struct hid_hotplug_callback *hotplug_cbs;

	/* Linked list of the device infos (mandatory when the device is disconnected) */
	struct hid_device_info *devs;
} hid_hotplug_context; /* zero-initialized; next_handle/monitor_fd/next_thread_id set on first init */

struct hid_hotplug_callback {
	hid_hotplug_callback_handle handle;
	unsigned short vendor_id;
	unsigned short product_id;
	int events; /* bitmask of hid_hotplug_event */
	void *user_data;
	hid_hotplug_callback_fn callback;

	/* Registration-time snapshot of the matching connected devices,
	   delivered asynchronously by the monitor thread as the
	   HID_API_HOTPLUG_ENUMERATE initial pass, before any live events
	   for this callback */
	struct hid_device_info *replay;

	/* Pointer to the next notification */
	struct hid_hotplug_callback *next;
};

static void hid_internal_hotplug_remove_postponed(void)
{
	/* Unregister the callbacks whose removal was postponed */
	/* This function is always called inside a locked mutex */
	/* However, any actions are only allowed if the mutex is NOT in use and if the DIRTY flag is set */
	if (hid_hotplug_context.mutex_in_use || !hid_hotplug_context.cb_list_dirty) {
		return;
	}

	/* Traverse the list of callbacks and check if any were marked for removal */
	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback *callback = *current;
		if (!callback->events) {
			*current = (*current)->next;
			/* A deregistered callback never fires again: drop its undelivered snapshot */
			hid_free_enumeration(callback->replay);
			free(callback);
			continue;
		}
		current = &callback->next;
	}
	
	/* Clear the flag so we don't start the cycle unless necessary */
	hid_hotplug_context.cb_list_dirty = 0;
}

/* Releases a (possibly partially initialized) monitoring context.
   Called with the mutex held, only when the monitor thread cannot touch it
   anymore: before it is started, after it has been joined, or by the monitor
   thread itself right before it announces its own exit. Idempotent. */
static void hid_internal_hotplug_release_monitor(void)
{
	hid_free_enumeration(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;
	if (hid_hotplug_context.mon) {
		udev_monitor_unref(hid_hotplug_context.mon);
		hid_hotplug_context.mon = NULL;
	}
	if (hid_hotplug_context.udev_ctx) {
		udev_unref(hid_hotplug_context.udev_ctx);
		hid_hotplug_context.udev_ctx = NULL;
	}
	hid_hotplug_context.monitor_fd = -1;
}

/* Moves the current generation onto the retired list once it has published
   FINISHED, so its pthread_t is tracked (never dropped) and the current slot is
   free for a new generation. The generation is identified by its stable id token,
   not by its pthread_t. Called with the mutex held. This only splices the
   generation's OWN pre-allocated node (hid_hotplug_context.thread_node, reserved
   at pthread_create time) onto the list - no allocation - so it CANNOT fail and
   never drops or overwrites a live pthread_t. */
static void hid_internal_hotplug_retire_current(void)
{
	struct hid_hotplug_monitor_thread *node;

	if (hid_hotplug_context.thread_state != HID_HOTPLUG_THREAD_FINISHED) {
		return;
	}

	node = hid_hotplug_context.thread_node;
	node->thread = hid_hotplug_context.thread;
	node->id = hid_hotplug_context.thread_id;
	node->being_joined = 0;
	node->next = hid_hotplug_context.retired;
	hid_hotplug_context.retired = node;

	/* The current slot no longer names a live-or-finished thread. */
	hid_hotplug_context.thread_node = NULL;
	hid_hotplug_context.thread_state = HID_HOTPLUG_THREAD_NONE;
	hid_hotplug_context.thread_id = 0;
}

/* The stable id token of the generation this thread IS, or 0 on any other
   thread. Written by hotplug_thread() as its first action and never cleared, so
   it identifies the thread for the whole of its life - including the
   thread-specific-data destructor phase, which runs AFTER the thread published
   FINISHED and returned. That phase is exactly where a pthread_t comparison
   stops being usable: a reaper may already have claimed the entry
   (being_joined) and be sitting in pthread_join(), which returns only once
   those destructors are done - so being_joined does NOT mean the thread has
   terminated, and its pthread_t must not be touched either way. The id tokens
   start at 1 and skip 0 on wrap, so 0 is never a valid generation; thread-locals
   are zero-initialized, and a recycled OS thread gets a fresh (zero) one. */
static __thread unsigned long hid_hotplug_thread_self_id;

/* Non-zero when the caller runs on one of HIDAPI's monitor threads, whatever
   the state of that thread's generation - running, finished, retired or being
   joined. Needs no lock: it only reads this thread's own thread-local.

   mutex_in_use alone does not catch every such call: it only covers a call made
   from within a dispatch. A thread-specific-data destructor armed by a user
   callback runs on the monitor thread after the dispatch unwound - and after the
   thread published FINISHED - and may re-enter the library from there. */
static int hid_internal_on_monitor_thread(void)
{
	return hid_hotplug_thread_self_id != 0;
}

/* Reaps monitor threads: first RETIRES the current generation if it has finished
   (hid_internal_hotplug_retire_current), then joins every retired thread it is
   allowed to join, each with the mutex RELEASED. Called with the mutex held
   exactly once; because the mutex is dropped for the joins, callers must
   re-validate any cached state after this returns.

   pthread_join() must NOT run under the mutex: a finished monitor thread has
   published FINISHED and released everything it owned, but a thread-specific-data
   destructor armed by a user callback still runs on it afterwards - and such a
   destructor re-entering HIDAPI would block on the hotplug mutex forever if the
   joiner held it, deadlocking the join. So each entry is claimed (being_joined),
   the mutex is dropped, the thread is joined, the mutex is re-acquired, and the
   entry is unlinked and freed with a thread_cond broadcast.

   Both hazards are handled WITHOUT ever inspecting a joined pthread_t:
   - Monitor threads never join, even from thread-specific-data destructors:
     two such destructors could otherwise join each other. They only retire;
     joins are left to an application-thread reaper / hid_exit().
   - Concurrent reapers: an entry already being joined (being_joined) is skipped,
     so the same pthread_t is never joined twice; its claimant unlinks it when its
     join completes. Identity is by list membership and the id token - a pthread_t
     is only ever passed to pthread_join() while still unjoined. */
static void hid_internal_hotplug_reap_thread(void)
{
	hid_internal_hotplug_retire_current();
	if (hid_internal_on_monitor_thread()) {
		return;
	}

	for (;;) {
		struct hid_hotplug_monitor_thread **link;
		struct hid_hotplug_monitor_thread *node = NULL;
		int join_error;

		for (link = &hid_hotplug_context.retired; *link != NULL; link = &(*link)->next) {
			if (!(*link)->being_joined) {
				node = *link;
				break;
			}
		}
		if (node == NULL) {
			/* Nothing left that we may join: the list is empty, or the only
			   entries are already being joined by another reaper. */
			return;
		}

		/* Claim the join, drop the mutex for it, then re-acquire and unlink. */
		node->being_joined = 1;
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		join_error = pthread_join(node->thread, NULL);
		pthread_mutex_lock(&hid_hotplug_context.mutex);
		if (join_error != 0) {
			node->being_joined = 0;
			pthread_cond_broadcast(&hid_hotplug_context.thread_cond);
			return;
		}

		for (link = &hid_hotplug_context.retired; *link != NULL; link = &(*link)->next) {
			if (*link == node) {
				*link = node->next;
				break;
			}
		}
		free(node);
		/* Wake hid_exit() (or any reaper) waiting for this join to complete. */
		pthread_cond_broadcast(&hid_hotplug_context.thread_cond);
	}
}

/* Winds the monitor thread down once the last callback is gone and reaps it. The
   thread releases the monitoring context itself, then publishes FINISHED; this
   retires it and joins the retired threads it can (see
   hid_internal_hotplug_reap_thread). Called with the mutex held; during a
   dispatch (mutex_in_use) it returns immediately. The unlock/lock pairs below
   therefore run at recursion depth one, dropping the mutex while waiting or
   joining, so the caller must re-validate cached state after this returns.

   This does NOT guarantee every retired generation is joined before it returns:
   monitor threads only retire, and an entry another reaper is joining is left
   on the list. Application-thread register/deregister calls join the finished
   generations they claim (waiting for their thread-specific-data destructors),
   but never wait for a join owned by another reaper. hid_exit() is the backstop
   that joins EVERY generation before it returns. */
static void hid_internal_hotplug_cleanup(void)
{
	if (hid_hotplug_context.mutex_in_use) {
		return;
	}

	for (;;) {
		/* Before checking if the list is empty, clear any entries whose removal was postponed first */
		hid_internal_hotplug_remove_postponed();

		if (hid_hotplug_context.hotplug_cbs != NULL) {
			/* Still serving callbacks - the thread must keep running */
			return;
		}

		if (hid_hotplug_context.thread_state == HID_HOTPLUG_THREAD_RUNNING) {
			/* The thread has not yet noticed the empty callback list. Drop the
			   mutex so it can make progress towards publishing FINISHED, then
			   re-evaluate. */
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			poll(NULL, 0, 1);
			pthread_mutex_lock(&hid_hotplug_context.mutex);
			continue;
		}

		/* NONE or FINISHED: retire the finished generation and join what we may.
		   The join drops the mutex, so another registration may start a new
		   generation meanwhile. Re-evaluate until the slot is NONE or callbacks
		   exist again; only hid_exit(), via exiting, excludes new generations. */
		hid_internal_hotplug_reap_thread();
		if (hid_hotplug_context.thread_state == HID_HOTPLUG_THREAD_NONE) {
			return;
		}
	}
}

static pthread_once_t hid_hotplug_init_once = PTHREAD_ONCE_INIT;

/* The one-time initialization of the hotplug mutex, run by pthread_once().
   On failure mutex_ready stays 0 and the hotplug API remains unavailable. */
static void hid_internal_hotplug_init_once(void)
{
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0) {
		return;
	}
	/* The mutex must be recursive: a callback runs with it held and is
	   allowed to call hid_hotplug_register_callback() /
	   hid_hotplug_deregister_callback() */
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
		pthread_mutexattr_destroy(&attr);
		return;
	}
	if (pthread_mutex_init(&hid_hotplug_context.mutex, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return;
	}
	pthread_mutexattr_destroy(&attr);

	/* Process-lifetime, like the mutex (never destroyed): lets hid_exit() wait for
	   a retired thread another reaper is joining, so no pthread_t is joined twice. */
	if (pthread_cond_init(&hid_hotplug_context.thread_cond, NULL) != 0) {
		pthread_mutex_destroy(&hid_hotplug_context.mutex);
		return;
	}

	hid_hotplug_context.monitor_fd = -1;
	/* The handles are monotonic and never reused (see hidapi.h); the counter
	   survives hid_exit */
	hid_hotplug_context.next_handle = FIRST_HOTPLUG_CALLBACK_HANDLE;
	/* Non-zero monitor-thread generation tokens; survive hid_exit.
	   Start at 1 so that 0 unambiguously means "no current generation". */
	hid_hotplug_context.next_thread_id = 1;

	/* Publish the mutex as usable, last */
	hid_hotplug_context.mutex_ready = 1;
}

/* Ensures the hotplug mutex exists. Returns 0 when the hotplug machinery is
   usable, -1 when it could not be initialized (locking an uninitialized mutex
   is undefined behavior, so the caller must fail).
   The mutex is process-lifetime: it is deliberately never destroyed.
   Destroying it in hid_exit() would race the concurrent (and allowed)
   hid_hotplug_register_callback()/hid_hotplug_deregister_callback() calls
   that are about to lock it - they can only re-check the state AFTER locking,
   so the mutex itself must stay valid; teardown is gated by the `exiting`
   flag INSIDE the mutex instead. There is deliberately no bootstrap lock
   around the initialization either: any lock ordered outside the hotplug
   mutex would deadlock against a registration made from within a callback
   (which already holds the hotplug mutex); pthread_once() provides both the
   one-time guarantee and the memory synchronization for reading mutex_ready. */
static int hid_internal_hotplug_init(void)
{
	pthread_once(&hid_hotplug_init_once, hid_internal_hotplug_init_once);

	return hid_hotplug_context.mutex_ready ? 0 : -1;
}

static void hid_internal_hotplug_exit(void)
{
	if (hid_internal_hotplug_init() != 0) {
		/* The hotplug mutex could not be created: nothing can ever have been
		   registered, and there is nothing to tear down */
		return;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	if (hid_hotplug_context.exiting) {
		/* Another hid_exit() is already tearing the machinery down */
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return;
	}

	if (hid_hotplug_context.mutex_in_use) {
		/* hid_exit() from within a hotplug callback has undefined behavior
		   (see hidapi.h); degrade gracefully instead of corrupting the
		   dispatch in flight below this frame: mark every callback for
		   removal and let the monitor thread wind itself down (releasing the
		   monitoring context) once the dispatch unwinds */
		for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback; callback = callback->next) {
			if (callback->events) {
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			}
		}
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return;
	}

	/* Close the hotplug API for the duration of the teardown: a concurrent
	   hid_hotplug_register_callback()/hid_hotplug_deregister_callback()
	   (allowed by the thread-safety contract) fails/no-ops instead of
	   re-arming the machinery while it is being torn down -
	   hid_internal_hotplug_cleanup() below temporarily drops the mutex
	   while reaping the monitor thread */
	hid_hotplug_context.exiting = 1;

	for (;;) {
		struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
		/* Remove all callbacks from the list, dropping any undelivered snapshots */
		while (*current) {
			struct hid_hotplug_callback* next = (*current)->next;
			hid_free_enumeration((*current)->replay);
			free(*current);
			*current = next;
		}
		hid_hotplug_context.cb_list_dirty = 0;

		/* Wind the current generation down and reap what can be reaped
		   (temporarily dropping the mutex). `exiting` keeps anything from
		   re-arming the machinery or starting a new generation while the mutex is
		   dropped, so from here the retired list only ever shrinks. */
		hid_internal_hotplug_cleanup();

		/* EVERY generation must be joined before hid_exit() returns. cleanup's
		   reap joined every retired thread it could, and `exiting` keeps new ones
		   from appearing, so anything still on the list is either being joined by
		   another (pre-exit) reaper - wait for that join to broadcast thread_cond,
		   then re-evaluate - or was left after a failed join.

		   A monitor-thread destructor must neither join nor wait for a join:
		   its own completion may be needed by another reaper. Leave all entries
		   for an application-thread register/deregister that finds the callback
		   list empty, or hid_exit(), to join. This is the same degradation as
		   hid_exit() from within a callback; identity uses the thread-local token,
		   never a possibly claimed pthread_t. */
		if (hid_internal_on_monitor_thread()) {
			break;
		}
		if (hid_hotplug_context.retired != NULL) {
			int others = 0;

			for (struct hid_hotplug_monitor_thread *entry = hid_hotplug_context.retired; entry != NULL; entry = entry->next) {
				if (entry->being_joined) {
					others = 1;
					break;
				}
			}

			if (others) {
				pthread_cond_wait(&hid_hotplug_context.thread_cond, &hid_hotplug_context.mutex);
			}
			continue;
		}

		if (hid_hotplug_context.hotplug_cbs == NULL
		    && hid_hotplug_context.thread_state == HID_HOTPLUG_THREAD_NONE) {
			/* The retired list is empty: every generation has been joined. */
			break;
		}
	}

	/* Re-open the hotplug API: the library may be initialized/used again */
	hid_hotplug_context.exiting = 0;

	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Serializes the implicit initialization: the hotplug API allows concurrent
   hid_hotplug_register_callback() calls, each of which implicitly initializes
   the library - directly, and once more through the initial enumeration - and
   setlocale() is not thread-safe against itself. */
static pthread_mutex_t hid_init_mutex = PTHREAD_MUTEX_INITIALIZER;

int HID_API_EXPORT hid_init(void)
{
	const char *locale;

	/* indicate no error */
	if (!hid_internal_on_monitor_thread()) {
		register_global_error(NULL);
	}

	pthread_mutex_lock(&hid_init_mutex);
	/* Set the locale if it's not set. */
	locale = setlocale(LC_CTYPE, NULL);
	if (!locale)
		setlocale(LC_CTYPE, "");
	pthread_mutex_unlock(&hid_init_mutex);

	return 0;
}


int HID_API_EXPORT hid_exit(void)
{
	/* Free global error message */
	if (!hid_internal_on_monitor_thread()) {
		register_global_error(NULL);
	}

	hid_internal_hotplug_exit();
	
	return 0;
}

static int hid_internal_match_device_id(unsigned short vendor_id, unsigned short product_id, unsigned short expected_vendor_id, unsigned short expected_product_id)
{
    return (expected_vendor_id == 0x0 || vendor_id == expected_vendor_id) && (expected_product_id == 0x0 || product_id == expected_product_id);
}

/* Same as hid_enumerate, but distinguishes a genuine failure from an empty
   system: *failure (when non-NULL) is set to 1 only when the enumeration itself
   failed; the hotplug caller supplies the failure message. An empty result is
   not a failure. Used by the initial hotplug-registration snapshot, which must fail
   rather than arm the callbacks against an incomplete device set.
   quiet: don't touch the global error string - for a caller running on HIDAPI's
   monitor thread, which never writes it (see hidapi.h). The library is already
   initialized in that case, so the implicit hid_init() (which resets the string)
   is skipped as well. */
static struct hid_device_info *hid_internal_enumerate(unsigned short vendor_id, unsigned short product_id, int *failure, int quiet)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;

	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;

	if (failure) {
		*failure = 0;
	}

	if (!quiet) {
		hid_init();
		/* register_global_error: global error is reset by hid_init */
	}

	/* Create the udev object */
	udev = udev_new();
	if (!udev) {
		if (!quiet) {
			register_global_error("Couldn't create udev context");
		}
		if (failure) {
			*failure = 1;
		}
		return NULL;
	}

	/* Create a list of the devices in the 'hidraw' subsystem. */
	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		udev_unref(udev);
		if (!quiet) {
			register_global_error("Couldn't create udev enumeration");
		}
		if (failure) {
			*failure = 1;
		}
		return NULL;
	}
	if (udev_enumerate_add_match_subsystem(enumerate, "hidraw") < 0) {
		udev_enumerate_unref(enumerate);
		udev_unref(udev);
		if (!quiet) {
			register_global_error("Couldn't add the hidraw subsystem match to the udev enumeration");
		}
		if (failure) {
			*failure = 1;
		}
		return NULL;
	}
	if (udev_enumerate_scan_devices(enumerate) < 0) {
		udev_enumerate_unref(enumerate);
		udev_unref(udev);
		if (!quiet) {
			register_global_error("Couldn't scan the udev devices");
		}
		if (failure) {
			*failure = 1;
		}
		return NULL;
	}
	devices = udev_enumerate_get_list_entry(enumerate);
	/* For each item, see if it matches the vid/pid, and if so
	   create a udev_device record for it */
	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *sysfs_path;
		unsigned short dev_vid = 0;
		unsigned short dev_pid = 0;
		unsigned bus_type = 0;
		struct udev_device *raw_dev; /* The device's hidraw udev node. */
		struct hid_device_info * tmp;

		/* Get the filename of the /sys entry for the device
		   and create a udev_device object (dev) representing it */
		sysfs_path = udev_list_entry_get_name(dev_list_entry);
		if (!sysfs_path)
			continue;

		if (vendor_id != 0 || product_id != 0) {
			if (!parse_hid_vid_pid_from_sysfs(sysfs_path, &bus_type, &dev_vid, &dev_pid, quiet))
				continue;

			if (vendor_id != 0 && vendor_id != dev_vid)
				continue;
			if (product_id != 0 && product_id != dev_pid)
				continue;
		}

		raw_dev = udev_device_new_from_syspath(udev, sysfs_path);
		if (!raw_dev) {
			/* Conservatively fail the initial snapshot if a listed path cannot
			   be read, even for a racing unplug; registration reports the failure
			   and the caller may retry. hid_enumerate() passes failure == NULL
			   and keeps its best-effort behavior. */
			if (failure) {
				*failure = 1;
			}
			continue;
		}

		tmp = create_device_info_for_device(raw_dev, quiet, failure);
		if (tmp) {
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;

			/* move the pointer to the tail of returned list */
			while (cur_dev->next != NULL) {
				cur_dev = cur_dev->next;
			}
		}

		udev_device_unref(raw_dev);
	}
	/* Free the enumerator and udev objects. */
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	if (root == NULL && !quiet && !(failure && *failure)) {
		if (vendor_id == 0 && product_id == 0) {
			register_global_error("No HID devices found in the system.");
		} else {
			register_global_error("No HID devices with requested VID/PID found in the system.");
		}
	}

	return root;
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	return hid_internal_enumerate(vendor_id, product_id, NULL, 0);
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}

/* Deep copy of a single hid_device_info entry; the next pointer of the copy is always NULL */
static struct hid_device_info *hid_internal_copy_device_info(const struct hid_device_info *src)
{
	struct hid_device_info *dst = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
	if (dst == NULL) {
		return NULL;
	}

	dst->path = src->path? strdup(src->path): NULL;
	dst->vendor_id = src->vendor_id;
	dst->product_id = src->product_id;
	dst->serial_number = src->serial_number? wcsdup(src->serial_number): NULL;
	dst->release_number = src->release_number;
	dst->manufacturer_string = src->manufacturer_string? wcsdup(src->manufacturer_string): NULL;
	dst->product_string = src->product_string? wcsdup(src->product_string): NULL;
	dst->usage_page = src->usage_page;
	dst->usage = src->usage;
	dst->interface_number = src->interface_number;
	dst->next = NULL;
	dst->bus_type = src->bus_type;

	/* A partial copy must never reach a callback */
	if ((src->path && !dst->path)
		|| (src->serial_number && !dst->serial_number)
		|| (src->manufacturer_string && !dst->manufacturer_string)
		|| (src->product_string && !dst->product_string)) {
		hid_free_enumeration(dst);
		return NULL;
	}

	return dst;
}

/* Deliver the registration-time snapshot taken by hid_hotplug_register_callback()
   with HID_API_HOTPLUG_ENUMERATE: the initial pass of synthetic "arrived" events.
   Only ever runs on the monitor thread, with the mutex held. */
static void hid_internal_hotplug_replay(struct hid_hotplug_callback *callback)
{
	unsigned char old_state = hid_hotplug_context.mutex_in_use;
	hid_hotplug_context.mutex_in_use = 1;

	while (callback->replay) {
		/* Detach one entry at a time, so a deregistration from within the callback
		   (or from another thread, once we return) never sees a dangling list */
		struct hid_device_info *device = callback->replay;
		callback->replay = device->next;
		device->next = NULL;
		if ((*callback->callback)(callback->handle, device, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, callback->user_data)) {
			/* A non-zero return deregisters the callback and stops the remainder of the pass */
			callback->events = 0;
			hid_hotplug_context.cb_list_dirty = 1;
		}
		hid_free_enumeration(device);
		if (!callback->events) {
			/* Deregistered (by return value or from within the callback): drop the undelivered entries */
			hid_free_enumeration(callback->replay);
			callback->replay = NULL;
		}
	}

	hid_hotplug_context.mutex_in_use = old_state;
}

/* Deliver the pending initial passes of all registered callbacks.
   Only ever runs on the monitor thread, with the mutex held (and not in use). */
static void hid_internal_hotplug_process_replays(void)
{
	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback; callback = callback->next) {
		if (callback->events && callback->replay) {
			hid_internal_hotplug_replay(callback);
		}
	}

	hid_internal_hotplug_remove_postponed();
}

/* Deliver one live event to the matching callbacks whose handle does not
   exceed dispatch_bound. The bound freezes the dispatch to the callbacks
   registered before the event started: a callback registered from within
   a callback observes the device through its registration snapshot instead
   of the in-flight event, keeping the arrivals exactly-once (the handles
   are monotonic and the list is kept in registration order). */
static void hid_internal_invoke_callbacks(struct hid_device_info *info, hid_hotplug_event event, hid_hotplug_callback_handle dispatch_bound)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	hid_hotplug_context.mutex_in_use = 1;

	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs;
	     callback != NULL && callback->handle <= dispatch_bound;
	     callback = callback->next) {
		/* Flush the callback's pending initial pass first, so it never observes
		   a live event before the synthetic events of HID_API_HOTPLUG_ENUMERATE */
		if (callback->events && callback->replay) {
			hid_internal_hotplug_replay(callback);
		}
		if ((callback->events & event) && hid_internal_match_device_id(info->vendor_id, info->product_id,
																	   callback->vendor_id, callback->product_id)) {
			int result = callback->callback(callback->handle, info, event, callback->user_data);
			/* If the result is non-zero, we mark the callback for removal and proceed */
			if (result) {
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			}
		}
	}

	hid_hotplug_context.mutex_in_use = 0;
	hid_internal_hotplug_remove_postponed();
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

static struct hid_device_info *hid_internal_find_device_in_list(struct hid_device_info *list, const char *path)
{
	if (path == NULL) {
		return NULL;
	}
	for (struct hid_device_info *device = list; device; device = device->next) {
		if (device->path && !strcmp(device->path, path)) {
			return device;
		}
	}
	return NULL;
}

static struct hid_device_info *hid_internal_find_device_by_path(const char *path)
{
	return hid_internal_find_device_in_list(hid_hotplug_context.devs, path);
}

/* Handle a device arrival: update the connected-device cache and dispatch the
   callbacks. Takes ownership of the whole device chain (one entry per usage).
   Only ever runs on the monitor thread, with the mutex held. */
static void hid_internal_hotplug_process_arrival(struct hid_device_info *info)
{
	hid_hotplug_callback_handle dispatch_bound;
	struct hid_device_info *copies = NULL;
	struct hid_device_info **copies_tail = &copies;

	if (info == NULL) {
		return;
	}
	if (info->path == NULL) {
		hid_free_enumeration(info);
		return;
	}

	/* The device may already be known: a device arriving between arming the
	   udev monitor and taking the initial enumeration at first registration
	   is both in the cache and queued as an event on the monitor socket.
	   Skip the queued duplicate arrival. This devnode-only key cannot distinguish
	   a queued removal of a predecessor that reused the same /dev/hidrawN while
	   the snapshot already contains its successor: that window can still produce
	   ARRIVED/LEFT/ARRIVED for the successor. */
	if (hid_internal_find_device_by_path(info->path) != NULL) {
		hid_free_enumeration(info);
		return;
	}

	/* Copy every usage entry up front, for the one-entry-per-invocation
	   dispatch below (the callback must always see device->next == NULL, and
	   a copy keeps the cache walkable for the snapshots of callbacks
	   registered from within a callback). On failure the whole arrival is
	   dropped BEFORE anything is observable: dispatching entries of the live
	   cache chain instead would temporarily truncate the cache, and a
	   callback registered during such a dispatch would miss the truncated
	   entries in its snapshot - yet receive their "left" events later. */
	for (struct hid_device_info *info_cur = info; info_cur; info_cur = info_cur->next) {
		*copies_tail = hid_internal_copy_device_info(info_cur);
		if (*copies_tail == NULL) {
			/* Out of memory: this device is never reported at all */
			hid_free_enumeration(copies);
			hid_free_enumeration(info);
			return;
		}
		copies_tail = &(*copies_tail)->next;
	}

	/* Append to the cache BEFORE dispatching, so an ENUMERATE snapshot taken
	   by a callback registered from within a callback captures the whole
	   device rather than the in-flight event */
	if (hid_hotplug_context.devs != NULL) {
		struct hid_device_info *last = hid_hotplug_context.devs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = info;
	} else {
		hid_hotplug_context.devs = info;
	}

	/* Freeze the dispatch to the callbacks registered up to this point */
	dispatch_bound = hid_hotplug_context.next_handle - 1;

	while (copies != NULL) {
		struct hid_device_info *copy = copies;
		copies = copy->next;
		/* One usage entry per invocation */
		copy->next = NULL;
		hid_internal_invoke_callbacks(copy, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, dispatch_bound);
		hid_free_enumeration(copy);
	}
}

/* Handle a device removal: detach the matching entries from the
   connected-device cache and dispatch the callbacks.
   Only ever runs on the monitor thread, with the mutex held. */
static void hid_internal_hotplug_process_removal(const char *devnode)
{
	/* Freeze the dispatch to the callbacks registered up to this point
	   (see hid_internal_hotplug_process_arrival) */
	hid_hotplug_callback_handle dispatch_bound = hid_hotplug_context.next_handle - 1;
	struct hid_device_info *removed = NULL;
	struct hid_device_info **removed_tail = &removed;

	/* Detach every usage entry of the device from the cache BEFORE dispatching
	   any of them: a callback registered from within this dispatch is excluded
	   from it by the bound, so it must not be able to capture a still-cached
	   usage entry of the leaving device in its ENUMERATE snapshot either -
	   that would be an "arrived" without a matching "left" */
	for (struct hid_device_info **current = &hid_hotplug_context.devs; *current;) {
		struct hid_device_info *info = *current;
		if (info->path && !strcmp(devnode, info->path)) {
			*current = info->next;
			info->next = NULL;
			*removed_tail = info;
			removed_tail = &info->next;
		} else {
			current = &info->next;
		}
	}

	while (removed != NULL) {
		struct hid_device_info *info = removed;
		removed = info->next;
		/* One usage entry per invocation: the callback always sees next == NULL */
		info->next = NULL;
		hid_internal_invoke_callbacks(info, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, dispatch_bound);
		/* Free every removed device */
		hid_free_enumeration(info);
	}
}

/* Dispatch one udev monitor event.
   Only ever runs on the monitor thread, with the mutex held. */
static void hid_internal_hotplug_process_event(struct udev_device *raw_dev)
{
	const char *action = udev_device_get_action(raw_dev);
	if (action == NULL) {
		return;
	}

	if (!strcmp(action, "add")) {
		/* We create a list of all usages on this UDEV device.
		   A device whose information cannot be read/parsed stays out of the
		   cache and is invisible to the callbacks - exactly as it would be
		   invisible to hid_enumerate(); there is nothing meaningful to
		   deliver instead. quiet: this runs on the monitor thread, which
		   never writes the global error string (see hidapi.h). */
		int arrival_failure = 0;
		struct hid_device_info *info = create_device_info_for_device(raw_dev, 1, &arrival_failure);
		if (arrival_failure) {
			/* Drop incomplete or degraded chains on resource failure.
			   process_arrival also rejects devices with no devnode, since those
			   cannot be matched by removal or recognized as duplicates. */
			hid_free_enumeration(info);
			info = NULL;
		}
		hid_internal_hotplug_process_arrival(info);
	} else if (!strcmp(action, "remove")) {
		const char *devnode = udev_device_get_devnode(raw_dev);
		char devnode_buf[32];
		if (devnode == NULL) {
			/* A remove event does not always carry the device node. The
			   cache is keyed by the node path, which for a hidraw device is
			   always "/dev/<sysname>": reconstruct it, or the stale cache
			   entry would suppress - as a duplicate - the arrival of the
			   next device that reuses the same node */
			const char *sysname = udev_device_get_sysname(raw_dev);
			if (sysname != NULL) {
				int len = snprintf(devnode_buf, sizeof(devnode_buf), "/dev/%s", sysname);
				if (len > 0 && (size_t)len < sizeof(devnode_buf)) {
					devnode = devnode_buf;
				}
			}
		}
		if (devnode != NULL) {
			hid_internal_hotplug_process_removal(devnode);
		}
	}
}

/* Consecutive poll() reports of an error condition on the udev monitor
   socket - with a drain attempt in between each - after which the socket is
   considered dead. Receive errors, including ENOBUFS, stop live delivery
   immediately: consuming an overrun error cannot restore lost event history. */
#define HID_HOTPLUG_SOCKET_ERROR_POLL_LIMIT 100

static void* hotplug_thread(void* user_data)
{
	int monitor_fd;
	int socket_error_polls = 0;
	int socket_dead = 0;
	struct pollfd fds;
	int ret = 0;
	int poll_error = 0;

	(void) user_data;

	/* The startup lock waits for thread_id, published after pthread_create().
	   The creating registration releases it after publishing RUNNING, and no
	   reaper can be joining this generation yet. monitor_fd is already ordered
	   by pthread_create() and stays immutable until this thread winds down. */
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	/* Learn our own generation id, for the join-safe self-identification the
	   library needs once this thread's pthread_t may have been claimed for a
	   join (see hid_hotplug_thread_self_id). The value read here is ours: the
	   creating registration holds the mutex continuously from pthread_create()
	   until after it published this id, and a newer generation cannot be
	   created before this one publishes FINISHED below. */
	hid_hotplug_thread_self_id = hid_hotplug_context.thread_id;
	monitor_fd = hid_hotplug_context.monitor_fd;
	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	/* From here on the main loop takes the mutex with trylock only: it must never
	   block on the mutex, so that it always makes progress towards its exit check
	   while hid_internal_hotplug_cleanup() waits, without holding the mutex,
	   for it to announce its own exit. All shared state, including the loop
	   decisions, is only ever accessed with the mutex held. */

	for (;;) {
		int stop = 0;

		if (pthread_mutex_trylock(&hid_hotplug_context.mutex) != 0) {
			/* Contended: back off shortly and retry. Polling the monitor fd
			   here would spin, as it stays readable until the queued events
			   are drained (which needs the mutex). */
			poll(NULL, 0, 1);
			continue;
		}

		/* Classify the previous poll result and publish death under the same
		   lock, so a registration cannot attach between those two actions. */
		if (ret < 0) {
			if (poll_error != EINTR && ++socket_error_polls >= HID_HOTPLUG_SOCKET_ERROR_POLL_LIMIT) {
				socket_dead = 1;
			}
		} else if (ret > 0 && !(fds.revents & POLLIN)) {
			if (fds.revents & (POLLHUP | POLLNVAL)) {
				socket_dead = 1;
			} else if (++socket_error_polls >= HID_HOTPLUG_SOCKET_ERROR_POLL_LIMIT) {
				socket_dead = 1;
			}
		} else {
			socket_error_polls = 0;
		}

		if (hid_hotplug_context.hotplug_cbs == NULL) {
			/* The last callback is gone: release the monitoring context (the
			   device cache, the udev monitor and its context) right away -
			   when the last callback removed itself from within a callback,
			   no further hotplug call is guaranteed to come and reap it -
			   then publish the exit under the mutex and stop touching any
			   shared state. A monitoring context created after this point
			   belongs to a new thread; the release is idempotent, so a
			   claimant repeating it is a no-op. */
			hid_internal_hotplug_release_monitor();
			hid_hotplug_context.monitor_dead = 0;
			/* Publish FINISHED as the last write under the mutex (the thread
			   touches no shared state below this point, and everything it owned
			   has just been released), then unlock and return. The pthread_t
			   stays JOINABLE: hid_exit() - and the next register/deregister -
			   reap it (see hid_internal_hotplug_reap_thread), so no
			   monitor-thread instruction is still running inside the library
			   once hid_exit() returns. When the last callback deregistered
			   itself from within a callback and the application then never calls
			   HIDAPI again, this one pthread_t lingers unjoined until hid_exit()
			   reaps it - a bounded, single-thread leak that is strictly better
			   than a detached thread resuming in unmapped code after dlclose(). */
			hid_hotplug_context.thread_state = HID_HOTPLUG_THREAD_FINISHED;
			stop = 1;
		} else {
			if (socket_dead && !hid_hotplug_context.monitor_dead) {
				/* The udev monitor socket died unrecoverably (see the poll()
				   handling below). Stop delivering events cleanly: losing the
				   event transport is NOT evidence that the devices left, so do
				   NOT fabricate removals and do NOT re-enumerate or diff. The
				   device cache is left exactly as it is and the application's
				   open handles are untouched (those devices are still physically
				   connected). Mark the machinery dead so new registrations are
				   refused (hid_hotplug_register_callback) - that refusal is the
				   observable failure; the global error string is NOT written
				   from this thread (see hidapi.h).

				   After an unrecoverable monitor failure, no further live hotplug
				   event is delivered for this machinery generation; devices
				   already reported remain in the cache and open handles are
				   unaffected. The thread keeps idling (still flushing any pending
				   initial ENUMERATE passes) until its callbacks are deregistered,
				   then winds down and releases everything (the dead monitor
				   included). */
				hid_hotplug_context.monitor_dead = 1;
			}

			/* Deliver the pending initial passes of HID_API_HOTPLUG_ENUMERATE
			   before any queued live events */
			hid_internal_hotplug_process_replays();

			if (!socket_dead) {
				/* Drain and dispatch the events queued on the (non-blocking)
				   udev monitor socket */
				for (;;) {
					errno = 0;
					struct udev_device *raw_dev = udev_monitor_receive_device(hid_hotplug_context.mon);
					if (raw_dev == NULL) {
						if (errno == EINTR) {
							continue;
						}
						if (errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
							socket_dead = 1;
							hid_hotplug_context.monitor_dead = 1;
						}
						break;
					}
					hid_internal_hotplug_process_event(raw_dev);
					udev_device_unref(raw_dev);
				}
			}
		}

		pthread_mutex_unlock(&hid_hotplug_context.mutex);

		if (stop) {
			break;
		}

		if (socket_dead) {
			/* The dead socket is no longer polled; release_monitor frees it
			   when the last callback is gone. Pace replay flushes and exit checks. */
			ret = 0;
			nanosleep(&(struct timespec){0, 5000000}, NULL);
			continue;
		}
		if (socket_error_polls && !(ret < 0 && poll_error == EINTR)) {
			/* Pace persistent poll failures independently of poll() itself. */
			nanosleep(&(struct timespec){0, 5000000}, NULL);
		}

		/* Wait for udev events; the timeout paces the mutex retries and caps
		   the latency of the initial HID_API_HOTPLUG_ENUMERATE passes.
		   5 msec seems reasonable; don't set too low to avoid high CPU usage. */
		fds.fd = monitor_fd;
		fds.events = POLLIN;
		fds.revents = 0;
		ret = poll(&fds, 1, 5);
		if (ret < 0) {
			poll_error = errno;
		}
	}

	return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_register_callback(unsigned short vendor_id, unsigned short product_id, int events, int flags, hid_hotplug_callback_fn callback, void *user_data, hid_hotplug_callback_handle *callback_handle)
{
	struct hid_hotplug_callback* hotplug_cb;
	int quiet;

	/* No events can be delivered before the out parameter is written */
	if (callback_handle != NULL) {
		*callback_handle = 0;
	}

	/* Ensure we are ready to actually use the mutex (the mutex is
	   process-lifetime and never destroyed - see hid_internal_hotplug_init).
	   This can only fail before any callback was ever registered - i.e. never
	   on the monitor thread - so the error string is safe to write here */
	if (hid_internal_hotplug_init() != 0) {
		register_global_error("Couldn't initialize the hotplug mutex");
		return -1;
	}

	/* Lock the mutex to avoid race conditions */
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* A registration made from within a callback runs on HIDAPI's internal
	   monitor thread, which holds this (recursive) mutex for the whole
	   dispatch - hence mutex_in_use. Such a call must leave the global error
	   string alone: HIDAPI's internal threads never write it (see hidapi.h),
	   or an application that reads it with hid_error(NULL) on another thread
	   would race a write it cannot serialize against. This is why even the
	   parameter checks below run under the mutex.
	   mutex_in_use only covers a call made from within a dispatch: a
	   thread-specific-data destructor armed by a user callback re-enters here on
	   the monitor thread after the dispatch unwound (and after the thread
	   published FINISHED), so the thread identity is checked as well. */
	quiet = hid_hotplug_context.mutex_in_use || hid_internal_on_monitor_thread();

	/* Check params */
	if (callback == NULL) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("Hotplug callback function is NULL");
		}
		return -1;
	}
	if (events == 0
		|| (events & ~(HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT))) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("Hotplug events mask contains no valid events or unknown bits");
		}
		return -1;
	}
	if (flags & ~(HID_API_HOTPLUG_ENUMERATE)) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("Hotplug flags mask contains unknown bits");
		}
		return -1;
	}

	if (hid_hotplug_context.exiting) {
		/* hid_exit() is tearing the machinery down: it deregisters every
		   callback itself, so there is nothing to register into */
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("hid_exit() is in progress");
		}
		return -1;
	}

	if (!quiet) {
		/* Implicit hid_init: unlike the other API entry points, concurrent
		   registrations are allowed - hid_init() serializes itself.
		   A nested registration skips it: the library is initialized by then,
		   and hid_init() resets the global error string (see above) */
		hid_init();
		/* register_global_error: global error is reset by hid_init */
	}

	hotplug_cb = (struct hid_hotplug_callback*)calloc(1, sizeof(struct hid_hotplug_callback));

	if (hotplug_cb == NULL) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("Failed to allocate a hotplug callback record");
		}
		return -1;
	}

	/* Fill out the record */
	hotplug_cb->next = NULL;
	hotplug_cb->vendor_id = vendor_id;
	hotplug_cb->product_id = product_id;
	hotplug_cb->events = events;
	hotplug_cb->user_data = user_data;
	hotplug_cb->callback = callback;
	hotplug_cb->replay = NULL;

	/* Reap the monitor thread first in case it is exiting (or has exited)
	   after the removal of its last callback, releasing the previous
	   monitoring context with it (may temporarily drop the mutex) */
	hid_internal_hotplug_cleanup();

	/* hid_exit() may have started while the mutex was dropped above: fail
	   instead of re-arming the machinery it is tearing down */
	if (hid_hotplug_context.exiting) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		free(hotplug_cb);
		if (!quiet) {
			register_global_error("hid_exit() is in progress");
		}
		return -1;
	}

	if (hid_hotplug_context.monitor_dead) {
		/* The udev monitor socket died (see hotplug_thread): this machinery
		   can deliver no further events, so refuse to attach to it. It winds
		   down once the surviving callbacks are deregistered; a registration
		   after that rebuilds it. */
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		free(hotplug_cb);
		if (!quiet) {
			register_global_error("The hotplug monitor is not operational (the udev monitor socket failed)");
		}
		return -1;
	}

	/* The handles are monotonic and never reused: fail instead of overflowing */
	if (hid_hotplug_context.next_handle == INT_MAX) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		free(hotplug_cb);
		if (!quiet) {
			register_global_error("Hotplug callback handles exhausted");
		}
		return -1;
	}

	/* Allocate the handle only after hid_internal_hotplug_cleanup() above:
	   it may temporarily drop the mutex, and the dispatch bounds rely on the
	   callback list being in (monotonic) handle order - a handle allocated
	   before the drop could get linked in after a younger one. The mutex is
	   then held continuously from this allocation up to (at least) the
	   unwinding point of every failure path below, so those paths can safely
	   return the handle to the counter (a failed registration must not
	   consume handles). */
	hotplug_cb->handle = hid_hotplug_context.next_handle++;

	/* Append a new callback to the end */
	if (hid_hotplug_context.hotplug_cbs != NULL) {
		struct hid_hotplug_callback *last = hid_hotplug_context.hotplug_cbs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = hotplug_cb;
	}
	else {
		int enumerate_failure = 0;
		const char *monitor_error = NULL;

		/* cleanup settles an empty callback list to NONE. Never overwrite a
		   generation if that invariant changes. */
		if (hid_hotplug_context.thread_state != HID_HOTPLUG_THREAD_NONE) {
			hid_hotplug_context.next_handle--;
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			if (!quiet) {
				register_global_error("The previous hotplug monitor is still active");
			}
			return -1;
		}

		/* This branch is never taken from within a dispatch: the callback list
		   is empty here, while a dispatch always has at least the callback it
		   is dispatching to in the list (a deregistration from within a
		   callback only tombstones the record). It CAN still run on the monitor
		   thread - a thread-specific-data destructor re-registering after the
		   last callback is gone - so the global error writes below, the
		   hid_internal_enumerate() ones included, are all made conditional on
		   quiet: HIDAPI's internal threads never write that string */

		/* Prepare a UDEV context to run monitoring on */
		hid_hotplug_context.udev_ctx = udev_new();
		if (!hid_hotplug_context.udev_ctx) {
			monitor_error = "Couldn't create udev context";
		}

		if (!monitor_error) {
			hid_hotplug_context.mon = udev_monitor_new_from_netlink(hid_hotplug_context.udev_ctx, "udev");
			if (!hid_hotplug_context.mon) {
				monitor_error = "Couldn't create udev monitor";
			}
		}
		if (!monitor_error && udev_monitor_filter_add_match_subsystem_devtype(hid_hotplug_context.mon, "hidraw", NULL) < 0) {
			monitor_error = "Couldn't add the hidraw filter to the udev monitor";
		}
		if (!monitor_error && udev_monitor_enable_receiving(hid_hotplug_context.mon) < 0) {
			monitor_error = "Couldn't enable receiving on the udev monitor";
		}
		if (!monitor_error) {
			/* 0 is a valid file descriptor: only negative values are errors */
			hid_hotplug_context.monitor_fd = udev_monitor_get_fd(hid_hotplug_context.mon);
			if (hid_hotplug_context.monitor_fd < 0) {
				monitor_error = "Couldn't get the udev monitor file descriptor";
			}
		}

		if (!monitor_error) {
			/* After monitoring is all set up, enumerate all devices: a failure
			   here would leave pre-connected devices without their "left"
			   events later, so it fails the registration (unlike an empty
			   system, which is not an error) */
			hid_hotplug_context.devs = hid_internal_enumerate(0, 0, &enumerate_failure, quiet);
			if (!enumerate_failure && !quiet) {
				register_global_error(NULL);
			}
		}

		if (monitor_error || enumerate_failure) {
			/* The handle never became visible: return it to the counter */
			hid_hotplug_context.next_handle--;
			hid_internal_hotplug_release_monitor();
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			if (monitor_error && !quiet) {
				register_global_error(monitor_error);
			} else if (enumerate_failure && !quiet) {
				register_global_error("Couldn't take the initial device snapshot for the hotplug registration");
			}
			return -1;
		}

		/* Don't forget to actually register the callback */
		hid_hotplug_context.hotplug_cbs = hotplug_cb;

		/* Start the event-scanning thread. cleanup's loop exit condition and the
		   defensive check above guarantee the current slot is NONE here.
		   The thread is JOINABLE: after releasing its context it publishes
		   FINISHED, and an application-thread register/deregister that finds the
		   callback list empty, or hid_exit(), joins it. */
		/* Reserve its node before creation; see hid_internal_hotplug_retire_current. */
		struct hid_hotplug_monitor_thread *thread_node =
			(struct hid_hotplug_monitor_thread *)calloc(1, sizeof(*thread_node));
		if (thread_node == NULL) {
			hid_hotplug_context.hotplug_cbs = NULL;
			/* The handle never became visible: return it to the counter */
			hid_hotplug_context.next_handle--;
			hid_internal_hotplug_release_monitor();
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			if (!quiet) {
				register_global_error("Couldn't allocate the hotplug monitor thread record");
			}
			return -1;
		}

		/* Always a no-op after cleanup settles the slot; retained as a guard
		   against a future cleanup change leaving a FINISHED predecessor. */
		hid_internal_hotplug_retire_current();

		int thread_error = pthread_create(&hid_hotplug_context.thread, NULL, &hotplug_thread, NULL);

		if (thread_error) {
			free(thread_node);
			hid_hotplug_context.hotplug_cbs = NULL;
			/* The handle never became visible: return it to the counter */
			hid_hotplug_context.next_handle--;
			hid_internal_hotplug_release_monitor();
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			if (!quiet) {
				register_global_error("Couldn't create the hotplug monitor thread");
			}
			return -1;
		}
		/* The new generation now owns its pre-allocated retired-list node. Stamp
		   it with a fresh stable id token, then publish it RUNNING (all under the
		   mutex, before the thread can do anything). */
		hid_hotplug_context.thread_node = thread_node;
		hid_hotplug_context.thread_id = hid_hotplug_context.next_thread_id;
		if (++hid_hotplug_context.next_thread_id == 0) {
			hid_hotplug_context.next_thread_id = 1;
		}
		hid_hotplug_context.thread_state = HID_HOTPLUG_THREAD_RUNNING;
	}

	if ((flags & HID_API_HOTPLUG_ENUMERATE) && (events & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)) {
		/* Take a snapshot of the matching connected devices: the monitor thread
		   delivers it asynchronously as the initial pass of synthetic "arrived"
		   events, before any live events for this callback and never from
		   within this call */
		int snapshot_failure = 0;
		struct hid_device_info **replay_tail = &hotplug_cb->replay;
		for (struct hid_device_info *device = hid_hotplug_context.devs; device != NULL; device = device->next) {
			if (!hid_internal_match_device_id(device->vendor_id, device->product_id, hotplug_cb->vendor_id, hotplug_cb->product_id)) {
				continue;
			}
			*replay_tail = hid_internal_copy_device_info(device);
			if (*replay_tail == NULL) {
				snapshot_failure = 1;
				break;
			}
			replay_tail = &(*replay_tail)->next;
		}

		if (snapshot_failure) {
			/* The initial pass is all-or-nothing (each device connection is
			   reported exactly once - never "neither"): unwind the whole
			   registration. No event can have been delivered yet: the mutex
			   was held since the callback was linked in. */
			for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
				if (*current == hotplug_cb) {
					*current = hotplug_cb->next;
					break;
				}
			}
			hid_free_enumeration(hotplug_cb->replay);
			/* The handle never became visible: return it to the counter */
			hid_hotplug_context.next_handle--;
			/* Reap the monitor thread in case this was the only callback */
			hid_internal_hotplug_cleanup();
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			if (!quiet) {
				register_global_error("Couldn't allocate the device snapshot for the hotplug enumerate pass");
			}
			return -1;
		}
	}

	/* Return the allocated handle: written before the mutex is released, i.e.
	   before any event can be delivered to the callback */
	if (callback_handle != NULL) {
		*callback_handle = hotplug_cb->handle;
	}

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	return 0;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_deregister_callback(hid_hotplug_callback_handle callback_handle)
{
	int quiet;

	if (hid_internal_hotplug_init() != 0) {
		/* The hotplug mutex could not be created: nothing can ever have been
		   registered (and this can never run on the monitor thread, so the
		   error string is safe to write here) */
		register_global_error("No hotplug callbacks are registered");
		return -1;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* A deregistration made from within a callback - or from a thread-specific-
	   data destructor running on the monitor thread once the dispatch unwound -
	   runs on HIDAPI's internal monitor thread, which never writes the global
	   error string, the parameter check included (see
	   hid_hotplug_register_callback) */
	quiet = hid_hotplug_context.mutex_in_use || hid_internal_on_monitor_thread();

	if (callback_handle <= 0) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("Invalid hotplug callback handle");
		}
		return -1;
	}

	if (hid_hotplug_context.exiting) {
		/* hid_exit() is tearing the machinery down: it deregisters every
		   callback and invalidates every handle itself */
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("No hotplug callbacks are registered");
		}
		return -1;
	}

	/* Reap a monitor thread that has wound down after the removal of its last
	   callback but has not been joined yet (may temporarily drop the mutex) */
	hid_internal_hotplug_cleanup();

	/* hid_exit() may have started while the mutex was dropped above */
	if (hid_hotplug_context.exiting) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("No hotplug callbacks are registered");
		}
		return -1;
	}

	if (hid_hotplug_context.hotplug_cbs == NULL) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		if (!quiet) {
			register_global_error("No hotplug callbacks are registered");
		}
		return -1;
	}

	int result = -1;

	/* Remove this notification */
	for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
		if ((*current)->handle == callback_handle) {
			/* A record deregistered from within a callback (awaiting its
			   postponed removal) is already gone for the caller: not found */
			if (!(*current)->events) {
				break;
			}
			/* Check if we were already in a locked state, as we are NOT allowed to remove any callbacks if we are */
			if (hid_hotplug_context.mutex_in_use) {
				(*current)->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			} else {
				struct hid_hotplug_callback *next = (*current)->next;
				/* A deregistered callback never fires again: drop its undelivered snapshot */
				hid_free_enumeration((*current)->replay);
				free(*current);
				*current = next;
			}
			result = 0;
			break;
		}
	}

	hid_internal_hotplug_cleanup();

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	if (result < 0 && !quiet) {
		register_global_error("Hotplug callback handle not found");
	} else if (!quiet) {
		register_global_error(NULL);
	}

	return result;
}

hid_device * hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	/* register_global_error: global error is reset by hid_enumerate/hid_init */
	devs = hid_enumerate(vendor_id, product_id);
	if (devs == NULL) {
		/* register_global_error: global error is already set by hid_enumerate */
		return NULL;
	}

	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	} else {
		register_global_error("Device with requested VID/PID/(SerialNumber) not found");
	}

	hid_free_enumeration(devs);

	return handle;
}

hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
	hid_device *dev = NULL;

	hid_init();
	/* register_global_error: global error is reset by hid_init */

	dev = new_hid_device();
	if (!dev) {
		errno = ENOMEM;
		register_global_error("Couldn't allocate memory");
		return NULL;
	}

	dev->device_handle = open(path, O_RDWR | O_CLOEXEC);

	if (dev->device_handle >= 0) {
		int res, desc_size = 0;

		/* Make sure this is a HIDRAW device - responds to HIDIOCGRDESCSIZE */
		res = ioctl(dev->device_handle, HIDIOCGRDESCSIZE, &desc_size);
		if (res < 0) {
			register_global_error_format("ioctl(GRDESCSIZE) error for '%s', not a HIDRAW device?: %s", path, strerror(errno));
			hid_close(dev);
			return NULL;
		}

		return dev;
	}
	else {
		/* Unable to open a device. */
		free(dev);
		register_global_error_format("Failed to open a device with path '%s': %s", path, strerror(errno));
		return NULL;
	}
}


int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	int bytes_written;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	bytes_written = write(dev->device_handle, data, length);

	register_device_error(dev, (bytes_written == -1)? strerror(errno): NULL);

	return bytes_written;
}


int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	if (!data || (length == 0)) {
		errno = EINVAL;
		register_error_str(&dev->last_read_error_str, "Zero buffer/length");
		return -1;
	}

	/* Set device error to none */
	register_error_str(&dev->last_read_error_str, NULL);

	int bytes_read;

	if (milliseconds >= 0) {
		/* Milliseconds is either 0 (non-blocking) or > 0 (contains
		   a valid timeout). In both cases we want to call poll()
		   and wait for data to arrive.  Don't rely on non-blocking
		   operation (O_NONBLOCK) since some kernels don't seem to
		   properly report device disconnection through read() when
		   in non-blocking mode.  */
		int ret;
		struct pollfd fds;

		fds.fd = dev->device_handle;
		fds.events = POLLIN;
		fds.revents = 0;
		ret = poll(&fds, 1, milliseconds);
		if (ret == 0) {
			/* Timeout */
			return ret;
		}
		if (ret == -1) {
			/* Error */
			register_error_str(&dev->last_read_error_str, strerror(errno));
			return ret;
		}
		else {
			/* Check for errors on the file descriptor. This will
			   indicate a device disconnection. */
			if (fds.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				// We cannot use strerror() here as no -1 was returned from poll().
				errno = EIO;
				register_error_str(&dev->last_read_error_str, "hid_read_timeout: unexpected poll error (device disconnected)");
				return -1;
			}
		}
	}

	bytes_read = read(dev->device_handle, data, length);
	if (bytes_read < 0) {
		if (errno == EAGAIN || errno == EINPROGRESS)
			bytes_read = 0;
		else
			register_error_str(&dev->last_read_error_str, strerror(errno));
	}

	return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_read_error(hid_device *dev)
{
	if (dev->last_read_error_str == NULL)
		return L"Success";
	return dev->last_read_error_str;
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
	/* Do all non-blocking in userspace using poll(), since it looks
	   like there's a bug in the kernel in some versions where
	   read() will not return -1 on disconnection of the USB device */

	dev->blocking = !nonblock;
	return 0; /* Success */
}

int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	int res;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCSFEATURE(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (SFEATURE): %s", strerror(errno));

	return res;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCGFEATURE(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (GFEATURE): %s", strerror(errno));

	return res;
}

int HID_API_EXPORT HID_API_CALL hid_send_output_report(hid_device *dev, const unsigned char *data, size_t length)
{
	int res;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCSOUTPUT(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (SOUTPUT): %s", strerror(errno));

	return res;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCGINPUT(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (GINPUT): %s", strerror(errno));

	return res;
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	close(dev->device_handle);

	free(dev->last_error_str);
	free(dev->last_read_error_str);

	hid_free_enumeration(dev->device_info);

	free(dev);
}


int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->manufacturer_string) {
		wcsncpy(string, info->manufacturer_string, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->product_string) {
		wcsncpy(string, info->product_string, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->serial_number) {
		wcsncpy(string, info->serial_number, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}


HID_API_EXPORT struct hid_device_info *HID_API_CALL hid_get_device_info(hid_device *dev) {
	if (dev->device_info) {
		register_device_error(dev, NULL);
	}
	else {
		// Lazy initialize device_info
		dev->device_info = create_device_info_for_hid_device(dev);
	}

	// create_device_info_for_hid_device will set an error if needed
	return dev->device_info;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	(void)string_index;
	(void)string;
	(void)maxlen;

	errno = ENOSYS;
	register_device_error(dev, "hid_get_indexed_string: not supported by hidraw");

	return -1;
}


int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device *dev, unsigned char *buf, size_t buf_size)
{
	struct hidraw_report_descriptor rpt_desc;

	if (!buf || !buf_size) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	int res = get_hid_report_descriptor_from_hidraw(dev, &rpt_desc);
	if (res < 0) {
		/* error already registered */
		return res;
	}

	if (rpt_desc.size < buf_size) {
		buf_size = (size_t) rpt_desc.size;
	}

	memcpy(buf, rpt_desc.value, buf_size);

	return (int) buf_size;
}


/* Passing in NULL means asking for the last global error message. */
HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->last_error_str == NULL)
			return L"Success";
		return dev->last_error_str;
	}

	if (last_global_error_str == NULL)
		return L"Success";
	return last_global_error_str;
}
