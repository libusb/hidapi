#include <dev/evdev/input.h>
#include <dev/hid/hid.h>
#include <dev/hid/hidraw.h>
#include <dev/usb/usb_ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <libusb.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include "hidapi.h"

static struct hid_api_version hid_api_ver = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

static wchar_t *global_error_str = NULL;
static libusb_context *global_usb_context = NULL;

struct hid_device_ {
	int device_handle;
	int blocking;
	int idx;
	wchar_t *error_str;
	wchar_t *read_error_str;
	const char *device_path;
	struct hid_device_info* device_info;
};

static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (dev == NULL) {
		return NULL;
	}

	dev->device_handle = -1;
	dev->blocking = 1;
	dev->error_str = NULL;
	dev->read_error_str = NULL;
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


/* Makes a copy of the given error message (and decoded according to the
 * currently locale) into the wide string pointer pointed by error_str.
 * The last stored error string is freed.
 * Use register_error_str(NULL) to free the error message completely. */
static void register_error_str(wchar_t **error_str, const char *msg)
{
	free(*error_str);
	*error_str = utf8_to_wchar_t(msg);
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
	register_error_str(&global_error_str, msg);
}

/* Similar to register_global_error, but allows passing a format string into this function. */
static void register_global_error_format(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&global_error_str, format, args);
	va_end(args);
}

/* Set the last error for a device to be reported by hid_error(dev).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored device error message is freed.
 * Use register_device_error(dev, NULL) to indicate "no error". */
static void register_device_error(hid_device *dev, const char *msg)
{
	register_error_str(&dev->error_str, msg);
}

/* Similar to register_device_error, but you can pass a format string into this function. */
static void register_device_error_format(hid_device *dev, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&dev->error_str, format, args);
	va_end(args);
}


struct hid_sysctl_iter {
	int cur_oid[CTL_MAXNAME];
	size_t matched_len;
	size_t oid_items;
	int idx;
};

static int hid_get_name_from_mib(int *mib, int items, char *name_buffer, size_t buffer_size) {
	int q_name_oid[CTL_MAXNAME] = {0};
	int result = -1;

	q_name_oid[0] = CTL_SYSCTL;
	q_name_oid[1] = CTL_SYSCTL_NAME;
	memcpy(q_name_oid + 2, mib, items * sizeof(int));

	if (sysctl(q_name_oid, items + 2, name_buffer, &buffer_size, NULL, 0)) {
		register_global_error_format("sysctl(CTL_SYSCTL_NAME): %s", strerror(errno));
		return 0;
	}
	
	return 1;
}

static int hid_get_dev_idx_from_mib(int *mib, int items) {
	char name_buffer[256];
	int result = 0;

	if (hid_get_name_from_mib(mib, items, name_buffer, sizeof(name_buffer)) == 0)
		return -1;

	return sscanf(name_buffer, "dev.hidraw.%d", &result) == 1 ? result : -1;
}

static int hid_get_next_from_oid(int *oid, size_t *items, int total_items) {
	int q_next_oid[CTL_MAXNAME], recv_oid[CTL_MAXNAME];
	int found = 0;
	size_t cur_items = *items, recv_items;
	q_next_oid[0] = CTL_SYSCTL;
	q_next_oid[1] = CTL_SYSCTL_NEXTNOSKIP;

	memcpy(recv_oid, oid, cur_items * sizeof(int));

	while (1) {
		memcpy(q_next_oid + 2, recv_oid, cur_items * sizeof(int));
		recv_items = sizeof(recv_oid);
		if (sysctl(q_next_oid, cur_items + 2, recv_oid, &recv_items, NULL, 0)) {
			register_global_error_format("sysctl(CTL_SYSCTL_NEXT): %s", strerror(errno));
			break;
		}
		recv_items /= sizeof(int);
		cur_items = recv_items;
		if (recv_items < (total_items - 1) ||
			memcmp(recv_oid, oid, (total_items - 1) * sizeof(int)))
			break;
		if (recv_items != total_items)
			continue;
		memcpy(oid, recv_oid, recv_items * sizeof(int));
		*items = total_items;
		found = 1;
		break;
	}

	return found;
}

static int hid_init_sysctl_iter(struct hid_sysctl_iter *iter, const char *mib) {
	iter->oid_items = CTL_MAXNAME;
	if (sysctlnametomib(mib, iter->cur_oid, &iter->oid_items)) {
		register_global_error_format("sysctlnametomib: %s", strerror(errno));
		return 0;
	}

	iter->matched_len = iter->oid_items + 1;
	iter->idx = -1;
	return 1;
}

static int hid_get_next_from_sysctl_iter(struct hid_sysctl_iter *iter) {
	int found = hid_get_next_from_oid(iter->cur_oid, &iter->oid_items, iter->matched_len);
	if (!found)
		return 0;
	iter->idx = hid_get_dev_idx_from_mib(iter->cur_oid, iter->oid_items);
	return iter->idx == -1 ? 0 : 1;
}

/*
 * Gets the size of the HID item at the given position
 * Returns 1 if successful, 0 if an invalid key
 * Sets data_len and key_size when successful
 */
static int get_hid_item_size(const uint8_t *report_descriptor, uint32_t size, unsigned int pos, int *data_len, int *key_size)
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
static uint32_t get_hid_report_bytes(const uint8_t *rpt, size_t len, size_t num_bytes, size_t cur)
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
 * 0 when error is occured (broken Descriptor, end of a Collection is found before its begin,
 *  or no Collection is found at all).
 */
static int hid_iterate_over_collection(const uint8_t *report_descriptor, uint32_t size, unsigned int *pos, int *data_len, int *key_size)
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
static int get_next_hid_usage(const uint8_t *report_descriptor, uint32_t size, struct hid_usage_iterator *ctx, unsigned short *usage_page, unsigned short *usage)
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

static libusb_device_handle *hid_find_device_handle_by_bus_and_port(int bus, int addr) {
	libusb_device **list;
	libusb_device_handle *handler = NULL;
	int error, num_devs;

	error = libusb_get_device_list(global_usb_context, &list);
	if (error < 0) {
		register_global_error_format("hid_find_device_handle_by_bus_and_port: %s", libusb_strerror(errno));
		return NULL;
	}

	num_devs = error;

	for (int i = 0; i < num_devs; ++i) {
		libusb_device *dev = list[i];
		if (bus != libusb_get_bus_number(dev) || addr != libusb_get_device_address(dev))
			continue;
		libusb_open(dev, &handler);
		break;
	}

	if (handler == NULL) {
		register_global_error_format("Unable to find the device with bus:%2d and addr:%2d", bus, addr);
	}

	libusb_free_device_list(list, 0);
	return handler;
}

static int hid_get_udev_location_from_hidraw_idx(int idx, int *bus, int *addr, int *inf) {
	char buff[256];
	int oid[CTL_MAXNAME];
	size_t oid_size  = CTL_MAXNAME;
	size_t len;
	int udev_idx;

	snprintf(buff, sizeof(buff), "dev.hidbus.%d.%%parent", idx);
	len = sizeof(buff);
	if (sysctlbyname(buff, buff, &len, NULL, 0))
		return 0;
	buff[len] = '\0';

	if (sscanf(buff, "usbhid%2d", &udev_idx) != 1)
		return 0;
	snprintf(buff, sizeof(buff), "dev.usbhid.%d.%%location", udev_idx);
	len = sizeof(buff);
	if (sysctlbyname(buff, buff, &len, NULL, 0))
		return 0;
	buff[len] = '\0';
	return sscanf(buff, "bus=%d hubaddr=%2d port=%2d devaddr=%2d interface=%d", bus, addr, addr, addr, inf) == 4;
}

static void hid_device_handle_bus_dependent(const struct hidraw_device_info *rawinfo, struct hid_device_info *info, int idx) {
	libusb_device_handle *handler;
	libusb_device *device;
	libusb_device_descriptor desc;
	uint8_t buffer[256] = {0};
	wchar_t namebuffer[256] = {0};
	int bus, addr;
	int len = 0;

	switch (rawinfo->hdi_bustype) {
	case BUS_USB:
		hid_get_udev_location_from_hidraw_idx(idx,&bus,
		    &addr,&info->interface_number);
		handler = hid_find_device_handle_by_bus_and_port(bus, addr);
		if (!handler)
			break;
		device = libusb_get_device(handler);
		if (libusb_get_device_descriptor(device, &desc)) {
			libusb_close(handler);
		}
		len = libusb_get_string_descriptor_ascii(handler, desc.iManufacturer, buffer, sizeof(buffer));
		if (len == 0) {
			libusb_close(handler);
			break;
		}
		mbstowcs(namebuffer, (char *)buffer, sizeof(namebuffer));
		info->manufacturer_string = wcsdup(namebuffer);
		info->bus_type = HID_API_BUS_USB;
		libusb_close(handler);
		break;
	case BUS_BLUETOOTH:
		info->bus_type = HID_API_BUS_BLUETOOTH;
		break;
	case BUS_I2C:
		info->bus_type = HID_API_BUS_I2C;
		break;
	case BUS_SPI:
		info->bus_type = HID_API_BUS_SPI;
		break;
	default:
		info->bus_type = HID_API_BUS_UNKNOWN;
		break;
	}
}

static struct hid_device_info *hid_create_device_info_by_hidraw_idx(int fd_prep, int idx)
{
	int error;
	int desc_size;
	int fd;
	struct hidraw_device_info devinfo;
	struct hidraw_report_descriptor desc;
	struct hid_device_info *result = NULL, *template = NULL;
	wchar_t namebuffer[256];
	char devpath[256];

	snprintf(devpath, sizeof(devpath), "/dev/hidraw%d", idx);

	fd = fd_prep == -1 ? open(devpath, O_RDWR | O_CLOEXEC) : fd_prep;
	if (fd == -1) {
		register_global_error_format("open: %s", strerror(errno));
		return NULL;
	}

	error = ioctl(fd, HIDRAW_GET_DEVICEINFO, &devinfo);
	if (error == -1) {
		register_global_error_format("ioctl(HIDRAW_GET_DEVICE_INFO): %s", strerror(errno));
		goto end;
	}

	template = malloc(sizeof(struct hid_device_info));
	template->path = strdup(devpath);
	template->vendor_id = devinfo.hdi_vendor;
	template->product_id = devinfo.hdi_product;
	mbstowcs(namebuffer, devinfo.hdi_uniq, sizeof(namebuffer));
	template->serial_number = strlen(devinfo.hdi_uniq) ? wcsdup(namebuffer) : NULL;
	template->release_number = devinfo.hdi_version;
	mbstowcs(namebuffer, devinfo.hdi_name, sizeof(namebuffer));
	template->product_string = strlen(devinfo.hdi_name) ? wcsdup(namebuffer) : NULL;
	template->interface_number = -1;
	template->manufacturer_string = NULL;
	template->next = NULL;
	hid_device_handle_bus_dependent(&devinfo, template, idx);

	error = ioctl(fd, HIDIOCGRDESCSIZE, &desc_size);
	if (error == -1) {
		register_global_error_format("ioctl(HIDIOCGRDESCSIZE): %s", strerror(errno));
		goto end;
	}
	desc.size = desc_size;
	error = ioctl(fd, HIDIOCGRDESC,  &desc);
	if (error == -1) {
		register_global_error_format("ioctl(HIDIOCGRDESC): %s", strerror(errno));
		goto end;
	}

	unsigned short page = 0, usage = 0;
	struct hid_usage_iterator iter;
	memset(&iter, 0, sizeof(iter));
	struct hid_device_info *cur = template;

	while (!get_next_hid_usage(desc.value, desc.size, &iter, &page, &usage)) {
		cur = cur->next = (struct hid_device_info *) calloc(1, sizeof(struct hid_device_info));
		if (!cur)
			break;
		cur->path = strdup(template->path);
		cur->vendor_id = template->vendor_id;
		cur->product_id = template->product_id;
		cur->serial_number = template->serial_number ? wcsdup(template->serial_number) : NULL;
		cur->release_number = template->release_number;
		cur->product_string = template->product_string ? wcsdup(template->product_string) : NULL;
		cur->interface_number = template->interface_number;
		cur->manufacturer_string = template->manufacturer_string ? wcsdup(template->manufacturer_string) : NULL;
		cur->bus_type = template->bus_type;
		cur->usage = usage;
		cur->usage_page = page;
	}
	/*
	 * make the circular linked list. so that caller can have
	 * the address of first element (cur->next) and last element(cur)
	 */
	cur->next = template->next;
	template->next = NULL;
end:
	if (template)
		hid_free_enumeration(template);
	if (fd_prep == -1)
		close(fd);
	return cur;
}


HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &hid_api_ver;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

int HID_API_EXPORT hid_init(void)
{
	const char *locale;
	int error;

	locale = setlocale(LC_CTYPE, NULL);
	if (!locale)
		setlocale(LC_CTYPE, "");

	if ((error = libusb_init(&global_usb_context)))
		return error;

	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
	register_global_error(NULL);
	libusb_exit(global_usb_context);
	return 0;
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	struct hid_device_info *root = calloc(1, sizeof(struct hid_device_info));
	struct hid_device_info *head = root;
	int oid[CTL_MAXNAME] = {0};
	size_t buf_len, oid_items = CTL_MAXNAME;
	struct hid_sysctl_iter iter;

	if (!hid_init_sysctl_iter(&iter, "dev.hidraw"))
		return NULL;

	/*
	 * we want to exactly match dev.hidraw.%d
	 */
	while(hid_get_next_from_sysctl_iter(&iter)) {
		struct hid_device_info *tmp = hid_create_device_info_by_hidraw_idx(-1, iter.idx);
		if (!tmp)
			continue;
		if ((vendor_id != 0 || product_id != 0) &&
			(vendor_id != tmp->vendor_id && product_id != tmp->product_id)) {
			continue;
		}
		root->next = tmp->next;
		root = tmp;
		tmp->next = NULL;
	}
	root = head->next;
	free(head);
	return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
	struct hid_device_info *next;

	while(devs) {
		next = devs->next;
		free(devs->manufacturer_string);
		free(devs->product_string);
		free(devs->path);
		free(devs->serial_number);
		free(devs);
		devs = next;
	}
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

	dev->device_path = strdup(path);
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
		register_error_str(&dev->read_error_str, "Zero buffer/length");
		return -1;
	}

	/* Set device error to none */
	register_error_str(&dev->read_error_str, NULL);

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
			register_error_str(&dev->read_error_str, strerror(errno));
			return ret;
		}
		else {
			/* Check for errors on the file descriptor. This will
			   indicate a device disconnection. */
			if (fds.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				// We cannot use strerror() here as no -1 was returned from poll().
				errno = EIO;
				register_error_str(&dev->read_error_str, "hid_read_timeout: unexpected poll error (device disconnected)");
				return -1;
			}
		}
	}

	bytes_read = read(dev->device_handle, data, length);
	if (bytes_read < 0) {
		if (errno == EAGAIN || errno == EINPROGRESS)
			bytes_read = 0;
		else
			register_error_str(&dev->read_error_str, strerror(errno));
	}

	return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_read_error(hid_device *dev)
{
	if (dev->read_error_str == NULL)
		return L"Success";
	return dev->read_error_str;
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
#ifndef HIDIOCGFEATURE
	struct hidraw_gen_descriptor desc;
#endif


	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

#ifdef HIDIOCSFEATURE
	res = ioctl(dev->device_handle, HIDIOCSFEATURE(length), data);
	length = res;
#else
	desc.hgd_maxlen = length;
	desc.hgd_data = (void *)data;
	desc.hgd_report_type = HID_FEATURE_REPORT;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDRAW_SET_REPORT, &desc);
#endif

	if (res < 0) {
		register_device_error_format(dev, "ioctl (SFEATURE): %s", strerror(errno));
		return -1;
	}

	return length;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;
#ifndef HIDIOCGFEATURE
	struct hidraw_gen_descriptor desc;
#endif

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

#ifdef HIDIOCGFEATURE
	res = ioctl(dev->device_handle, HIDIOCGFEATURE(length), data);
	length = res;
#else
	desc.hgd_maxlen = length;
	desc.hgd_data = (void *)data;
	desc.hgd_report_type = HID_FEATURE_REPORT;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDRAW_GET_REPORT, &desc);
	length = desc.hgd_actlen;
#endif

	if (res < 0) {
		register_device_error_format(dev, "ioctl (GFEATURE): %s", strerror(errno));
		return -1;
	}

	return length;
}

int HID_API_EXPORT HID_API_CALL hid_send_output_report(hid_device *dev, const unsigned char *data, size_t length)
{
	int res;
#ifndef HIDIOCSOUTPUT
	struct hidraw_gen_descriptor desc;
#endif

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

#ifdef HIDIOCSOUTPUT
	res = ioctl(dev->device_handle, HIDIOCSOUTPUT(length), data);
	length = res;
#else
	desc.hgd_maxlen = length;
	desc.hgd_data = (void *)data;
	desc.hgd_report_type = HID_OUTPUT_REPORT;
	res = ioctl(dev->device_handle, HIDRAW_SET_REPORT, &desc);
#endif

	if (res < 0) {
		register_device_error_format(dev, "ioctl (SOUTPUT): %s", strerror(errno));
		return -1;
	}

	return length;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;
#ifndef HIDIOCGINPUT
	struct hidraw_gen_descriptor desc;
#endif

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

#ifdef HIDIOCGINPUT
	res = ioctl(dev->device_handle, HIDIOCGINPUT(length), data);
	length = res;
#else
	desc.hgd_maxlen = length;
	desc.hgd_data = (void *)data;
	desc.hgd_report_type = HID_INPUT_REPORT;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDRAW_GET_REPORT, &desc);
	length = desc.hgd_actlen;
#endif

	if (res < 0) {
		register_device_error_format(dev, "ioctl (GINPUT): %s", strerror(errno));
		return -1;
	}

	return length;
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	close(dev->device_handle);

	free((void *)dev->device_path);
	free(dev->error_str);
	free(dev->read_error_str);

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
	int idx;
	char path_buffer[1024];

	if (dev->device_info) {
		register_device_error(dev, NULL);
	}
	else {
		// Lazy initialize device_info
		realpath(dev->device_path, path_buffer);
		sscanf(path_buffer, "/dev/hidraw%d", &idx);
		struct hid_device_info *devinfo =
			hid_create_device_info_by_hidraw_idx(dev->device_handle, idx);
		if (devinfo != NULL) {
			dev->device_info = devinfo->next;
			devinfo->next = NULL;
		} else {
			register_device_error(dev, "Failed to create device: maybe the permission is not correct?");
		}
	}

	// hid_create_device_info_by_hidraw_idx will set an error if needed
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
	int error;
	int act_len = 0;
	struct hidraw_report_descriptor desc;

	if (!buf || !buf_size) {
		errno = EINVAL;
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	error = ioctl(dev->device_handle ,HIDIOCGRDESCSIZE, &act_len);
	if (error < 0) {
		register_device_error_format(dev, "hid_get_report_descriptor: unable to do ioctl on %s: %s", dev->device_path, strerror(errno));
		/* error already registered */
		return error;
	}

	desc.size = act_len;
	error = ioctl(dev->device_handle, HIDIOCGRDESC, &desc);
	if (error < 0) {
		register_device_error_format(dev, "hid_get_report_descriptor: unable to do ioctl on %s: %s", dev->device_path, strerror(errno));
		/* error already registered */
		return error;
	}

	if (act_len < buf_size)
		buf_size = act_len;

	memcpy(buf, desc.value, buf_size);
	return buf_size;
}


/* Passing in NULL means asking for the last global error message. */
HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->error_str == NULL)
			return L"Success";
		return dev->error_str;
	}

	if (global_error_str == NULL)
		return L"Success";
	return global_error_str;
}
