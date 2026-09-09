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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* needed for wcsdup() before glibc 2.10 */
#endif

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <errno.h>
#include <time.h>

/* Unix */
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <wchar.h>

/* GNU / LibUSB */
#include <libusb.h>
#if !defined(__ANDROID__) && !defined(NO_ICONV)
#include <iconv.h>
#ifndef ICONV_CONST
#define ICONV_CONST
#endif
#endif

#include "hidapi_libusb.h"

#ifndef HIDAPI_THREAD_MODEL_INCLUDE
#define HIDAPI_THREAD_MODEL_INCLUDE "hidapi_thread_pthread.h"
#endif
#include HIDAPI_THREAD_MODEL_INCLUDE

/* The value of the first callback handle to be given upon registration */
/* Can be any arbitrary positive integer */
#define FIRST_HOTPLUG_CALLBACK_HANDLE 1

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DEBUG_PRINTF
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...) do {} while (0)
#endif

#ifndef __FreeBSD__
#define DETACH_KERNEL_DRIVER
#endif

/* Uncomment to enable the retrieval of Usage and Usage Page in
hid_enumerate(). Warning, on platforms different from FreeBSD
this is very invasive as it requires the detach
and re-attach of the kernel driver. See comments inside hid_enumerate().
libusb HIDAPI programs are encouraged to use the interface number
instead to differentiate between interfaces on a composite HID device. */
/*#define INVASIVE_GET_USAGE*/

/* Linked List of input reports received from the device. */
struct input_report {
	uint8_t *data;
	size_t len;
	struct input_report *next;
};


typedef struct hidapi_error_ctx_ {
	/* libusb error code (negative LIBUSB_ERROR_* values or LIBUSB_SUCCESS),
	 * or a sentinel non-zero value when set via register_string_error().
	 * Stored as plain int so call sites can pass libusb return values
	 * (which are typed as int) without an explicit cast — that keeps
	 * libusb/hid.c compilable as C++ as well as C. */
	int error_code;
	/* designed to hold string literals only - do not free */
	const char *error_context;

	int last_error_code_cache;
	const char *last_error_context_cache;
	/* dynamically allocated */
	wchar_t *last_error_str;
} hidapi_error_ctx;


struct hid_device_ {
	/* Handle to the actual device. */
	libusb_device_handle *device_handle;

	/* USB Configuration Number of the device */
	int config_number;
	/* The interface number of the HID */
	int interface;

	uint16_t report_descriptor_size;

	/* Endpoint information */
	int input_endpoint;
	int output_endpoint;
	int input_ep_max_packet_size;

	/* Indexes of Strings */
	int manufacturer_index;
	int product_index;
	int serial_index;
	struct hid_device_info* device_info;

	/* Whether blocking reads are used */
	int blocking; /* boolean */

	/* Read thread objects */
	hidapi_thread_state thread_state;
	int shutdown_thread;
	int transfer_loop_finished;
	struct libusb_transfer *transfer;

	/* List of received input reports. */
	struct input_report *input_reports;

	/* Was kernel driver detached by libusb */
#ifdef DETACH_KERNEL_DRIVER
	int is_driver_detached;
#endif

	hidapi_error_ctx error;
	wchar_t *last_read_error_str;

	unsigned int write_timeout_ms;
	unsigned int send_output_report_timeout_ms;
	unsigned int send_feature_report_timeout_ms;
};

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

static libusb_context *usb_context = NULL;

static hidapi_error_ctx last_global_error;

/* Serializes mutations of the global error state: hid_hotplug_register_callback()
 * and hid_hotplug_deregister_callback() are thread-safe by contract and their
 * failure paths write last_global_error concurrently (an unserialized
 * register_string_error() would double-free the stored string). */
static pthread_mutex_t hid_global_error_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Identity of the hotplug callback thread - the one HIDAPI-owned thread that can
 * reach the public API, as it is the thread the callbacks run on.
 *
 * The global error state belongs to the application: hid_error(NULL) frees and
 * replaces the cached string, and an application cannot serialize its own calls
 * against a thread it does not know about. Registering or deregistering a
 * callback from within a callback is explicitly allowed (see hidapi.h), and both
 * write the global error state on failure, for example when deregistering an
 * already removed handle. Those writes are therefore DROPPED on this thread
 * (see register_libusb_error()/register_string_error()):
 * a call that fails there still reports the failure through its return value,
 * only the error string of the process is left alone.
 *
 * Guarded by hid_global_error_mutex, which is a leaf lock (never held while
 * another one is acquired), so this adds no lock-order edge. Only one callback
 * thread exists at a time (see threads_running). */
static pthread_t hid_callback_thread_id;
static unsigned char hid_callback_thread_id_valid;

/* Called by the callback thread as its first and last action */
static void hid_internal_callback_thread_enter(void)
{
	pthread_mutex_lock(&hid_global_error_mutex);
	hid_callback_thread_id = pthread_self();
	hid_callback_thread_id_valid = 1;
	pthread_mutex_unlock(&hid_global_error_mutex);
}

static void hid_internal_callback_thread_leave(void)
{
	pthread_mutex_lock(&hid_global_error_mutex);
	hid_callback_thread_id_valid = 0;
	pthread_mutex_unlock(&hid_global_error_mutex);
}

/* Called with hid_global_error_mutex held */
static int hid_internal_on_callback_thread(void)
{
	return hid_callback_thread_id_valid && pthread_equal(pthread_self(), hid_callback_thread_id);
}

struct hid_hotplug_queue {
	/* The device this message is about; NULL marks a request to flush
	 * the pending HID_API_HOTPLUG_ENUMERATE snapshots (a "replay marker") */
	libusb_device* device;
	int event; /* Arrived or removed; unused for replay markers */
	struct hid_hotplug_queue* next;
};

struct hid_hotplug_device {
	/* NULL after a reconciled removal while a snapshot arrival is pending. */
	struct hid_device_info *info;
	libusb_device *device; /* Referenced identity of this connection */
	/* A snapshot entry may still have an unprocessed live arrival. */
	unsigned char arrival_seen;
	/* Nonzero once reconciliation finds it gone: hide it from snapshots and
	 * deliver its removal only to handles below this registration boundary. */
	hid_hotplug_callback_handle removed_before;
	struct hid_hotplug_device *next;
};

static struct hid_hotplug_context {
	/* A separate libusb context for hotplug events: helps avoid mutual blocking with read_thread's */
	libusb_context * context;

	/* libusb callback handle */
	libusb_hotplug_callback_handle callback_handle;

	/* HIDAPI unique callback handle counter */
	hid_hotplug_callback_handle next_handle;

	/* A thread that fills the event queue */
	hidapi_thread_state libusb_thread;

	/* A separate thread which processes hidapi's internal event queue.
	 * Its condition parks the callback thread while !shutdown_pending and
	 * shutdown waiters while shutdown_pending; flag transitions broadcast. */
	hidapi_thread_state callback_thread;

	/* This mutex prevents changes to the callback list */
	pthread_mutex_t mutex;

	/* Boolean flags */
	/* `mutex` (and the thread states) have been initialized. A one-way latch:
	 * written once under hid_hotplug_init_mutex and read under it as well; the
	 * objects it advertises are never destroyed, so once it is set, locking
	 * `mutex` is safe forever. */
	unsigned char mutex_ready;
	unsigned char mutex_in_use;
	unsigned char cb_list_dirty;
	/* The event threads have been started and their join not yet claimed */
	unsigned char threads_running;
	/* A thread has claimed the join of the event threads (threads_running is
	 * already cleared) but has not completed it yet. It releases `mutex` while
	 * joining, so in that window the event threads may still be running - and
	 * still draining messages into `devs`, which nobody else may free until the
	 * join is through (see hid_internal_hotplug_finish_shutdown). */
	unsigned char join_claimed;
	/* Tells the event threads to wind down; set when the last callback is
	 * removed and cleared once they have been joined. Always written under BOTH
	 * `mutex` and callback_thread's mutex (see
	 * hid_internal_hotplug_set_shutdown_pending), so a reader may hold either.
	 * The join is performed synchronously on the initiating application thread;
	 * when the shutdown is initiated from the callback (event) thread itself,
	 * which cannot join itself, it is deferred to the next registration or
	 * hid_exit(). */
	unsigned char shutdown_pending;
	/* Completed wind-downs; protected by callback_thread's mutex */
	unsigned long shutdown_generation;

	/* Pending messages for the callback thread; protected by callback_thread's mutex */
	struct hid_hotplug_queue* queue;
	/* A removal was dropped; protected by callback_thread's mutex. Cleared
	 * before reconciliation, restored on failure or another dropped removal. */
	unsigned char cache_stale;

	/* Linked list of the hotplug callbacks */
	struct hid_hotplug_callback *hotplug_cbs;

	/* Linked list of the devices and their infos (needed after disconnection).
	 * Protected by `mutex`: all reads, writes and the final free during teardown
	 * are performed while holding it. Freed by
	 * hotplug_thread() after joining the callback thread,
	 * hid_internal_hotplug_cleanup()/hid_internal_hotplug_exit() when the threads
	 * never ran or were already joined (and no join is claimed), or
	 * hid_internal_hotplug_unwind_registration() on a failed first registration.
	 * Device references are released before their libusb context is destroyed. */
	struct hid_hotplug_device *devs;
} hid_hotplug_context; /* zero-initialized (static storage); next_handle set on first init */

uint16_t get_usb_code_for_current_locale(void);
static int return_data(hid_device *dev, unsigned char *data, size_t length);

static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (!dev)
		return NULL;

	dev->blocking = 1;
	dev->write_timeout_ms = 1000;
	dev->send_output_report_timeout_ms = 1000;
	dev->send_feature_report_timeout_ms = 1000;

	hidapi_thread_state_init(&dev->thread_state);

	return dev;
}

static void free_hidapi_error(hidapi_error_ctx *err)
{
	free(err->last_error_str);
}

static void free_hid_device(hid_device *dev)
{
	/* Clean up the thread objects */
	hidapi_thread_state_destroy(&dev->thread_state);

	hid_free_enumeration(dev->device_info);
	free_hidapi_error(&dev->error);
	free(dev->last_read_error_str);

	/* Free the device itself */
	free(dev);
}

/* Get bytes from a HID Report Descriptor.
   Only call with a num_bytes of 0, 1, 2, or 4. */
static uint32_t get_bytes(uint8_t *rpt, size_t len, size_t num_bytes, size_t cur)
{
	/* Return if there aren't enough bytes. */
	if (cur + num_bytes >= len)
		return 0;

	if (num_bytes == 0)
		return 0;
	else if (num_bytes == 1) {
		return rpt[cur+1];
	}
	else if (num_bytes == 2) {
		return (rpt[cur+2] * 256 + rpt[cur+1]);
	}
	else if (num_bytes == 4) {
		return (rpt[cur+4] * 0x01000000 +
		        rpt[cur+3] * 0x00010000 +
		        rpt[cur+2] * 0x00000100 +
		        rpt[cur+1] * 0x00000001);
	}
	else
		return 0;
}

/* Retrieves the device's Usage Page and Usage from the report
   descriptor. The algorithm is simple, as it just returns the first
   Usage and Usage Page that it finds in the descriptor.
   The return value is 0 on success and -1 on failure. */
static int get_usage(uint8_t *report_descriptor, size_t size,
                     unsigned short *usage_page, unsigned short *usage)
{
	unsigned int i = 0;
	int size_code;
	int data_len, key_size;
	int usage_found = 0, usage_page_found = 0;

	while (i < size) {
		int key = report_descriptor[i];
		int key_cmd = key & 0xfc;

		//printf("key: %02hhx\n", key);

		if ((key & 0xf0) == 0xf0) {
			/* This is a Long Item. The next byte contains the
			   length of the data section (value) for this key.
			   See the HID specification, version 1.11, section
			   6.2.2.3, titled "Long Items." */
			if (i+1 < size)
				data_len = report_descriptor[i+1];
			else
				data_len = 0; /* malformed report */
			key_size = 3;
		}
		else {
			/* This is a Short Item. The bottom two bits of the
			   key contain the size code for the data section
			   (value) for this key.  Refer to the HID
			   specification, version 1.11, section 6.2.2.2,
			   titled "Short Items." */
			size_code = key & 0x3;
			switch (size_code) {
			case 0:
			case 1:
			case 2:
				data_len = size_code;
				break;
			case 3:
				data_len = 4;
				break;
			default:
				/* Can't ever happen since size_code is & 0x3 */
				data_len = 0;
				break;
			};
			key_size = 1;
		}

		if (key_cmd == 0x4) {
			*usage_page  = get_bytes(report_descriptor, size, data_len, i);
			usage_page_found = 1;
			//printf("Usage Page: %x\n", (uint32_t)*usage_page);
		}
		if (key_cmd == 0x8) {
			if (data_len == 4) { /* Usages 5.5 / Usage Page 6.2.2.7 */
				*usage_page = get_bytes(report_descriptor, size, 2, i + 2);
				usage_page_found = 1;
				*usage = get_bytes(report_descriptor, size, 2, i);
				usage_found = 1;
			}
			else {
				*usage = get_bytes(report_descriptor, size, data_len, i);
				usage_found = 1;
			}
			//printf("Usage: %x\n", (uint32_t)*usage);
		}

		if (usage_page_found && usage_found)
			return 0; /* success */

		/* Skip over this key and it's associated data */
		i += data_len + key_size;
	}

	return -1; /* failure */
}

#if defined(__FreeBSD__) && __FreeBSD__ < 10
/* The libusb version included in FreeBSD < 10 doesn't have this function. In
   mainline libusb, it's inlined in libusb.h. This function will bear a striking
   resemblance to that one, because there's about one way to code it.

   Note that the data parameter is Unicode in UTF-16LE encoding.
   Return value is the number of bytes in data, or LIBUSB_ERROR_*.
 */
static inline int libusb_get_string_descriptor(libusb_device_handle *dev,
	uint8_t descriptor_index, uint16_t lang_id,
	unsigned char *data, int length)
{
	return libusb_control_transfer(dev,
		LIBUSB_ENDPOINT_IN | 0x0, /* Endpoint 0 IN */
		LIBUSB_REQUEST_GET_DESCRIPTOR,
		(LIBUSB_DT_STRING << 8) | descriptor_index,
		lang_id, data, (uint16_t) length, 1000);
}

#endif


static wchar_t *ctowcdup(const char *s, size_t slen)
{
	size_t i;

	size_t wchar_len = slen;
	wchar_t *wbuf = (wchar_t*) malloc((wchar_len + 1) * sizeof(wchar_t));

	if (!wbuf)
		return NULL;

	for (i = 0u; i < wchar_len; i++) {
		wbuf[i] = s[i];
	}

	wbuf[wchar_len] = L'\0';

	return wbuf;
}


static void register_libusb_error(hidapi_error_ctx *err, int error, const char *error_context)
{
	int is_global = (err == &last_global_error);

	if (is_global) {
		pthread_mutex_lock(&hid_global_error_mutex);
		if (hid_internal_on_callback_thread()) {
			/* Dropped: never write the global error state from HIDAPI's own
			 * thread (see hid_callback_thread_id) */
			pthread_mutex_unlock(&hid_global_error_mutex);
			return;
		}
	}

	err->error_code = error;
	err->error_context = error_context;

	if (is_global)
		pthread_mutex_unlock(&hid_global_error_mutex);
}


static void register_string_error(hidapi_error_ctx *err, const char *error)
{
	int is_global = (err == &last_global_error);

	if (is_global) {
		pthread_mutex_lock(&hid_global_error_mutex);
		if (hid_internal_on_callback_thread()) {
			/* Dropped: never write the global error state from HIDAPI's own
			 * thread (see hid_callback_thread_id) */
			pthread_mutex_unlock(&hid_global_error_mutex);
			return;
		}
	}

	free(err->last_error_str);

	err->last_error_str = ctowcdup(error, strlen(error));
	err->error_code = err->last_error_code_cache = 1;
	err->error_context = err->last_error_context_cache = NULL;

	if (is_global)
		pthread_mutex_unlock(&hid_global_error_mutex);
}


static void register_read_error(hid_device *dev, const char *error)
{
	free(dev->last_read_error_str);
	dev->last_read_error_str = error ? ctowcdup(error, strlen(error)) : NULL;
}


/* we don't use iconv on Android, or when it is explicitly disabled */
#if !defined(__ANDROID__) && !defined(NO_ICONV)

/* iconv implementations */

static wchar_t *utf_to_wchar(char *utf, size_t utfbytes, const char *fromcode, size_t max_expected_wchar)
{
	iconv_t ic;
	size_t inbytes;
	size_t outbytes;
	size_t res;
	ICONV_CONST char *inptr;
	char *outptr;
	wchar_t *wbuf = NULL;
	size_t wbuf_size;

	/* Initialize iconv. */
	ic = iconv_open("WCHAR_T", fromcode);
	if (ic == (iconv_t)-1) {
		LOG("iconv_open() failed\n");
		return NULL;
	}

	wbuf_size = (max_expected_wchar + 1) * sizeof(wchar_t);
	wbuf = (wchar_t *) malloc(wbuf_size);
	if (!wbuf) {
		goto end;
	}

	inptr = utf;
	inbytes = utfbytes;
	outptr = (char*) wbuf;
	outbytes = wbuf_size;
	res = iconv(ic, &inptr, &inbytes, &outptr, &outbytes);
	if (res == (size_t)-1) {
		LOG("iconv() failed\n");
		goto err;
	}

	/* Ensure the terminating NULL. */
	wbuf[max_expected_wchar] = L'\0';
	if (outbytes >= sizeof(wbuf[0]))
		*((wchar_t*)outptr) = L'\0';

	goto end;

err:
	free(wbuf);
	wbuf = NULL;

end:
	iconv_close(ic);

	return wbuf;
}


static wchar_t *utf16le_to_wchar(char *utf16, size_t utf16bytes)
{
	return utf_to_wchar(utf16, utf16bytes, "UTF-16LE", utf16bytes / 2);
}


static wchar_t *utf8_to_wchar(char *utf8, size_t utf8bytes)
{
	return utf_to_wchar(utf8, utf8bytes, "UTF-8", utf8bytes);
}


#else

/* simple implementations */


static wchar_t *utf16le_to_wchar(char *utf16, size_t utf16bytes)
{
	/* The following code will only work for
	   code points that can be represented as a single UTF-16 character,
	   and will incorrectly convert any code points which require more
	   than one UTF-16 character. */

	size_t i;

	size_t wchar_len = utf16bytes / 2;
	wchar_t *wbuf = (wchar_t*) malloc((wchar_len + 1) * sizeof(wchar_t));

	if (!wbuf)
		return NULL;

	for (i = 0u; i < wchar_len; i++) {
		wbuf[i] = utf16[i * 2] | (utf16[i * 2 + 1] << 8);
	}

	wbuf[wchar_len] = L'\0';

	return wbuf;
}


static wchar_t *utf8_to_wchar(char *utf8, size_t utf8bytes)
{
	/* The will only work for
	   code points that can be represented as a single UTF-8 character,
	   and will incorrectly convert any code points which require more
	   than one UTF-8 character. */

	return ctowcdup(utf8, utf8bytes);
}


#endif


/* Get the first language the device says it reports. This comes from
   USB string #0. */
static uint16_t get_first_language(libusb_device_handle *dev)
{
	uint16_t buf[32];
	int len;

	/* Get the string from libusb. */
	len = libusb_get_string_descriptor(dev,
			0x0, /* String ID */
			0x0, /* Language */
			(unsigned char*)buf,
			sizeof(buf));
	if (len < 4)
		return 0x0;

	return buf[1]; /* First two bytes are len and descriptor type. */
}

static int is_language_supported(libusb_device_handle *dev, uint16_t lang)
{
	uint16_t buf[32];
	int len;
	int i;

	/* Get the string from libusb. */
	len = libusb_get_string_descriptor(dev,
			0x0, /* String ID */
			0x0, /* Language */
			(unsigned char*)buf,
			sizeof(buf));
	if (len < 4)
		return 0x0;


	len /= 2; /* language IDs are two-bytes each. */
	/* Start at index 1 because there are two bytes of protocol data. */
	for (i = 1; i < len; i++) {
		if (buf[i] == lang)
			return 1;
	}

	return 0;
}


/* This function returns a newly allocated wide string containing the USB
   device string numbered by the index. The returned string must be freed
   by using free(). */
static wchar_t *get_usb_string(libusb_device_handle *dev, uint8_t idx, int *res)
{
	char buf[512];
	int len;

	/* Determine which language to use. */
	uint16_t lang;
	lang = get_usb_code_for_current_locale();
	if (!is_language_supported(dev, lang))
		lang = get_first_language(dev);

	/* Get the string from libusb. */
	len = libusb_get_string_descriptor(dev,
			idx,
			lang,
			(unsigned char*)buf,
			sizeof(buf));
	*res = len;

	if (len < 2) /* we always skip first 2 bytes */
		return NULL;

	return utf16le_to_wchar(buf + 2, (size_t)(len - 2));
}

/**
  Max length of the result: "000-000.000.000.000.000.000.000:000.000" (39 chars).
  64 is used for simplicity/alignment.
*/
static void get_path(char (*result)[64], libusb_device *dev, int config_number, int interface_number)
{
	char *str = *result;

	/* Note that USB3 port count limit is 7; use 8 here for alignment */
	uint8_t port_numbers[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int num_ports = libusb_get_port_numbers(dev, port_numbers, 8);

	if (num_ports > 0) {
		int n = snprintf(str, sizeof("000-000"), "%u-%u", libusb_get_bus_number(dev), port_numbers[0]);
		for (uint8_t i = 1; i < num_ports; i++) {
			n += snprintf(&str[n], sizeof(".000"), ".%u", port_numbers[i]);
		}
		n += snprintf(&str[n], sizeof(":000.000"), ":%u.%u", (uint8_t)config_number, (uint8_t)interface_number);
		str[n] = '\0';
	} else {
		/* Likely impossible, but check: USB3.0 specs limit number of ports to 7 and buffer size here is 8 */
		if (num_ports == LIBUSB_ERROR_OVERFLOW) {
			LOG("make_path() failed. buffer overflow error\n");
		} else {
			LOG("make_path() failed. unknown error\n");
		}
		str[0] = '\0';
	}
}

static char *make_path(libusb_device *dev, int config_number, int interface_number)
{
	char str[64];
	get_path(&str, dev, config_number, interface_number);
	return strdup(str);
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

struct hid_hotplug_callback
{
	unsigned short vendor_id;
	unsigned short product_id;
	hid_hotplug_callback_fn callback;
	void* user_data;
	int events;
	/* Deep-copied registration-time snapshot of the matching connected devices
	 * (HID_API_HOTPLUG_ENUMERATE): delivered asynchronously on the event thread
	 * as synthetic arrival events, always before any live event for this
	 * callback. Protected by hid_hotplug_context.mutex. */
	struct hid_device_info* replay;
	struct hid_hotplug_callback* next;

	hid_hotplug_callback_handle handle;
};

static void hid_internal_hotplug_free_devices(struct hid_hotplug_device *devs)
{
	while (devs) {
		struct hid_hotplug_device *next = devs->next;
		hid_free_enumeration(devs->info);
		libusb_unref_device(devs->device);
		free(devs);
		devs = next;
	}
}

/* Takes ownership of info on success only */
static struct hid_hotplug_device *hid_internal_hotplug_create_device(libusb_device *device, struct hid_device_info *info)
{
	struct hid_hotplug_device *dev = (struct hid_hotplug_device *) calloc(1, sizeof(struct hid_hotplug_device));
	if (dev) {
		dev->info = info;
		dev->device = libusb_ref_device(device);
	}
	return dev;
}

/* Sets shutdown_pending and wakes everyone who may be waiting for the change:
 * the event threads, and any thread in hid_internal_hotplug_wait_shutdown().
 * Called with `mutex` held, so every write to shutdown_pending is made under
 * BOTH `mutex` and the callback thread's mutex - a reader may therefore hold
 * either one of them. */
static void hid_internal_hotplug_set_shutdown_pending(unsigned char value)
{
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	hid_hotplug_context.shutdown_pending = value;
	if (!value) {
		hid_hotplug_context.shutdown_generation++;
	}
	hidapi_thread_cond_broadcast(&hid_hotplug_context.callback_thread);
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
}

static void hid_internal_hotplug_remove_postponed(void)
{
	/* Unregister the callbacks whose removal was postponed */
	/* This function is always called with `mutex` held, which implies the
	 * machinery is initialized: locking it is the only way to get here */
	/* However, any actions are only allowed if the mutex is NOT in use */
	if (hid_hotplug_context.mutex_in_use) {
		return;
	}

	if (hid_hotplug_context.cb_list_dirty) {
		/* Traverse the list of callbacks and check if any were marked for removal */
		struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
		while (*current) {
			struct hid_hotplug_callback *callback = *current;
			if (!callback->events) {
				*current = callback->next;
				/* An undelivered ENUMERATE snapshot dies with its callback */
				hid_free_enumeration(callback->replay);
				free(callback);
				continue;
			}
			current = &callback->next;
		}

		/* Clear the flag so we don't start the cycle unless necessary */
		hid_hotplug_context.cb_list_dirty = 0;
	}

	if (hid_hotplug_context.hotplug_cbs == NULL && hid_hotplug_context.threads_running && !hid_hotplug_context.shutdown_pending) {
		/* The last callback is gone: ask the event threads to wind down. The
		 * caller joins them (hid_internal_hotplug_cleanup_sync()), unless this
		 * runs on the event thread itself - it cannot join itself, so the join
		 * is then deferred to the next registration or hid_exit(). */
		hid_internal_hotplug_set_shutdown_pending(1);
	}
}

static void hid_internal_hotplug_cleanup(void)
{
	/* Called with `mutex` held, which implies the machinery is initialized */
	if (hid_hotplug_context.mutex_in_use) {
		return;
	}

	/* Before checking if the list is empty, clear any entries whose removal was
	 * postponed first; this also winds the event threads down (shutdown_pending)
	 * once the list becomes empty */
	hid_internal_hotplug_remove_postponed();

	if (hid_hotplug_context.hotplug_cbs != NULL) {
		return;
	}

	if (!hid_hotplug_context.threads_running && !hid_hotplug_context.join_claimed) {
		/* The event threads either never ran or have already been joined: we
		 * have exclusive access to `devs` (the caller holds `mutex`).
		 * A merely CLAIMED join does not qualify: threads_running is already
		 * cleared, but the threads are still running and may be draining
		 * messages into `devs` - the pump frees it once the drainer is gone. */
		hid_internal_hotplug_free_devices(hid_hotplug_context.devs);
		hid_hotplug_context.devs = NULL;
	}
	/* When the threads are still winding down, the pump frees `devs` and
	 * hid_internal_hotplug_finish_shutdown() joins it (releasing `mutex`
	 * while joining): joining here could deadlock, as the callback
	 * thread locks `mutex` to drain its queue while the caller of this
	 * function is holding it. */
}

/* Completes the wind-down of the event threads (a claimed join): joins them
 * and clears/broadcasts shutdown_pending for any other
 * thread waiting for the wind-down to finish.
 * Called with `mutex` held (recursion level 1) and shutdown_pending set;
 * temporarily releases `mutex` while joining so the callback thread can drain
 * its queue (process_hotplug_event locks it); on return `mutex` is held again.
 * Must not run on the event thread itself. */
static void hid_internal_hotplug_finish_shutdown(void)
{
	/* Claim the join: other threads now see threads_running == 0 and wait for
	 * shutdown_pending to be cleared instead of joining a second time.
	 * join_claimed keeps them from mistaking the cleared threads_running for
	 * "the threads are gone" while `mutex` is released below - they are not, and
	 * `devs` stays theirs until the join is through. */
	hid_hotplug_context.threads_running = 0;
	hid_hotplug_context.join_claimed = 1;
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
	/* The libusb thread joins the callback thread on its way out */
	hidapi_thread_join(&hid_hotplug_context.libusb_thread);
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	hid_hotplug_context.join_claimed = 0;

	/* Announce the completed wind-down to hid_internal_hotplug_wait_shutdown() */
	hid_internal_hotplug_set_shutdown_pending(0);
}

/* Waits out a wind-down that another thread has already claimed (threads_running
 * cleared while shutdown_pending is still set): a busy `unlock`/`lock` spin would
 * be unfair and can livelock under a real-time scheduling policy, so we block on
 * the callback thread's condition, which the joiner broadcasts once it is done.
 * Called with `mutex` held (recursion level 1); releases and re-acquires it while
 * waiting. Must not run on the event thread itself. */
static void hid_internal_hotplug_wait_shutdown(void)
{
	unsigned long generation;

	if (!hid_hotplug_context.shutdown_pending) {
		return;
	}

	/* Acquire the mutex that guards shutdown_pending (and its condition) BEFORE
	 * releasing `mutex`, so the joiner cannot complete in between and leave us
	 * waiting for a broadcast that has already happened */
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	generation = hid_hotplug_context.shutdown_generation;
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
	while (hid_hotplug_context.shutdown_pending && hid_hotplug_context.shutdown_generation == generation) {
		hidapi_thread_cond_wait(&hid_hotplug_context.callback_thread);
	}
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
	pthread_mutex_lock(&hid_hotplug_context.mutex);
}

/* Called with `mutex` held. With no callbacks left, the caller must be off
 * the event thread at recursion level 1.
 * Both join and wait release it: re-check which generation needs settling. */
static void hid_internal_hotplug_settle_shutdown(void)
{
	while (hid_hotplug_context.hotplug_cbs == NULL && hid_hotplug_context.shutdown_pending) {
		if (hid_hotplug_context.threads_running) {
			hid_internal_hotplug_finish_shutdown();
		}
		else {
			hid_internal_hotplug_wait_shutdown();
		}
	}
}

/* Runs the postponed-removal cleanup and, unless this is the event thread
 * dispatching (mutex_in_use), completes the wind-down of the event threads
 * synchronously once the last callback is gone: when this returns on an
 * application thread, the machinery is fully stopped, no callback can be
 * invoked anymore and the library may be safely unloaded.
 * The event thread cannot join itself, so a shutdown initiated from within a
 * callback stays deferred to the next registration or hid_exit().
 * Called with `mutex` held (recursion level 1). */
static void hid_internal_hotplug_cleanup_sync(void)
{
	hid_internal_hotplug_cleanup();

	if (hid_hotplug_context.mutex_in_use || hid_hotplug_context.hotplug_cbs != NULL) {
		return;
	}

	hid_internal_hotplug_settle_shutdown();
}

/* Serializes the one-time initialization of `mutex` and publishes the
 * mutex_ready flag that advertises it: mutex_ready is read under this mutex, so
 * no reader observes it without a happens-before relation to the initialization
 * it advertises (a plain read would be a data race, and C11 atomics are not
 * available on the C99 baseline).
 *
 * This mutex is a leaf: it is never held while another one is acquired. Holding
 * it across the acquisition of `mutex` would deadlock, as the event thread takes
 * them in the opposite order whenever a callback registers another callback
 * (it already holds `mutex` and then needs this one). */
static pthread_mutex_t hid_hotplug_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Initializes the hotplug machinery if needed, then locks `mutex`.
 * Once initialized, `mutex` and the thread states live for the rest of the
 * process: hid_internal_hotplug_exit() winds the machinery down but does NOT
 * destroy them (mutex_ready is a one-way latch). Destroying `mutex` would race
 * every thread that is about to lock it - it cannot be done safely without a
 * lock, and taking one around it is what the lock-order note above rules out. */
static void hid_internal_hotplug_init_and_lock(void)
{
	pthread_mutex_lock(&hid_hotplug_init_mutex);
	if (!hid_hotplug_context.mutex_ready) {
		hidapi_thread_state_init(&hid_hotplug_context.libusb_thread);
		hidapi_thread_state_init(&hid_hotplug_context.callback_thread);

		/* Initialize the mutex as recursive */
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&hid_hotplug_context.mutex, &attr);
		pthread_mutexattr_destroy(&attr);

		/* Set state to Ready */
		hid_hotplug_context.mutex_in_use = 0;
		hid_hotplug_context.cb_list_dirty = 0;
		hid_hotplug_context.threads_running = 0;
		hid_hotplug_context.join_claimed = 0;
		hid_hotplug_context.shutdown_pending = 0;
		if (hid_hotplug_context.next_handle < FIRST_HOTPLUG_CALLBACK_HANDLE)
			hid_hotplug_context.next_handle = FIRST_HOTPLUG_CALLBACK_HANDLE;

		hid_hotplug_context.mutex_ready = 1;
	}
	pthread_mutex_unlock(&hid_hotplug_init_mutex);

	pthread_mutex_lock(&hid_hotplug_context.mutex);
}

/* Locks `mutex` if the hotplug machinery has ever been initialized; returns -1
 * (without locking anything) if it has not */
static int hid_internal_hotplug_lock(void)
{
	unsigned char ready;

	pthread_mutex_lock(&hid_hotplug_init_mutex);
	ready = hid_hotplug_context.mutex_ready;
	pthread_mutex_unlock(&hid_hotplug_init_mutex);

	if (!ready) {
		return -1;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);
	return 0;
}

static void hid_internal_hotplug_exit(void)
{
	/* Initialize the machinery if it never was, instead of taking a lock-free
	 * shortcut for that case: the common path below then handles it naturally
	 * (empty callback list, no threads to wind down). The application must
	 * serialize hid_exit() against registration, as it does against hid_init(). */
	hid_internal_hotplug_init_and_lock();

	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	/* Remove all callbacks from the list (undelivered ENUMERATE snapshots die with them) */
	while (*current) {
		struct hid_hotplug_callback* next = (*current)->next;
		hid_free_enumeration((*current)->replay);
		free(*current);
		*current = next;
	}
	hid_hotplug_context.cb_list_dirty = 0;

	if (hid_hotplug_context.threads_running) {
		/* Request the wind-down */
		hid_internal_hotplug_set_shutdown_pending(1);
	}
	hid_internal_hotplug_settle_shutdown();
	hid_internal_hotplug_free_devices(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;

	/* The event threads are gone. Destroy the main context under `mutex` too;
	 * libusb_exit() does not re-enter HIDAPI. */
	if (usb_context) {
		libusb_exit(usb_context);
		usb_context = NULL;
	}

	/* `mutex` and the thread states are deliberately NOT destroyed: they are
	 * reused by the next registration (see hid_internal_hotplug_init_and_lock) */
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

int HID_API_EXPORT hid_init(void)
{
	register_libusb_error(&last_global_error, LIBUSB_SUCCESS, NULL);

	if (!usb_context) {
		const char *locale;

		/* Init Libusb */
		int res = libusb_init(&usb_context);
		if (res) {
			register_libusb_error(&last_global_error, res, "libusb_init");
			return -1;
		}

		/* Set the locale if it's not set. */
		locale = setlocale(LC_CTYPE, NULL);
		if (!locale)
			setlocale(LC_CTYPE, "");
	}

	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
	/* Stop the hotplug machinery before destroying the main usb_context.
	 * The application must serialize this call against registration and must
	 * never make it from a hotplug callback (see hidapi.h). */
	hid_internal_hotplug_exit();

	/* Free global error state */
	pthread_mutex_lock(&hid_global_error_mutex);
	free_hidapi_error(&last_global_error);
	memset(&last_global_error, 0, sizeof(last_global_error));
	pthread_mutex_unlock(&hid_global_error_mutex);

	return 0;
}

static int hid_internal_match_device_id(unsigned short vendor_id, unsigned short product_id, unsigned short expected_vendor_id, unsigned short expected_product_id)
{
	return (expected_vendor_id == 0x0 || vendor_id == expected_vendor_id) && (expected_product_id == 0x0 || product_id == expected_product_id);
}

static int hid_get_report_descriptor_libusb(libusb_device_handle *handle, int interface_num, uint16_t expected_report_descriptor_size, unsigned char *buf, size_t buf_size)
{
	unsigned char tmp[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];

	if (expected_report_descriptor_size > HID_API_MAX_REPORT_DESCRIPTOR_SIZE)
		expected_report_descriptor_size = HID_API_MAX_REPORT_DESCRIPTOR_SIZE;

	/* Get the HID Report Descriptor.
	   See USB HID Specification, section 7.1.1
	*/
	int res = libusb_control_transfer(handle, LIBUSB_ENDPOINT_IN|LIBUSB_RECIPIENT_INTERFACE, LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_REPORT << 8), interface_num, tmp, expected_report_descriptor_size, 5000);
	if (res < 0) {
		LOG("libusb_control_transfer() for getting the HID Report descriptor failed with %d: %s\n", res, libusb_error_name(res));
		return res;
	}

	if (res > (int)buf_size)
		res = (int)buf_size;

	memcpy(buf, tmp, (size_t)res);
	return res;
}

/**
 * Requires an opened device with *claimed interface*.
 */
static void fill_device_info_usage(struct hid_device_info *cur_dev, libusb_device_handle *handle, int interface_num, uint16_t expected_report_descriptor_size)
{
	unsigned char hid_report_descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
	unsigned short page = 0, usage = 0;

	int res = hid_get_report_descriptor_libusb(handle, interface_num, expected_report_descriptor_size, hid_report_descriptor, sizeof(hid_report_descriptor));
	if (res >= 0) {
		/* Parse the usage and usage page
		   out of the report descriptor. */
		get_usage(hid_report_descriptor, res,  &page, &usage);
	}

	cur_dev->usage_page = page;
	cur_dev->usage = usage;
}

#ifdef INVASIVE_GET_USAGE
static void invasive_fill_device_info_usage(struct hid_device_info *cur_dev, libusb_device_handle *handle, int interface_num, uint16_t report_descriptor_size)
{
	int res = 0;

#ifdef DETACH_KERNEL_DRIVER
	int detached = 0;
	/* Usage Page and Usage */
	res = libusb_kernel_driver_active(handle, interface_num);
	if (res == 1) {
		res = libusb_detach_kernel_driver(handle, interface_num);
		if (res < 0)
			LOG("Couldn't detach kernel driver, even though a kernel driver was attached.\n");
		else
			detached = 1;
	}
#endif

	res = libusb_claim_interface(handle, interface_num);
	if (res >= 0) {
		fill_device_info_usage(cur_dev, handle, interface_num, report_descriptor_size);

		/* Release the interface */
		res = libusb_release_interface(handle, interface_num);
		if (res < 0)
			LOG("Can't release the interface.\n");
	}
	else
		LOG("Can't claim interface: (%d) %s\n", res, libusb_error_name(res));

#ifdef DETACH_KERNEL_DRIVER
	/* Re-attach kernel driver if necessary. */
	if (detached) {
		res = libusb_attach_kernel_driver(handle, interface_num);
		if (res < 0)
			LOG("Couldn't re-attach kernel driver.\n");
	}
#endif
}
#endif /* INVASIVE_GET_USAGE */

/**
 * Create and fill up most of hid_device_info fields.
 * usage_page/usage is not filled up.
 */
static struct hid_device_info * create_device_info_for_device(libusb_device *device, libusb_device_handle *handle, struct libusb_device_descriptor *desc, int config_number, int interface_num)
{
	int res = 0;
	struct hid_device_info *cur_dev = (struct hid_device_info *) calloc(1, sizeof(struct hid_device_info));
	if (cur_dev == NULL) {
		return NULL;
	}

	/* VID/PID */
	cur_dev->vendor_id = desc->idVendor;
	cur_dev->product_id = desc->idProduct;

	cur_dev->release_number = desc->bcdDevice;

	cur_dev->interface_number = interface_num;

	cur_dev->bus_type = HID_API_BUS_USB;

	cur_dev->path = make_path(device, config_number, interface_num);

	if (!handle) {
		return cur_dev;
	}

	if (desc->iSerialNumber > 0)
		cur_dev->serial_number = get_usb_string(handle, desc->iSerialNumber, &res);

	/* Manufacturer and Product strings */
	if (desc->iManufacturer > 0)
		cur_dev->manufacturer_string = get_usb_string(handle, desc->iManufacturer, &res);
	if (desc->iProduct > 0)
		cur_dev->product_string = get_usb_string(handle, desc->iProduct, &res);

	return cur_dev;
}

static uint16_t get_report_descriptor_size_from_interface_descriptors(const struct libusb_interface_descriptor *intf_desc)
{
	int i = 0;
	int found_hid_report_descriptor = 0;
	uint16_t result = HID_API_MAX_REPORT_DESCRIPTOR_SIZE;
	const unsigned char *extra = intf_desc->extra;
	int extra_length = intf_desc->extra_length;

	/*
	 "extra" contains a HID descriptor
	 See section 6.2.1 of HID 1.1 specification.
	*/

	while (extra_length >= 2) { /* Descriptor header: bLength/bDescriptorType */
		if (extra[1] == LIBUSB_DT_HID) { /* bDescriptorType */
			if (extra_length < 6) {
				LOG("Broken HID descriptor: not enough data\n");
				break;
			}
			unsigned char bNumDescriptors = extra[5];
			if (extra_length < (6 + 3 * bNumDescriptors)) {
				LOG("Broken HID descriptor: not enough data for Report metadata\n");
				break;
			}
			for (i = 0; i < bNumDescriptors; i++) {
				if (extra[6 + 3 * i] == LIBUSB_DT_REPORT) {
					result = (uint16_t)extra[6 + 3 * i + 2] << 8 | extra[6 + 3 * i + 1];
					found_hid_report_descriptor = 1;
					break;
				}
			}

			if (!found_hid_report_descriptor) {
				/* We expect to find exactly 1 HID descriptor (LIBUSB_DT_HID)
				   which should contain exactly one HID Report Descriptor metadata (LIBUSB_DT_REPORT). */
				LOG("Broken HID descriptor: missing Report descriptor\n");
			}
			break;
		}

		if (extra[0] == 0) { /* bLength */
			LOG("Broken HID Interface descriptors: zero-sized descriptor\n");
			break;
		}

		/* Iterate over to the next Descriptor */
		extra_length -= extra[0];
		extra += extra[0];
	}

	return result;
}

static int is_xbox360(unsigned short vendor_id, const struct libusb_interface_descriptor *intf_desc)
{
	static const int xb360_iface_subclass = 93;
	static const int xb360_iface_protocol = 1; /* Wired */
	static const int xb360w_iface_protocol = 129; /* Wireless */
	static const int supported_vendors[] = {
		0x0079, /* GPD Win 2 */
		0x044f, /* Thrustmaster */
		0x045e, /* Microsoft */
		0x046d, /* Logitech */
		0x056e, /* Elecom */
		0x06a3, /* Saitek */
		0x0738, /* Mad Catz */
		0x07ff, /* Mad Catz */
		0x0e6f, /* PDP */
		0x0f0d, /* Hori */
		0x1038, /* SteelSeries */
		0x11c9, /* Nacon */
		0x12ab, /* Unknown */
		0x1430, /* RedOctane */
		0x146b, /* BigBen */
		0x1532, /* Razer Sabertooth */
		0x15e4, /* Numark */
		0x162e, /* Joytech */
		0x1689, /* Razer Onza */
		0x1949, /* Lab126, Inc. */
		0x1bad, /* Harmonix */
		0x20d6, /* PowerA */
		0x24c6, /* PowerA */
		0x2c22, /* Qanba */
		0x2dc8, /* 8BitDo */
		0x9886, /* ASTRO Gaming */
	};

	if (intf_desc->bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC &&
	    intf_desc->bInterfaceSubClass == xb360_iface_subclass &&
	    (intf_desc->bInterfaceProtocol == xb360_iface_protocol ||
	     intf_desc->bInterfaceProtocol == xb360w_iface_protocol)) {
		size_t i;
		for (i = 0; i < sizeof(supported_vendors)/sizeof(supported_vendors[0]); ++i) {
			if (vendor_id == supported_vendors[i]) {
				return 1;
			}
		}
	}
	return 0;
}

static int is_xboxone(unsigned short vendor_id, const struct libusb_interface_descriptor *intf_desc)
{
	static const int xb1_iface_subclass = 71;
	static const int xb1_iface_protocol = 208;
	static const int supported_vendors[] = {
		0x044f, /* Thrustmaster */
		0x045e, /* Microsoft */
		0x0738, /* Mad Catz */
		0x0e6f, /* PDP */
		0x0f0d, /* Hori */
		0x10f5, /* Turtle Beach */
		0x1532, /* Razer Wildcat */
		0x20d6, /* PowerA */
		0x24c6, /* PowerA */
		0x2dc8, /* 8BitDo */
		0x2e24, /* Hyperkin */
		0x3537, /* GameSir */
	};

	if (intf_desc->bInterfaceNumber == 0 &&
	    intf_desc->bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC &&
	    intf_desc->bInterfaceSubClass == xb1_iface_subclass &&
	    intf_desc->bInterfaceProtocol == xb1_iface_protocol) {
		size_t i;
		for (i = 0; i < sizeof(supported_vendors)/sizeof(supported_vendors[0]); ++i) {
			if (vendor_id == supported_vendors[i]) {
				return 1;
			}
		}
	}
	return 0;
}

static int should_enumerate_interface(unsigned short vendor_id, const struct libusb_interface_descriptor *intf_desc)
{
#if 0
	printf("Checking interface 0x%x %d/%d/%d/%d\n", vendor_id, intf_desc->bInterfaceNumber, intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol);
#endif

	if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID)
		return 1;

	/* Also enumerate Xbox 360 controllers */
	if (is_xbox360(vendor_id, intf_desc))
		return 1;

	/* Also enumerate Xbox One controllers */
	if (is_xboxone(vendor_id, intf_desc))
		return 1;

	return 0;
}

static struct hid_device_info* hid_enumerate_from_libusb(libusb_device *dev, unsigned short vendor_id, unsigned short product_id, int *oom)
{
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	struct libusb_device_descriptor desc;
	struct libusb_config_descriptor *conf_desc = NULL;
	libusb_device_handle *handle = NULL;
	int j, k;

	int res = libusb_get_device_descriptor(dev, &desc);
	if (res < 0)
		return NULL;

	unsigned short dev_vid = desc.idVendor;
	unsigned short dev_pid = desc.idProduct;

	if ((vendor_id != 0x0 && vendor_id != dev_vid) ||
		(product_id != 0x0 && product_id != dev_pid)) {
		return NULL;
	}

	res = libusb_get_active_config_descriptor(dev, &conf_desc);
	if (res < 0 && res != LIBUSB_ERROR_NO_MEM)
		res = libusb_get_config_descriptor(dev, 0, &conf_desc);
	if (res == LIBUSB_ERROR_NO_MEM) {
		if (oom) {
			*oom = 1;
		}
		return NULL;
	}
	if (conf_desc) {
		for (j = 0; j < conf_desc->bNumInterfaces; j++) {
			const struct libusb_interface *intf = &conf_desc->interface[j];
			for (k = 0; k < intf->num_altsetting; k++) {
				const struct libusb_interface_descriptor *intf_desc;
				intf_desc = &intf->altsetting[k];
				if (should_enumerate_interface(dev_vid, intf_desc)) {
					struct hid_device_info *tmp;

					res = libusb_open(dev, &handle);

#ifdef __ANDROID__
					if (handle) {
						/* There is (a potential) libusb Android backend, in which
						   device descriptor is not accurate up until the device is opened.
						   https://github.com/libusb/libusb/pull/874#discussion_r632801373
						   A workaround is to re-read the descriptor again.
						   Even if it is not going to be accepted into libusb master,
						   having it here won't do any harm, since reading the device descriptor
						   is as cheap as copy 18 bytes of data. */
						libusb_get_device_descriptor(dev, &desc);
					}
#endif

					tmp = create_device_info_for_device(dev, handle, &desc, conf_desc->bConfigurationValue, intf_desc->bInterfaceNumber);
					if (!tmp || !tmp->path) {
						if (oom) {
							*oom = 1;
						}
						hid_free_enumeration(tmp);
						tmp = NULL;
					}
					if (tmp) {
#ifdef INVASIVE_GET_USAGE
						/* TODO: have a runtime check for this section. */

						/*
						This section is removed because it is too
						invasive on the system. Getting a Usage Page
						and Usage requires parsing the HID Report
						descriptor. Getting a HID Report descriptor
						involves claiming the interface. Claiming the
						interface involves detaching the kernel driver.
						Detaching the kernel driver is hard on the system
						because it will unclaim interfaces (if another
						app has them claimed) and the re-attachment of
						the driver will sometimes change /dev entry names.
						It is for these reasons that this section is
						optional. For composite devices, use the interface
						field in the hid_device_info struct to distinguish
						between interfaces. */
						if (handle) {
							uint16_t report_descriptor_size = get_report_descriptor_size_from_interface_descriptors(intf_desc);

							invasive_fill_device_info_usage(tmp, handle, intf_desc->bInterfaceNumber, report_descriptor_size);
						}
#endif /* INVASIVE_GET_USAGE */

						if (cur_dev) {
							cur_dev->next = tmp;
						}
						else {
							root = tmp;
						}
						cur_dev = tmp;
					}

					if (res >= 0) {
						libusb_close(handle);
						handle = NULL;
					}
					break;
				}
			} /* altsettings */
		} /* interfaces */
		libusb_free_config_descriptor(conf_desc);
	}
	return root;
}

/* Enumerates the connected HID devices. Unlike hid_enumerate(), it tells a
 * genuine failure apart from an empty result: `*failed` is set to 1 only when
 * the enumeration itself failed (and the global error describes why), and to 0
 * when the system simply has no matching device (NULL is returned in both
 * cases). No error is registered for the empty case. Both public enumeration
 * and the hotplug snapshot fail on allocation failure, rather than returning
 * a partial list. With `cache` set, stores the infos and connection identities
 * there instead of returning a public list. The caller initializes `ctx`. */
static struct hid_device_info *hid_internal_enumerate(libusb_context *ctx, unsigned short vendor_id, unsigned short product_id, int *failed, struct hid_hotplug_device **cache)
{
	libusb_device **devs;
	libusb_device *dev;
	ssize_t num_devs;
	int i = 0;
	int oom = 0;
	struct hid_hotplug_device **cache_tail = cache;

	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;

	*failed = 1;
	if (cache) {
		*cache = NULL;
	}

	num_devs = libusb_get_device_list(ctx, &devs);
	if (num_devs < 0) {
		register_libusb_error(&last_global_error, num_devs, "libusb_get_device_list");
		return NULL;
	}
	while ((dev = devs[i++]) != NULL) {
		struct hid_device_info *tmp = hid_enumerate_from_libusb(dev, vendor_id, product_id, &oom);
		if (cache && tmp && !oom) {
			*cache_tail = hid_internal_hotplug_create_device(dev, tmp);
			if (*cache_tail) {
				cache_tail = &(*cache_tail)->next;
				continue;
			}
			oom = 1;
		}
		if (oom) {
			hid_free_enumeration(tmp);
			hid_free_enumeration(root);
			if (cache) {
				hid_internal_hotplug_free_devices(*cache);
				*cache = NULL;
			}
			libusb_free_device_list(devs, 1);
			register_string_error(&last_global_error, "Failed to allocate memory for the device enumeration");
			return NULL;
		}
		if (cur_dev) {
			cur_dev->next = tmp;
		}
		else {
			root = tmp;
			cur_dev = tmp;
		}
		/* Traverse to the end of newly attached tail */
		if (cur_dev) {
			while (cur_dev->next) {
				cur_dev = cur_dev->next;
			}
		}
	}

	libusb_free_device_list(devs, 1);

	*failed = 0;

	return root;
}

struct hid_device_info HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	int failed = 0;
	struct hid_device_info *root;
	if (hid_init() < 0) {
		/* register_global_error: global error is set by hid_init */
		return NULL;
	}
	root = hid_internal_enumerate(usb_context, vendor_id, product_id, &failed, NULL);

	if (root == NULL && !failed) {
		if (vendor_id == 0 && product_id == 0) {
			register_string_error(&last_global_error, "No HID devices found in the system.");
		} else {
			register_string_error(&last_global_error, "No HID devices with requested VID/PID found in the system.");
		}
	}

	return root;
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

/* Creates a standalone (next == NULL) deep copy of a single device info entry */
static struct hid_device_info *hid_internal_copy_device_info(const struct hid_device_info *src)
{
	struct hid_device_info *copy = (struct hid_device_info *) calloc(1, sizeof(struct hid_device_info));
	if (copy == NULL) {
		return NULL;
	}

	*copy = *src;
	copy->next = NULL;
	copy->path = src->path ? strdup(src->path) : NULL;
	copy->serial_number = src->serial_number ? wcsdup(src->serial_number) : NULL;
	copy->manufacturer_string = src->manufacturer_string ? wcsdup(src->manufacturer_string) : NULL;
	copy->product_string = src->product_string ? wcsdup(src->product_string) : NULL;

	if ((src->path && !copy->path)
		|| (src->serial_number && !copy->serial_number)
		|| (src->manufacturer_string && !copy->manufacturer_string)
		|| (src->product_string && !copy->product_string)) {
		hid_free_enumeration(copy);
		return NULL;
	}

	return copy;
}

/* Delivers the registration-time ENUMERATE snapshot of a single callback as
 * synthetic arrival events. Called on the event thread only, with `mutex` held
 * and mutex_in_use set. A non-zero return from the callback stops the rest of
 * the pass and deregisters the callback (the removal itself is postponed). */
static void hid_internal_flush_replay(struct hid_hotplug_callback *callback)
{
	while (callback->replay != NULL && callback->events) {
		struct hid_device_info *info = callback->replay;
		int result;
		callback->replay = info->next;
		info->next = NULL;
		result = callback->callback(callback->handle, info, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, callback->user_data);
		hid_free_enumeration(info);
		if (result) {
			callback->events = 0;
			hid_hotplug_context.cb_list_dirty = 1;
		}
	}

	if (callback->replay != NULL) {
		/* The callback was deregistered mid-pass: the undelivered rest of the
		 * snapshot must never be delivered */
		hid_free_enumeration(callback->replay);
		callback->replay = NULL;
	}
}

/* Delivers the pending ENUMERATE snapshots of all registered callbacks
 * (a queued replay marker requests this when there is no live event traffic) */
static void hid_internal_flush_replays(void)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	hid_hotplug_context.mutex_in_use = 1;

	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback != NULL; callback = callback->next) {
		if (callback->replay != NULL && callback->events) {
			hid_internal_flush_replay(callback);
		}
	}

	hid_hotplug_context.mutex_in_use = 0;
	hid_internal_hotplug_remove_postponed();
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Delivers a single device event to the matching callbacks up to (and
 * including) `last`, the dispatch boundary computed once per hotplug message by
 * process_hotplug_event(). Called on the event thread only, with `mutex` held
 * and mutex_in_use set (which keeps `last` alive: removals are postponed). */
static void hid_internal_invoke_callbacks(struct hid_device_info* info, hid_hotplug_event event, struct hid_hotplug_callback *last)
{
	struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs;
	while (callback != NULL) {
		/* The ENUMERATE snapshot is always delivered before any live event for the callback */
		if (callback->replay != NULL && callback->events) {
			hid_internal_flush_replay(callback);
		}
		if ((callback->events & event) && hid_internal_match_device_id(info->vendor_id, info->product_id, callback->vendor_id, callback->product_id)) {
			int result = callback->callback(callback->handle, info, event, callback->user_data);
			/* If the result is non-zero, we mark the callback for removal and proceed */
			if (result) {
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			}
		}
		if (callback == last) {
			break;
		}
		callback = callback->next;
	}
}

/* Record an arrival already represented in the cache, including a retired
 * snapshot identity retained only to suppress this delayed arrival. */
static int hid_internal_hotplug_is_known_device(libusb_device *device)
{
	for (struct hid_hotplug_device **current = &hid_hotplug_context.devs; *current; current = &(*current)->next) {
		struct hid_hotplug_device *dev = *current;
		if (device == dev->device) {
			dev->arrival_seen = 1;
			if (dev->info == NULL) {
				*current = dev->next;
				dev->next = NULL;
				hid_internal_hotplug_free_devices(dev);
			}
			return 1;
		}
	}

	return 0;
}

static int hid_internal_hotplug_cache_stale(void)
{
	int stale;
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	stale = hid_hotplug_context.cache_stale;
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
	return stale;
}

/* Called with `mutex` held. Mark departed entries without freeing infos that
 * an active callback may still be using; the callback thread dispatches their
 * removals after the current message. */
static int hid_internal_hotplug_reconcile(void)
{
	libusb_device **devices;
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	hid_hotplug_context.cache_stale = 0;
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
	ssize_t count = libusb_get_device_list(hid_hotplug_context.context, &devices);
	if (count < 0) {
		hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
		hid_hotplug_context.cache_stale = 1;
		hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
		return (int) count;
	}

	for (struct hid_hotplug_device *dev = hid_hotplug_context.devs; dev != NULL; dev = dev->next) {
		ssize_t i;
		for (i = 0; i < count; i++) {
			if (devices[i] == dev->device) {
				break;
			}
		}
		if (i == count && !dev->removed_before) {
			dev->removed_before = hid_hotplug_context.next_handle;
		}
	}
	libusb_free_device_list(devices, 1);
	return 0;
}

/* Appends a message for the callback thread to the queue and wakes it up.
 * A NULL device marks a request to flush the pending ENUMERATE snapshots. */
static int hid_internal_enqueue_hotplug_message(libusb_device *device, int event)
{
	struct hid_hotplug_queue* msg = (struct hid_hotplug_queue*) calloc(1, sizeof(struct hid_hotplug_queue));
	if (NULL == msg) {
		return -1;
	}

	msg->device = device;
	msg->event = event;
	msg->next = NULL;

	/* Use callback_thread's mutex to protect the queue and signal it */
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	struct hid_hotplug_queue* end = hid_hotplug_context.queue;
	if (end) {
		while (end->next) {
			end = end->next;
		}
		end->next = msg;
	} else {
		hid_hotplug_context.queue = msg;
	}

	/* Wake up the callback thread so it can react to the new message immediately */
	hidapi_thread_cond_signal(&hid_hotplug_context.callback_thread);
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);

	return 0;
}

static int LIBUSB_CALL hid_libusb_hotplug_callback(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void * user_data)
{
	(void)ctx;
	(void)user_data;

	/* Make sure we HOLD the device until we are done with it - otherwise libusb would delete it the moment we exit this function */
	libusb_ref_device(device);

	if (hid_internal_enqueue_hotplug_message(device, event) != 0) {
		/* A dropped arrival cannot be retried. A dropped removal requires cache
		 * reconciliation before another snapshot or an unknown live arrival. */
		if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
			hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
			hid_hotplug_context.cache_stale = 1;
			hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
		}
		libusb_unref_device(device);
	}

	return 0;
}

static void process_hotplug_event(struct hid_hotplug_queue* msg)
{
	if (msg->device == NULL) {
		/* A replay marker: deliver the pending ENUMERATE snapshots promptly
		 * even when there is no live event traffic */
		hid_internal_flush_replays();
		return;
	}

	/* Lock the mutex to avoid race conditions with hid_hotplug_register_callback(),
	 * which iterates devs during HID_API_HOTPLUG_ENUMERATE while holding this mutex.
	 * The mutex is recursive, so a callback may safely re-enter the API. */
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* Mark the list of callbacks as in use for the WHOLE message: a callback
	 * deregistered from within a callback (its own or another's) is only marked
	 * and gets removed by hid_internal_hotplug_remove_postponed() below, which
	 * is what keeps the `last` boundary below alive across the invocations. */
	hid_hotplug_context.mutex_in_use = 1;

	/* Compute the dispatch boundary ONCE for the whole message, before any
	 * callback runs: a callback registered from within a callback (i.e. on this
	 * thread, while this message is being dispatched) must not observe this
	 * event - it receives an already-arrived device through its own
	 * registration-time HID_API_HOTPLUG_ENUMERATE snapshot instead. Recomputing
	 * the boundary per interface would deliver the remaining interfaces of a
	 * multi-interface device to such a callback a second time. */
	struct hid_hotplug_callback *last = hid_hotplug_context.hotplug_cbs;
	while (last != NULL && last->next != NULL) {
		last = last->next;
	}

	if (msg->event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
		/* The device may already be in the cache: the libusb listener is armed
		 * before the initial enumeration, so a device that connects in between
		 * is reported both by the snapshot and as a live arrival. Match only
		 * connection identity: an older queued arrival must not evict a newer
		 * connection already captured at the same port by the snapshot. Suppress
		 * retired snapshot identities, but deliver an unseen queued arrival even
		 * if the device has already disconnected. */
		if (!hid_internal_hotplug_is_known_device(msg->device)) {
			if (hid_internal_hotplug_cache_stale()) {
				hid_internal_hotplug_reconcile();
			}
			struct hid_device_info* info = hid_enumerate_from_libusb(msg->device, 0, 0, NULL);
			struct hid_hotplug_device *dev = info ? hid_internal_hotplug_create_device(msg->device, info) : NULL;

			if (dev) {
				dev->arrival_seen = 1;
				/* Append everything we got to the end of the device list BEFORE
				 * invoking any callback: a callback registered from within a
				 * callback takes its HID_API_HOTPLUG_ENUMERATE snapshot from
				 * `devs`, and this device - which it is excluded from receiving
				 * as a live event (see `last` above) - must be in it. */
				struct hid_hotplug_device **tail = &hid_hotplug_context.devs;
				while (*tail != NULL) {
					tail = &(*tail)->next;
				}
				*tail = dev;

				for (struct hid_device_info* info_cur = info; info_cur != NULL; info_cur = info_cur->next) {
					/* Each invocation describes exactly one device: `device->next`
					 * is NULL by contract. A shallow copy is passed rather than
					 * unlinking the entry, as `devs` must stay whole: a callback
					 * may walk it (through a nested registration) while we are
					 * dispatching. */
					struct hid_device_info single = *info_cur;
					single.next = NULL;
					hid_internal_invoke_callbacks(&single, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, last);
				}
			}
			else {
				hid_free_enumeration(info);
			}
		}
	}
	else if (msg->event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
		struct hid_hotplug_device *removed = NULL;
		struct hid_hotplug_device **removed_tail = &removed;

		/* Detach EVERY interface of the departed device from `devs` BEFORE
		 * invoking any callback: a callback registered from within this dispatch
		 * takes its HID_API_HOTPLUG_ENUMERATE snapshot from `devs`, and the
		 * interfaces that have not been dispatched yet must not be in it - the
		 * device is physically gone, so it would receive a synthetic arrival for
		 * them and never a matching removal (this event is excluded from it by
		 * the `last` boundary above). */
		for (struct hid_hotplug_device **current = &hid_hotplug_context.devs; *current;) {
			struct hid_hotplug_device *dev = *current;
			if (msg->device == dev->device) {
				if (dev->removed_before) {
					/* Registrations whose snapshots excluded this connection must
					 * not receive its delayed removal either. */
					last = NULL;
					for (struct hid_hotplug_callback *cb = hid_hotplug_context.hotplug_cbs;
						cb != NULL && cb->handle < dev->removed_before; cb = cb->next) {
						last = cb;
					}
				}
				/* Detach only this connection, never its same-port replacement */
				*current = dev->next;
				dev->next = NULL;
				*removed_tail = dev;
				removed_tail = &dev->next;
			} else {
				current = &dev->next;
			}
		}

		while (removed != NULL) {
			struct hid_hotplug_device *dev = removed;
			removed = dev->next;
			dev->next = NULL;
			for (struct hid_device_info *info = dev->info; info != NULL; info = info->next) {
				/* Each invocation describes exactly one device */
				struct hid_device_info single = *info;
				single.next = NULL;
				if (last != NULL) {
					hid_internal_invoke_callbacks(&single, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, last);
				}
			}
			if (dev->removed_before && !dev->arrival_seen && dev->info != NULL) {
				/* Only snapshot entries can still have an unprocessed arrival.
				 * Retain their identity until it is drained, a queued LEFT
				 * confirms no arrival is pending, or the context exits. */
				hid_free_enumeration(dev->info);
				dev->info = NULL;
				dev->next = hid_hotplug_context.devs;
				hid_hotplug_context.devs = dev;
			} else {
				hid_internal_hotplug_free_devices(dev);
			}
		}
	}

	hid_hotplug_context.mutex_in_use = 0;
	/* Remove the callbacks whose removal was postponed during the dispatch; this
	 * also winds the event threads down once the last one is gone */
	hid_internal_hotplug_remove_postponed();

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	/* Release the libusb device - we are done with it */
	libusb_unref_device(msg->device);

	/* Cleanup note: this function is called inside a thread that the cleanup function would be waiting to finish */
	/* Any callbacks that await removal are removed above */
	/* No further cleaning is needed */
}

/* Called on the callback thread between messages, never during a callback. */
static void hid_internal_hotplug_dispatch_removed(void)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	while (1) {
		struct hid_hotplug_device *dev = hid_hotplug_context.devs;
		while (dev != NULL && (!dev->removed_before || dev->info == NULL)) {
			dev = dev->next;
		}
		if (dev == NULL) {
			break;
		}
		struct hid_hotplug_queue msg;
		msg.device = libusb_ref_device(dev->device);
		msg.event = LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
		msg.next = NULL;
		process_hotplug_event(&msg);
	}
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

static void* callback_thread(void* user_data)
{
	(void) user_data;

	/* Publish this thread's identity before any callback can run on it: the
	 * global error state must not be written from here (see
	 * hid_callback_thread_id) */
	hid_internal_callback_thread_enter();

	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);

	/* We stop the thread once the shutdown is requested (the last callback is
	 * removed) and the queue has been drained */
	while (1) {
		/* Wait for events to arrive or shutdown signal */
		while (!hid_hotplug_context.queue && !hid_hotplug_context.shutdown_pending) {
			hidapi_thread_cond_wait(&hid_hotplug_context.callback_thread);
		}

		/* Process all pending events from the queue */
		while (hid_hotplug_context.queue) {
			struct hid_hotplug_queue *cur_event = hid_hotplug_context.queue;
			hid_hotplug_context.queue = cur_event->next;

			/* Release the lock while processing to avoid blocking event producers */
			hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
			process_hotplug_event(cur_event);
			free(cur_event);
			hid_internal_hotplug_dispatch_removed();
			hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
		}

		if (hid_hotplug_context.shutdown_pending) {
			break;
		}
	}

	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);

	hid_internal_callback_thread_leave();

	return NULL;
}

/* The libusb event thread. The callback thread is started by the registration
 * (which can report a failure to start it), not from here, and is joined below. */
static void* hotplug_thread(void* user_data)
{
	(void) user_data;
	int error_logged = 0;
	const struct timespec retry_delay = { 0, 5000000 };

	/* 5 msec timeout seems reasonable; don't set too low to avoid high CPU usage */
	/* This timeout only affects how much time it takes to stop the thread */
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 5000;

	while (1) {
		unsigned char shutdown;
		/* The shutdown flag is set under callback_thread's mutex: read it under
		 * the same one to synchronize with the writer */
		hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
		shutdown = hid_hotplug_context.shutdown_pending;
		hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);
		if (shutdown) {
			break;
		}

		/* This will allow libusb to call the callbacks, which will fill up the queue */
		int res = libusb_handle_events_timeout_completed(hid_hotplug_context.context, &tv, NULL);
		if (res < 0 && res != LIBUSB_ERROR_TIMEOUT && res != LIBUSB_ERROR_INTERRUPTED) {
			if (!error_logged) {
				LOG("Hotplug event handling failed: (%d) %s\n", res, libusb_error_name(res));
				error_logged = 1;
			}
			nanosleep(&retry_delay, NULL);
		}
	}

	/* Disarm the libusb listener: no new messages can be enqueued after this */
	libusb_hotplug_deregister_callback(hid_hotplug_context.context, hid_hotplug_context.callback_handle);

	hidapi_thread_join(&hid_hotplug_context.callback_thread);

	/* Free anything still in the queue (the callback thread may exit before the
	 * last messages are enqueued); the devices must be unreferenced before their
	 * libusb context is destroyed. Nothing else can touch the queue anymore. */
	while (hid_hotplug_context.queue) {
		struct hid_hotplug_queue *msg = hid_hotplug_context.queue;
		hid_hotplug_context.queue = msg->next;
		if (msg->device) {
			libusb_unref_device(msg->device);
		}
		free(msg);
	}

	/* Self-removal may defer the pump's join indefinitely. Release its context
	 * now, after all queued and cached device references have been released. */
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	hid_internal_hotplug_free_devices(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;
	libusb_exit(hid_hotplug_context.context);
	hid_hotplug_context.context = NULL;
	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	return NULL;
}

/* Rolls a failed registration back to the state the hotplug machinery was in
 * before it: frees the callback that was never added to the list and, when it
 * would have been the first one, tears the freshly created libusb context and
 * device cache down again. Called with `mutex` held. For a first registration,
 * no event thread is running (the caller must have joined the callback thread
 * if it managed to start it); otherwise the earlier generation is untouched
 * and only the never-added callback is freed.
 * The caller registers the error itself, as the roll-back may overwrite it. */
static void hid_internal_hotplug_unwind_registration(struct hid_hotplug_callback *hotplug_cb, int is_first_callback)
{
	hid_free_enumeration(hotplug_cb->replay);
	free(hotplug_cb);

	if (!is_first_callback) {
		return;
	}

	/* Drop anything enqueued by synchronous libusb I/O during the snapshot,
	 * or by the replay marker. Neither event thread is running now. */
	hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
	while (hid_hotplug_context.queue) {
		struct hid_hotplug_queue *msg = hid_hotplug_context.queue;
		hid_hotplug_context.queue = msg->next;
		if (msg->device) {
			libusb_unref_device(msg->device);
		}
		free(msg);
	}
	hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);

	libusb_hotplug_deregister_callback(hid_hotplug_context.context, hid_hotplug_context.callback_handle);
	hid_internal_hotplug_free_devices(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;
	libusb_exit(hid_hotplug_context.context);
	hid_hotplug_context.context = NULL;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_register_callback(unsigned short vendor_id, unsigned short product_id, int events, int flags, hid_hotplug_callback_fn callback, void *user_data, hid_hotplug_callback_handle *callback_handle)
{
	if (callback_handle != NULL) {
		*callback_handle = 0;
	}

	if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		register_string_error(&last_global_error, "Hotplug is not supported by this version of libusb");
		return -1;
	}

	/* Check params */
	if (events == 0
		|| (events & ~(HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT))) {
		register_string_error(&last_global_error, "Invalid hotplug events mask");
		return -1;
	}

	if (flags & ~(HID_API_HOTPLUG_ENUMERATE)) {
		register_string_error(&last_global_error, "Invalid hotplug flags");
		return -1;
	}

	if (callback == NULL) {
		register_string_error(&last_global_error, "Hotplug callback function is NULL");
		return -1;
	}

	struct hid_hotplug_callback* hotplug_cb = (struct hid_hotplug_callback*)calloc(1, sizeof(struct hid_hotplug_callback));

	if (hotplug_cb == NULL) {
		register_string_error(&last_global_error, "Failed to allocate memory for a hotplug callback");
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

	/* Ensure we are ready to actually use the mutex, and lock it to avoid race conditions */
	hid_internal_hotplug_init_and_lock();

	/* If a previous generation of the event threads is still winding down (the
	 * last callback was removed from the event thread itself, so its join had to
	 * be deferred), finish it before starting a new one */
	hid_internal_hotplug_settle_shutdown();

	/* Registration implicitly initializes HIDAPI (as if by hid_init()); done
	 * under the mutex so concurrent registrations do not race in it. The
	 * application must serialize hid_exit() against registration. */
	if (!usb_context && hid_init() < 0) {
		/* register_global_error: global error is already set by hid_init */
		free(hotplug_cb);
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	/* Handles are never reused, as a stale handle must not silently address a
	 * live callback. Refuse to register rather than to overflow (undefined) or
	 * to wrap around into the handles still in use. */
	if (hid_hotplug_context.next_handle == INT_MAX) {
		register_string_error(&last_global_error, "No hotplug callback handles left");
		free(hotplug_cb);
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	int is_first_callback = (hid_hotplug_context.hotplug_cbs == NULL);

	if (is_first_callback) {
		int res = libusb_init(&hid_hotplug_context.context);
		if (res) {
			register_libusb_error(&last_global_error, res, "hotplug/libusb_init");
			free(hotplug_cb);
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}
		hidapi_thread_mutex_lock(&hid_hotplug_context.callback_thread);
		hid_hotplug_context.cache_stale = 0;
		hidapi_thread_mutex_unlock(&hid_hotplug_context.callback_thread);

		/* Arm a global callback for ALL USB devices (HID is an interface class;
		 * hid_enumerate_from_libusb() filters supported interfaces on the callback
		 * thread) BEFORE taking the snapshot from this same context: libusb does
		 * not report the devices that are already connected when the listener is
		 * armed (LIBUSB_HOTPLUG_ENUMERATE is deliberately not used, the snapshot
		 * takes that role), so a device connecting the other way around - after
		 * the snapshot but before the listener - would be in neither, and its
		 * removal would later go unreported as well. The reverse order can only
		 * report a device twice, which the arrival path deduplicates against
		 * `devs` (see hid_internal_hotplug_is_known_device). */
		res = libusb_hotplug_register_callback(hid_hotplug_context.context,
											LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
											0, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, &hid_libusb_hotplug_callback, NULL,
											&hid_hotplug_context.callback_handle);
		if (res) {
			/* Major malfunction, failed to register a callback */
			register_libusb_error(&last_global_error, res, "libusb_hotplug_register_callback");
			libusb_exit(hid_hotplug_context.context);
			hid_hotplug_context.context = NULL;
			free(hotplug_cb);
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}

		/* Fill already connected devices so we can use this info in disconnection
		 * notification. An empty system is not a failure of the registration, but
		 * a failed enumeration is: silently caching an empty list would make every
		 * already-connected device invisible to this and all later callbacks. */
		int enumeration_failed = 0;
		hid_internal_enumerate(hid_hotplug_context.context, 0, 0, &enumeration_failed, &hid_hotplug_context.devs);
		if (enumeration_failed) {
			/* register_global_error: global error is already set by hid_internal_enumerate */
			hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}
	}

	if (hid_internal_hotplug_cache_stale()) {
		int res = hid_internal_hotplug_reconcile();
		if (res < 0) {
			hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
			register_libusb_error(&last_global_error, res, "hotplug/libusb_get_device_list");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}
	}

	if ((flags & HID_API_HOTPLUG_ENUMERATE) && (events & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)) {
		/* Take a registration-time snapshot of the matching connected devices:
		 * it is replayed asynchronously on the event thread as synthetic arrival
		 * events, never from within this call (see hid_internal_flush_replay).
		 * All or nothing: a partially copied snapshot would silently hide a
		 * connected device from the callback forever. */
		struct hid_device_info *replay_tail = NULL;
		for (struct hid_hotplug_device *dev = hid_hotplug_context.devs; dev != NULL; dev = dev->next) {
			if (dev->removed_before) {
				continue;
			}
			for (struct hid_device_info *device = dev->info; device != NULL; device = device->next) {
				struct hid_device_info *copy;
				if (!hid_internal_match_device_id(device->vendor_id, device->product_id, hotplug_cb->vendor_id, hotplug_cb->product_id)) {
					continue;
				}
				copy = hid_internal_copy_device_info(device);
				if (copy == NULL) {
					hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
					register_string_error(&last_global_error, "Failed to allocate memory for the hotplug device snapshot");
					pthread_mutex_unlock(&hid_hotplug_context.mutex);
					return -1;
				}
				if (replay_tail != NULL) {
					replay_tail->next = copy;
				}
				else {
					hotplug_cb->replay = copy;
				}
				replay_tail = copy;
			}
		}
	}

	int removals_pending = 0;
	for (struct hid_hotplug_device *dev = hid_hotplug_context.devs; dev != NULL; dev = dev->next) {
		if (dev->removed_before && dev->info != NULL) {
			removals_pending = 1;
			break;
		}
	}
	if (hotplug_cb->replay != NULL || removals_pending || hid_internal_hotplug_cache_stale()) {
		/* Wake the callback thread up so the snapshot is delivered promptly even
		 * with no live event traffic, and reconciled removals are dispatched.
		 * Enqueued (and, for the first callback,
		 * before the threads are even started) while holding the mutex the
		 * delivery needs, so the callback is always in the list by the time the
		 * marker is acted upon. Without the marker the snapshot would be stuck
		 * until the next live event, which may never come. */
		if (hid_internal_enqueue_hotplug_message(NULL, 0) != 0) {
			hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
			register_string_error(&last_global_error, "Failed to allocate memory for a hotplug message");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}
	}

	if (is_first_callback) {
		/* Initialization succeeded! We run the threads now. The callback thread
		 * is started here rather than from the libusb thread so that a failure to
		 * start it can be reported. Neither thread can deliver anything before we
		 * release the mutex. */
		if (hidapi_thread_create(&hid_hotplug_context.callback_thread, callback_thread, NULL) != 0) {
			hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
			register_string_error(&last_global_error, "Failed to start the hotplug callback thread");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}

		if (hidapi_thread_create(&hid_hotplug_context.libusb_thread, hotplug_thread, NULL) != 0) {
			/* Stop the callback thread we have just started. The mutex is
			 * released while joining, as the thread locks it to process the
			 * replay marker (a no-op: the callback is not in the list yet). */
			hid_internal_hotplug_set_shutdown_pending(1);

			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			hidapi_thread_join(&hid_hotplug_context.callback_thread);
			pthread_mutex_lock(&hid_hotplug_context.mutex);

			hid_internal_hotplug_set_shutdown_pending(0);

			hid_internal_hotplug_unwind_registration(hotplug_cb, is_first_callback);
			register_string_error(&last_global_error, "Failed to start the hotplug event thread");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			return -1;
		}

		hid_hotplug_context.threads_running = 1;
	}

	/* Commit the registration: from here on nothing can fail */
	hotplug_cb->handle = hid_hotplug_context.next_handle++;

	/* Append the new callback to the end */
	if (hid_hotplug_context.hotplug_cbs != NULL) {
		struct hid_hotplug_callback *last = hid_hotplug_context.hotplug_cbs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = hotplug_cb;
	}
	else {
		hid_hotplug_context.hotplug_cbs = hotplug_cb;
	}

	/* Return the allocated handle: it is guaranteed to be written before any
	 * events can be delivered, as they are dispatched under the same mutex */
	if (callback_handle != NULL) {
		*callback_handle = hotplug_cb->handle;
	}

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	return 0;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_deregister_callback(hid_hotplug_callback_handle callback_handle)
{
	if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		register_string_error(&last_global_error, "Hotplug is not supported by this version of libusb");
		return -1;
	}

	if (callback_handle <= 0) {
		register_string_error(&last_global_error, "Invalid or unknown hotplug callback handle");
		return -1;
	}

	/* Fails only if the machinery was never initialized in this process.
	 * mutex_ready is a one-way latch: after hid_exit() the never-destroyed mutex
	 * is locked normally and the empty list below reports the stale handle.
	 * On success `mutex` is locked. */
	if (hid_internal_hotplug_lock() < 0) {
		register_string_error(&last_global_error, "Invalid or unknown hotplug callback handle");
		return -1;
	}

	int result = -1;

	/* Remove this notification */
	for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
		/* A callback whose removal is already postponed (events == 0) is gone as
		 * far as the caller is concerned: deregistering it a second time must
		 * fail, not silently succeed */
		if ((*current)->handle == callback_handle && (*current)->events != 0) {
			/* Check if we were already in a locked state, as we are NOT allowed to remove any callbacks if we are */
			if (hid_hotplug_context.mutex_in_use) {
				/* Postpone the removal; the callback receives no events
				 * (including undelivered synthetic ones) from now on */
				(*current)->events = 0;
				hid_free_enumeration((*current)->replay);
				(*current)->replay = NULL;
				hid_hotplug_context.cb_list_dirty = 1;
			} else {
				struct hid_hotplug_callback *next = (*current)->next;
				hid_free_enumeration((*current)->replay);
				free(*current);
				*current = next;
			}
			result = 0;
			break;
		}
	}

	/* Deregistering the last callback stops the machinery: unless we are the
	 * event thread (which cannot join itself), do it synchronously, so that no
	 * callback can be running anymore by the time we return */
	hid_internal_hotplug_cleanup_sync();

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	if (result < 0) {
		register_string_error(&last_global_error, "Invalid or unknown hotplug callback handle");
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
	if (!devs) {
		/* register_global_error: global error is already set by hid_enumerate */
		return NULL;
	}

	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (cur_dev->serial_number &&
				    wcscmp(serial_number, cur_dev->serial_number) == 0) {
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
		register_string_error(&last_global_error, "Device with requested VID/PID/(SerialNumber) not found");
	}

	hid_free_enumeration(devs);

	return handle;
}

static void LIBUSB_CALL read_callback(struct libusb_transfer *transfer)
{
	hid_device *dev = (hid_device *) transfer->user_data;
	int res;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {

		struct input_report *rpt = (struct input_report*) malloc(sizeof(*rpt));
		rpt->data = (uint8_t*) malloc(transfer->actual_length);
		memcpy(rpt->data, transfer->buffer, transfer->actual_length);
		rpt->len = transfer->actual_length;
		rpt->next = NULL;

		hidapi_thread_mutex_lock(&dev->thread_state);

		/* Attach the new report object to the end of the list. */
		if (dev->input_reports == NULL) {
			/* The list is empty. Put it at the root. */
			dev->input_reports = rpt;
			hidapi_thread_cond_signal(&dev->thread_state);
		}
		else {
			/* Find the end of the list and attach. */
			struct input_report *cur = dev->input_reports;
			int num_queued = 0;
			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}
			cur->next = rpt;

			/* Pop one off if we've reached 30 in the queue. This
			   way we don't grow forever if the user never reads
			   anything from the device. */
			if (num_queued > 30) {
				return_data(dev, NULL, 0);
			}
		}
		hidapi_thread_mutex_unlock(&dev->thread_state);
	}
	else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		dev->shutdown_thread = 1;
	}
	else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		dev->shutdown_thread = 1;
	}
	else if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
		//LOG("Timeout (normal)\n");
	}
	else {
		LOG("Unknown transfer code: %d\n", transfer->status);
	}

	if (dev->shutdown_thread) {
		dev->transfer_loop_finished = 1;
		return;
	}

	/* Re-submit the transfer object. */
	res = libusb_submit_transfer(transfer);
	if (res != 0) {
		LOG("Unable to submit URB: (%d) %s\n", res, libusb_error_name(res));
		dev->shutdown_thread = 1;
		dev->transfer_loop_finished = 1;
	}
}


static void *read_thread(void *param)
{
	int res;
	hid_device *dev = (hid_device *) param;
	uint8_t *buf;
	const size_t length = dev->input_ep_max_packet_size;

	/* Set up the transfer object. */
	buf = (uint8_t*) malloc(length);
	dev->transfer = libusb_alloc_transfer(0);
	libusb_fill_interrupt_transfer(dev->transfer,
		dev->device_handle,
		dev->input_endpoint,
		buf,
		(int)length,
		read_callback,
		dev,
		5000/*timeout*/);

	/* Make the first submission. Further submissions are made
	   from inside read_callback() */
	res = libusb_submit_transfer(dev->transfer);
	if (res < 0) {
		LOG("libusb_submit_transfer failed: %d %s. Stopping read_thread from running\n", res, libusb_error_name(res));
		dev->shutdown_thread = 1;
		dev->transfer_loop_finished = 1;
	}

	/* Notify the main thread that the read thread is up and running. */
	hidapi_thread_barrier_wait(&dev->thread_state);

	/* Handle all the events. */
	while (!dev->shutdown_thread) {
		res = libusb_handle_events(usb_context);
		if (res < 0) {
			/* There was an error. */
			LOG("read_thread(): (%d) %s\n", res, libusb_error_name(res));

			/* Break out of this loop only on fatal error.*/
			if (res != LIBUSB_ERROR_BUSY &&
			    res != LIBUSB_ERROR_TIMEOUT &&
			    res != LIBUSB_ERROR_OVERFLOW &&
			    res != LIBUSB_ERROR_INTERRUPTED) {
				dev->shutdown_thread = 1;
				break;
			}
		}
	}

	/* Cancel any transfer that may be pending. This call will fail
	   if no transfers are pending, but that's OK. */
	libusb_cancel_transfer(dev->transfer);

	while (!dev->transfer_loop_finished)
		libusb_handle_events_completed(usb_context, &dev->transfer_loop_finished);

	/* Now that the read thread is stopping, Wake any threads which are
	   waiting on data (in hid_read_timeout()). Do this under a mutex to
	   make sure that a thread which is about to go to sleep waiting on
	   the condition actually will go to sleep before the condition is
	   signaled. */
	hidapi_thread_mutex_lock(&dev->thread_state);
	hidapi_thread_cond_broadcast(&dev->thread_state);
	hidapi_thread_mutex_unlock(&dev->thread_state);

	/* The dev->transfer->buffer and dev->transfer objects are cleaned up
	   in hid_close(). They are not cleaned up here because this thread
	   could end either due to a disconnect or due to a user
	   call to hid_close(). In both cases the objects can be safely
	   cleaned up after the call to hidapi_thread_join() (in hid_close()), but
	   since hid_close() calls libusb_cancel_transfer(), on these objects,
	   they can not be cleaned up here. */

	return NULL;
}

static void init_xbox360(libusb_device_handle *device_handle, unsigned short idVendor, unsigned short idProduct, const struct libusb_config_descriptor *conf_desc)
{
	(void)conf_desc;

	if ((idVendor == 0x05ac && idProduct == 0x055b) /* Gamesir-G3w */ ||
	    idVendor == 0x0f0d /* Hori Xbox controllers */) {
		unsigned char data[20];

		/* The HORIPAD FPS for Nintendo Switch requires this to enable input reports.
		   This VID/PID is also shared with other HORI controllers, but they all seem
		   to be fine with this as well.
		*/
		memset(data, 0, sizeof(data));
		libusb_control_transfer(device_handle, 0xC1, 0x01, 0x100, 0x0, data, sizeof(data), 100);
	}
}

static void init_xboxone(libusb_device_handle *device_handle, unsigned short idVendor, unsigned short idProduct, const struct libusb_config_descriptor *conf_desc)
{
	static const int vendor_microsoft = 0x045e;
	static const int xb1_iface_subclass = 71;
	static const int xb1_iface_protocol = 208;
	int j, k, res;

	(void)idProduct;

	for (j = 0; j < conf_desc->bNumInterfaces; j++) {
		const struct libusb_interface *intf = &conf_desc->interface[j];
		for (k = 0; k < intf->num_altsetting; k++) {
			const struct libusb_interface_descriptor *intf_desc = &intf->altsetting[k];
			if (intf_desc->bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC &&
			    intf_desc->bInterfaceSubClass == xb1_iface_subclass &&
			    intf_desc->bInterfaceProtocol == xb1_iface_protocol) {
				int bSetAlternateSetting = 0;

				/* Newer Microsoft Xbox One controllers have a high speed alternate setting */
				if (idVendor == vendor_microsoft &&
					intf_desc->bInterfaceNumber == 0 && intf_desc->bAlternateSetting == 1) {
					bSetAlternateSetting = 1;
				} else if (intf_desc->bInterfaceNumber != 0 && intf_desc->bAlternateSetting == 0) {
					bSetAlternateSetting = 1;
				}

				if (bSetAlternateSetting) {
					res = libusb_claim_interface(device_handle, intf_desc->bInterfaceNumber);
					if (res < 0) {
						LOG("can't claim interface %d: %d\n", intf_desc->bInterfaceNumber, res);
						continue;
					}

					LOG("Setting alternate setting for VID/PID 0x%x/0x%x interface %d to %d\n",  idVendor, idProduct, intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting);

					res = libusb_set_interface_alt_setting(device_handle, intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting);
					if (res < 0) {
						LOG("xbox init: can't set alt setting %d: %d\n", intf_desc->bInterfaceNumber, res);
					}

					libusb_release_interface(device_handle, intf_desc->bInterfaceNumber);
				}
			}
		}
	}
}

/* Reattaches the kernel driver detached during a partial initialization, if any.
 * Shared by every failure path in hidapi_initialize_device() so they cannot drift
 * apart and leave the device with its kernel driver detached (unusable until
 * replug). A no-op unless DETACH_KERNEL_DRIVER support actually detached it. */
static void hidapi_reattach_kernel_driver(hid_device *dev, int interface_num)
{
#ifdef DETACH_KERNEL_DRIVER
	if (dev->is_driver_detached) {
		int res = libusb_attach_kernel_driver(dev->device_handle, interface_num);
		if (res < 0)
			LOG("Failed to reattach the driver to kernel: (%d) %s\n", res, libusb_error_name(res));
	}
#else
	(void)dev;
	(void)interface_num;
#endif
}

static int hidapi_initialize_device(hid_device *dev, const struct libusb_interface_descriptor *intf_desc, const struct libusb_config_descriptor *conf_desc)
{
	int i =0;
	int res = 0;
	struct libusb_device_descriptor desc;
	libusb_get_device_descriptor(libusb_get_device(dev->device_handle), &desc);

#ifdef DETACH_KERNEL_DRIVER
	/* Detach the kernel driver, but only if the
	   device is managed by the kernel */
	dev->is_driver_detached = 0;
	if (libusb_kernel_driver_active(dev->device_handle, intf_desc->bInterfaceNumber) == 1) {
		res = libusb_detach_kernel_driver(dev->device_handle, intf_desc->bInterfaceNumber);
		if (res < 0) {
			LOG("Unable to detach Kernel Driver: (%d) %s\n", res, libusb_error_name(res));
			return 0;
		}
		else {
			dev->is_driver_detached = 1;
			LOG("Driver successfully detached from kernel.\n");
		}
	}
#endif
	res = libusb_claim_interface(dev->device_handle, intf_desc->bInterfaceNumber);
	if (res < 0) {
		LOG("can't claim interface %d: (%d) %s\n", intf_desc->bInterfaceNumber, res, libusb_error_name(res));

		/* The interface was never claimed; just undo the kernel-driver detach. */
		hidapi_reattach_kernel_driver(dev, intf_desc->bInterfaceNumber);
		return 0;
	}

	/* Initialize XBox 360 controllers */
	if (is_xbox360(desc.idVendor, intf_desc)) {
		init_xbox360(dev->device_handle, desc.idVendor, desc.idProduct, conf_desc);
	}

	/* Initialize XBox One controllers */
	if (is_xboxone(desc.idVendor, intf_desc)) {
		init_xboxone(dev->device_handle, desc.idVendor, desc.idProduct, conf_desc);
	}

	/* Store off the string descriptor indexes */
	dev->manufacturer_index = desc.iManufacturer;
	dev->product_index      = desc.iProduct;
	dev->serial_index       = desc.iSerialNumber;

	/* Store off the USB information */
	dev->config_number = conf_desc->bConfigurationValue;
	dev->interface = intf_desc->bInterfaceNumber;

	dev->report_descriptor_size = get_report_descriptor_size_from_interface_descriptors(intf_desc);

	dev->input_endpoint = 0;
	dev->input_ep_max_packet_size = 0;
	dev->output_endpoint = 0;

	/* Find the INPUT and OUTPUT endpoints. An
	   OUTPUT endpoint is not required. */
	for (i = 0; i < intf_desc->bNumEndpoints; i++) {
		const struct libusb_endpoint_descriptor *ep
			= &intf_desc->endpoint[i];

		/* Determine the type and direction of this
		   endpoint. */
		int is_interrupt =
			(ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
			  == LIBUSB_TRANSFER_TYPE_INTERRUPT;
		int is_output =
			(ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
			  == LIBUSB_ENDPOINT_OUT;
		int is_input =
			(ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
			  == LIBUSB_ENDPOINT_IN;

		/* Decide whether to use it for input or output. */
		if (dev->input_endpoint == 0 &&
		    is_interrupt && is_input) {
			/* Use this endpoint for INPUT */
			dev->input_endpoint = ep->bEndpointAddress;
			dev->input_ep_max_packet_size = ep->wMaxPacketSize;
		}
		if (dev->output_endpoint == 0 &&
		    is_interrupt && is_output) {
			/* Use this endpoint for OUTPUT */
			dev->output_endpoint = ep->bEndpointAddress;
		}
	}

	if (hidapi_thread_create(&dev->thread_state, read_thread, dev) != 0) {
		/* Without the read thread nothing would ever release the barrier below:
		 * fail the open instead of blocking in it forever. The caller registers
		 * the error and destroys the device. Unwind the interface claim and the
		 * kernel-driver detach we already performed - libusb_close() alone would
		 * drop the claim but leave the kernel driver detached, i.e. the device
		 * unusable by the kernel until it is replugged. */
		LOG("hidapi_initialize_device: couldn't start the read thread\n");
		libusb_release_interface(dev->device_handle, intf_desc->bInterfaceNumber);
		hidapi_reattach_kernel_driver(dev, intf_desc->bInterfaceNumber);
		return 0;
	}

	/* Wait here for the read thread to be initialized. */
	hidapi_thread_barrier_wait(&dev->thread_state);
	return 1;
}


hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
	hid_device *dev = NULL;

	libusb_device **devs = NULL;
	libusb_device *usb_dev = NULL;
	int res = 0;
	int d = 0;
	int good_open = 0;

	if (hid_init() < 0)
		/* register_global_error: global error is set by hid_init */
		return NULL;

	dev = new_hid_device();
	if (!dev) {
		LOG("hid_open_path failed: Couldn't allocate memory\n");
		register_string_error(&last_global_error, "hid_open_path: Couldn't allocate memory");
		return NULL;
	}

	res = libusb_get_device_list(usb_context, &devs);
	if (res < 0) {
		register_libusb_error(&last_global_error, res, "hid_open_path/libusb_get_device_list");
		free_hid_device(dev);
		return NULL;
	}
	while ((usb_dev = devs[d++]) != NULL && !good_open) {
		struct libusb_device_descriptor desc;
		struct libusb_config_descriptor *conf_desc = NULL;
		int j,k;

		res = libusb_get_device_descriptor(usb_dev, &desc);
		if (res < 0)
			continue;

		res = libusb_get_active_config_descriptor(usb_dev, &conf_desc);
		if (res < 0)
			libusb_get_config_descriptor(usb_dev, 0, &conf_desc);
		if (!conf_desc)
			continue;

		for (j = 0; j < conf_desc->bNumInterfaces && !good_open; j++) {
			const struct libusb_interface *intf = &conf_desc->interface[j];
			for (k = 0; k < intf->num_altsetting && !good_open; k++) {
				const struct libusb_interface_descriptor *intf_desc = &intf->altsetting[k];
				if (should_enumerate_interface(desc.idVendor, intf_desc)) {
					char dev_path[64];
					get_path(&dev_path, usb_dev, conf_desc->bConfigurationValue, intf_desc->bInterfaceNumber);
					if (!strcmp(dev_path, path)) {
						/* Matched Paths. Open this device */

						/* OPEN HERE */
						res = libusb_open(usb_dev, &dev->device_handle);
						if (res < 0) {
							LOG("can't open device\n");
							register_libusb_error(&last_global_error, res, "hid_open_path/libusb_open");
							break;
						}
						good_open = hidapi_initialize_device(dev, intf_desc, conf_desc);
						if (!good_open) {
							register_string_error(&last_global_error, "hid_open_path: failed to initialize device");
							libusb_close(dev->device_handle);
						}
					}
				}
			}
		}
		libusb_free_config_descriptor(conf_desc);
	}

	libusb_free_device_list(devs, 1);

	/* If we have a good handle, return it. */
	if (good_open) {
		return dev;
	}
	else {
		/* Unable to open any devices. */
		if (last_global_error.error_code == LIBUSB_SUCCESS) {
			register_string_error(&last_global_error, "hid_open_path: device not found");
		}
		free_hid_device(dev);
		return NULL;
	}
}


HID_API_EXPORT hid_device * HID_API_CALL hid_libusb_wrap_sys_device(intptr_t sys_dev, int interface_num)
{
/* 0x01000107 is a LIBUSB_API_VERSION for 1.0.23 - version when libusb_wrap_sys_device was introduced */
#if (!defined(HIDAPI_TARGET_LIBUSB_API_VERSION) || HIDAPI_TARGET_LIBUSB_API_VERSION >= 0x01000107) && (LIBUSB_API_VERSION >= 0x01000107)
	hid_device *dev = NULL;
	struct libusb_config_descriptor *conf_desc = NULL;
	const struct libusb_interface_descriptor *selected_intf_desc = NULL;
	int res = 0;
	int j = 0, k = 0;

	if (hid_init() < 0)
		/* register_global_error: global error is set by hid_init */
		return NULL;

	dev = new_hid_device();
	if (!dev) {
		register_string_error(&last_global_error, "hid_libusb_wrap_sys_device: Couldn't allocate memory");
		return NULL;
	}

	res = libusb_wrap_sys_device(usb_context, sys_dev, &dev->device_handle);
	if (res < 0) {
		LOG("libusb_wrap_sys_device failed: %d %s\n", res, libusb_error_name(res));
		register_libusb_error(&last_global_error, res, "hid_libusb_wrap_sys_device/libusb_wrap_sys_device");
		goto err;
	}

	res = libusb_get_active_config_descriptor(libusb_get_device(dev->device_handle), &conf_desc);
	if (res < 0)
		libusb_get_config_descriptor(libusb_get_device(dev->device_handle), 0, &conf_desc);

	if (!conf_desc) {
		LOG("Failed to get configuration descriptor: %d %s\n", res, libusb_error_name(res));
		register_libusb_error(&last_global_error, res, "hid_libusb_wrap_sys_device/get_config_descriptor");
		goto err;
	}

	/* find matching HID interface */
	for (j = 0; j < conf_desc->bNumInterfaces && !selected_intf_desc; j++) {
		const struct libusb_interface *intf = &conf_desc->interface[j];
		for (k = 0; k < intf->num_altsetting; k++) {
			const struct libusb_interface_descriptor *intf_desc = &intf->altsetting[k];
			if (intf_desc->bInterfaceClass == LIBUSB_CLASS_HID) {
				if (interface_num < 0 || interface_num == intf_desc->bInterfaceNumber) {
					selected_intf_desc = intf_desc;
					break;
				}
			}
		}
	}

	if (!selected_intf_desc) {
		if (interface_num < 0) {
			LOG("Sys USB device doesn't contain a HID interface\n");
			register_string_error(&last_global_error, "hid_libusb_wrap_sys_device: device doesn't contain a HID interface");
		}
		else {
			LOG("Sys USB device doesn't contain a HID interface with number %d\n", interface_num);
			register_string_error(&last_global_error, "hid_libusb_wrap_sys_device: device doesn't contain the requested HID interface");
		}
		goto err;
	}

	if (!hidapi_initialize_device(dev, selected_intf_desc, conf_desc)) {
		register_string_error(&last_global_error, "hid_libusb_wrap_sys_device: failed to initialize device");
		goto err;
	}

	return dev;

err:
	if (conf_desc)
		libusb_free_config_descriptor(conf_desc);
	if (dev->device_handle)
		libusb_close(dev->device_handle);
	free_hid_device(dev);
#else
	(void)sys_dev;
	(void)interface_num;
	LOG("libusb_wrap_sys_device is not available\n");
	register_string_error(&last_global_error, "libusb_wrap_sys_device is not available (libusb API too old)");
#endif
	return NULL;
}

void HID_API_EXPORT hid_libusb_set_write_timeout(hid_device *dev, unsigned int timeout)
{
	dev->write_timeout_ms = timeout;
}

void HID_API_EXPORT hid_libusb_set_send_output_report_timeout(hid_device *dev, unsigned int timeout)
{
	dev->send_output_report_timeout_ms = timeout;
}

void HID_API_EXPORT hid_libusb_set_send_feature_report_timeout(hid_device *dev, unsigned int timeout)
{
	dev->send_feature_report_timeout_ms = timeout;
}

static int hidapi_internal_send_output_report(hid_device *dev, const unsigned char *data, size_t length, unsigned int timeout_ms)
{
	int res = -1;
	int skipped_report_id = 0;
	int report_number;

	if (!data || !length) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	report_number = data[0];

	if (report_number == 0x0) {
		data++;
		length--;
		skipped_report_id = 1;
	}

	res = libusb_control_transfer(dev->device_handle,
		LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_OUT,
		0x09/*HID set_report*/,
		(2/*HID output*/ << 8) | report_number,
		dev->interface,
		(unsigned char *)data, length,
		timeout_ms);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hidapi_internal_send_output_report");
		return -1;
	}

	/* Account for the report ID */
	if (skipped_report_id)
		length++;

	return (int)length;
}

int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	int res;
	int report_number;
	int skipped_report_id = 0;

	if (dev->output_endpoint <= 0) {
		/* No interrupt out endpoint. Use the Control Endpoint */
		return hidapi_internal_send_output_report(dev, data, length, dev->write_timeout_ms);
	}

	if (!data || !length) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	report_number = data[0];

	if (report_number == 0x0) {
		data++;
		length--;
		skipped_report_id = 1;
	}

	/* Use the interrupt out endpoint */
	int actual_length;
	res = libusb_interrupt_transfer(dev->device_handle,
		dev->output_endpoint,
		(unsigned char*)data,
		(int)length,
		&actual_length, dev->write_timeout_ms);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hid_write");
		return -1;
	}

	if (skipped_report_id)
		actual_length++;

	return actual_length;
}

/* Helper function, to simplify hid_read().
   This should be called with dev->mutex locked. */
static int return_data(hid_device *dev, unsigned char *data, size_t length)
{
	/* Copy the data out of the linked list item (rpt) into the
	   return buffer (data), and delete the liked list item. */
	struct input_report *rpt = dev->input_reports;
	size_t len = (length < rpt->len)? length: rpt->len;
	if (len > 0)
		memcpy(data, rpt->data, len);
	dev->input_reports = rpt->next;
	free(rpt->data);
	free(rpt);
	return (int)len;
}

static void cleanup_mutex(void *param)
{
	hid_device *dev = (hid_device *) param;
	hidapi_thread_mutex_unlock(&dev->thread_state);
}


int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
#if 0
	int transferred;
	int res = libusb_interrupt_transfer(dev->device_handle, dev->input_endpoint, data, length, &transferred, 5000);
	LOG("transferred: %d\n", transferred);
	return transferred;
#endif
	/* by initialising this variable right here, GCC gives a compilation warning/error: */
	/* error: variable 'bytes_read' might be clobbered by 'longjmp' or 'vfork' [-Werror=clobbered] */
	int bytes_read; /* = -1; */

	if (!data || !length) {
		register_read_error(dev, "Zero buffer/length");
		return -1;
	}

	register_read_error(dev, NULL);

	hidapi_thread_mutex_lock(&dev->thread_state);
	hidapi_thread_cleanup_push(cleanup_mutex, dev);

	bytes_read = -1;

	/* There's an input report queued up. Return it. */
	if (dev->input_reports) {
		/* Return the first one */
		bytes_read = return_data(dev, data, length);
		goto ret;
	}

	if (dev->shutdown_thread) {
		/* The read thread is no longer running (device disconnected,
		   event loop failure, etc.).
		   An error code of -1 should be returned. */
		bytes_read = -1;
		register_read_error(dev, "hid_read(_timeout): read thread terminated");
		goto ret;
	}

	if (milliseconds == -1) {
		/* Blocking */
		while (!dev->input_reports && !dev->shutdown_thread) {
			hidapi_thread_cond_wait(&dev->thread_state);
		}
		if (dev->input_reports) {
			bytes_read = return_data(dev, data, length);
		}
		else {
			/* Woken up by shutdown_thread without data. */
			register_read_error(dev, "hid_read(_timeout): read thread terminated");
		}
	}
	else if (milliseconds > 0) {
		/* Non-blocking, but called with timeout. */
		int res;
		hidapi_timespec ts;
		hidapi_thread_gettime(&ts);
		hidapi_thread_addtime(&ts, milliseconds);

		while (!dev->input_reports && !dev->shutdown_thread) {
			res = hidapi_thread_cond_timedwait(&dev->thread_state, &ts);
			if (res == 0) {
				if (dev->input_reports) {
					bytes_read = return_data(dev, data, length);
					break;
				}
				if (dev->shutdown_thread) {
					register_read_error(dev, "hid_read(_timeout): read thread terminated");
					break;
				}

				/* Spurious wake up. Loop again. */
			}
			else if (res == HIDAPI_THREAD_TIMED_OUT) {
				/* Timed out. */
				bytes_read = 0;
				break;
			}
			else {
				/* Error. */
				bytes_read = -1;
				register_read_error(dev, "hid_read(_timeout): error waiting for data");
				break;
			}
		}
	}
	else {
		/* Purely non-blocking */
		bytes_read = 0;
	}

ret:
	hidapi_thread_mutex_unlock(&dev->thread_state);
	hidapi_thread_cleanup_pop(0);

	return bytes_read;
}


int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, dev->blocking ? -1 : 0);
}


HID_API_EXPORT const wchar_t * HID_API_CALL hid_read_error(hid_device *dev)
{
	if (dev->last_read_error_str == NULL)
		return L"Success";
	return dev->last_read_error_str;
}


int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = !nonblock;

	return 0;
}


int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	int res = -1;
	int skipped_report_id = 0;
	int report_number;

	if (!data || !length) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	report_number = data[0];

	if (report_number == 0x0) {
		data++;
		length--;
		skipped_report_id = 1;
	}

	res = libusb_control_transfer(dev->device_handle,
		LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_OUT,
		0x09/*HID set_report*/,
		(3/*HID feature*/ << 8) | report_number,
		dev->interface,
		(unsigned char *)data, length,
		dev->send_feature_report_timeout_ms);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hid_send_feature_report");
		return -1;
	}

	/* Account for the report ID */
	if (skipped_report_id)
		length++;

	return (int)length;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res = -1;
	int skipped_report_id = 0;
	int report_number;

	if (!data || !length) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	report_number = data[0];

	if (report_number == 0x0) {
		/* Offset the return buffer by 1, so that the report ID
		   will remain in byte 0. */
		data++;
		length--;
		skipped_report_id = 1;
	}
	res = libusb_control_transfer(dev->device_handle,
		LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_IN,
		0x01/*HID get_report*/,
		(3/*HID feature*/ << 8) | report_number,
		dev->interface,
		(unsigned char *)data, length,
		1000/*timeout millis*/);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hid_get_feature_report");
		return -1;
	}

	if (skipped_report_id)
		res++;

	return res;
}

int HID_API_EXPORT hid_send_output_report(hid_device *dev, const unsigned char *data, size_t length)
{
	return hidapi_internal_send_output_report(dev, data, length, dev->send_output_report_timeout_ms);
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res = -1;
	int skipped_report_id = 0;
	int report_number;

	if (!data || !length) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	report_number = data[0];

	if (report_number == 0x0) {
		/* Offset the return buffer by 1, so that the report ID
		   will remain in byte 0. */
		data++;
		length--;
		skipped_report_id = 1;
	}
	res = libusb_control_transfer(dev->device_handle,
		LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE|LIBUSB_ENDPOINT_IN,
		0x01/*HID get_report*/,
		(1/*HID Input*/ << 8) | report_number,
		dev->interface,
		(unsigned char *)data, length,
		1000/*timeout millis*/);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hid_get_input_report");
		return -1;
	}

	if (skipped_report_id)
		res++;

	return res;
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	/* Cause read_thread() to stop. */
	dev->shutdown_thread = 1;
	libusb_cancel_transfer(dev->transfer);

	/* Wait for read_thread() to end. */
	hidapi_thread_join(&dev->thread_state);

	/* Clean up the Transfer objects allocated in read_thread(). */
	free(dev->transfer->buffer);
	dev->transfer->buffer = NULL;
	libusb_free_transfer(dev->transfer);

	/* release the interface */
	libusb_release_interface(dev->device_handle, dev->interface);

	/* reattach the kernel driver if it was detached */
#ifdef DETACH_KERNEL_DRIVER
	if (dev->is_driver_detached) {
		int res = libusb_attach_kernel_driver(dev->device_handle, dev->interface);
		if (res < 0)
			LOG("Failed to reattach the driver to kernel.\n");
	}
#endif

	/* Close the handle */
	libusb_close(dev->device_handle);

	/* Clear out the queue of received reports. */
	hidapi_thread_mutex_lock(&dev->thread_state);
	while (dev->input_reports) {
		return_data(dev, NULL, 0);
	}
	hidapi_thread_mutex_unlock(&dev->thread_state);

	free_hid_device(dev);
}


int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return hid_get_indexed_string(dev, dev->manufacturer_index, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return hid_get_indexed_string(dev, dev->product_index, string, maxlen);
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	return hid_get_indexed_string(dev, dev->serial_index, string, maxlen);
}

HID_API_EXPORT struct hid_device_info *HID_API_CALL hid_get_device_info(hid_device *dev) {
	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	if (!dev->device_info) {
		struct libusb_device_descriptor desc;
		libusb_device *usb_device = libusb_get_device(dev->device_handle);
		libusb_get_device_descriptor(usb_device, &desc);

		dev->device_info = create_device_info_for_device(usb_device, dev->device_handle, &desc, dev->config_number, dev->interface);

		if (dev->device_info) {
			fill_device_info_usage(dev->device_info, dev->device_handle, dev->interface, dev->report_descriptor_size);
		}
		else {
			register_string_error(&dev->error, "hid_get_device_info: failed to allocate device info");
		}
	}

	return dev->device_info;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	wchar_t *str;
	int res = 0;

	if (!string || !maxlen) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	str = get_usb_string(dev->device_handle, string_index, &res);
	if (str) {
		wcsncpy(string, str, maxlen);
		string[maxlen-1] = L'\0';
		free(str);
		return 0;
	}
	else {
		if (res < 0) {
			register_libusb_error(&dev->error, res, "hid_get_indexed_string");
		}
		else {
			register_string_error(&dev->error, "hid_get_indexed_string: failed to allocate result string");
		}
		return -1;
	}
}


int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device *dev, unsigned char *buf, size_t buf_size)
{
	int res = 0;

	if (!buf || !buf_size) {
		register_string_error(&dev->error, "Zero buffer/length");
		return -1;
	}

	register_libusb_error(&dev->error, LIBUSB_SUCCESS, NULL);

	res = hid_get_report_descriptor_libusb(dev->device_handle, dev->interface, dev->report_descriptor_size, buf, buf_size);

	if (res < 0) {
		register_libusb_error(&dev->error, res, "hid_get_report_descriptor");
		return -1;
	}

	return res;
}

/* Formats - and caches - the error string of one error context. The global
 * context is only ever passed with hid_global_error_mutex held. */
static const wchar_t *hid_internal_error(hidapi_error_ctx *err)
{
	const char *name, *description, *context;
	char *buffer;
	int len;

	if (err->error_code == LIBUSB_SUCCESS) {
		return L"Success";
	}

	if (err->error_code == err->last_error_code_cache
	    && err->error_context == err->last_error_context_cache) {
		if (err->last_error_str)
			return err->last_error_str;
		else
			return L"Error string memory allocation error";
	}
	else {
		free(err->last_error_str);
		err->last_error_str = NULL;
		err->last_error_code_cache = LIBUSB_SUCCESS;
		err->last_error_context_cache = NULL;
	}

	name = libusb_error_name(err->error_code);
	description = libusb_strerror(err->error_code);
	context = err->error_context;

	len = snprintf(NULL, 0, "%s: (%s) %s", context, name, description);

	if (len <= 0) {
		return L"Error string format error";
	}

	buffer = (char *) malloc((size_t)len + 1); /* +1 for terminating NULL */
	if (!buffer) {
		return L"Error string memory allocation error";
	}

	len = snprintf(buffer, (size_t)len + 1, "%s: (%s) %s", context, name, description);

	if (len <= 0) {
		free(buffer);
		return L"Error string format error";
	}

	buffer[len] = '\0';


	err->last_error_str = utf8_to_wchar(buffer, (size_t)len);

	free(buffer);

	if (err->last_error_str != NULL) {
		err->last_error_code_cache = err->error_code;
		err->last_error_context_cache = err->error_context;
		return err->last_error_str;
	}
	else {
		return L"Error string memory allocation error";
	}
}


HID_API_EXPORT const wchar_t * HID_API_CALL hid_error(hid_device *dev)
{
	if (!dev) {
		/* The global error state is shared by every application thread: this
		 * function frees and replaces its cached string, so without the lock two
		 * threads - one here, one in a failing API call - would double-free it.
		 * HIDAPI's own threads never write it (see hid_callback_thread_id); they
		 * only take this leaf lock briefly to check their identity, so holding
		 * it here cannot deadlock anything internal. */
		const wchar_t *res;

		pthread_mutex_lock(&hid_global_error_mutex);
		res = hid_internal_error(&last_global_error);
		pthread_mutex_unlock(&hid_global_error_mutex);

		return res;
	}

	return hid_internal_error(&dev->error);
}

HID_API_EXPORT int HID_API_CALL hid_libusb_error(hid_device *dev)
{
	if (!dev) {
		return last_global_error.error_code;
	}

	return dev->error.error_code;
}


struct lang_map_entry {
	const char *name;
	const char *string_code;
	uint16_t usb_code;
};

#define LANG(name,code,usb_code) { name, code, usb_code }
static struct lang_map_entry lang_map[] = {
	LANG("Afrikaans", "af", 0x0436),
	LANG("Albanian", "sq", 0x041C),
	LANG("Arabic - United Arab Emirates", "ar_ae", 0x3801),
	LANG("Arabic - Bahrain", "ar_bh", 0x3C01),
	LANG("Arabic - Algeria", "ar_dz", 0x1401),
	LANG("Arabic - Egypt", "ar_eg", 0x0C01),
	LANG("Arabic - Iraq", "ar_iq", 0x0801),
	LANG("Arabic - Jordan", "ar_jo", 0x2C01),
	LANG("Arabic - Kuwait", "ar_kw", 0x3401),
	LANG("Arabic - Lebanon", "ar_lb", 0x3001),
	LANG("Arabic - Libya", "ar_ly", 0x1001),
	LANG("Arabic - Morocco", "ar_ma", 0x1801),
	LANG("Arabic - Oman", "ar_om", 0x2001),
	LANG("Arabic - Qatar", "ar_qa", 0x4001),
	LANG("Arabic - Saudi Arabia", "ar_sa", 0x0401),
	LANG("Arabic - Syria", "ar_sy", 0x2801),
	LANG("Arabic - Tunisia", "ar_tn", 0x1C01),
	LANG("Arabic - Yemen", "ar_ye", 0x2401),
	LANG("Armenian", "hy", 0x042B),
	LANG("Azeri - Latin", "az_az", 0x042C),
	LANG("Azeri - Cyrillic", "az_az", 0x082C),
	LANG("Basque", "eu", 0x042D),
	LANG("Belarusian", "be", 0x0423),
	LANG("Bulgarian", "bg", 0x0402),
	LANG("Catalan", "ca", 0x0403),
	LANG("Chinese - China", "zh_cn", 0x0804),
	LANG("Chinese - Hong Kong SAR", "zh_hk", 0x0C04),
	LANG("Chinese - Macau SAR", "zh_mo", 0x1404),
	LANG("Chinese - Singapore", "zh_sg", 0x1004),
	LANG("Chinese - Taiwan", "zh_tw", 0x0404),
	LANG("Croatian", "hr", 0x041A),
	LANG("Czech", "cs", 0x0405),
	LANG("Danish", "da", 0x0406),
	LANG("Dutch - Netherlands", "nl_nl", 0x0413),
	LANG("Dutch - Belgium", "nl_be", 0x0813),
	LANG("English - Australia", "en_au", 0x0C09),
	LANG("English - Belize", "en_bz", 0x2809),
	LANG("English - Canada", "en_ca", 0x1009),
	LANG("English - Caribbean", "en_cb", 0x2409),
	LANG("English - Ireland", "en_ie", 0x1809),
	LANG("English - Jamaica", "en_jm", 0x2009),
	LANG("English - New Zealand", "en_nz", 0x1409),
	LANG("English - Philippines", "en_ph", 0x3409),
	LANG("English - Southern Africa", "en_za", 0x1C09),
	LANG("English - Trinidad", "en_tt", 0x2C09),
	LANG("English - Great Britain", "en_gb", 0x0809),
	LANG("English - United States", "en_us", 0x0409),
	LANG("Estonian", "et", 0x0425),
	LANG("Farsi", "fa", 0x0429),
	LANG("Finnish", "fi", 0x040B),
	LANG("Faroese", "fo", 0x0438),
	LANG("French - France", "fr_fr", 0x040C),
	LANG("French - Belgium", "fr_be", 0x080C),
	LANG("French - Canada", "fr_ca", 0x0C0C),
	LANG("French - Luxembourg", "fr_lu", 0x140C),
	LANG("French - Switzerland", "fr_ch", 0x100C),
	LANG("Gaelic - Ireland", "gd_ie", 0x083C),
	LANG("Gaelic - Scotland", "gd", 0x043C),
	LANG("German - Germany", "de_de", 0x0407),
	LANG("German - Austria", "de_at", 0x0C07),
	LANG("German - Liechtenstein", "de_li", 0x1407),
	LANG("German - Luxembourg", "de_lu", 0x1007),
	LANG("German - Switzerland", "de_ch", 0x0807),
	LANG("Greek", "el", 0x0408),
	LANG("Hebrew", "he", 0x040D),
	LANG("Hindi", "hi", 0x0439),
	LANG("Hungarian", "hu", 0x040E),
	LANG("Icelandic", "is", 0x040F),
	LANG("Indonesian", "id", 0x0421),
	LANG("Italian - Italy", "it_it", 0x0410),
	LANG("Italian - Switzerland", "it_ch", 0x0810),
	LANG("Japanese", "ja", 0x0411),
	LANG("Korean", "ko", 0x0412),
	LANG("Latvian", "lv", 0x0426),
	LANG("Lithuanian", "lt", 0x0427),
	LANG("F.Y.R.O. Macedonia", "mk", 0x042F),
	LANG("Malay - Malaysia", "ms_my", 0x043E),
	LANG("Malay – Brunei", "ms_bn", 0x083E),
	LANG("Maltese", "mt", 0x043A),
	LANG("Marathi", "mr", 0x044E),
	LANG("Norwegian - Bokml", "no_no", 0x0414),
	LANG("Norwegian - Nynorsk", "no_no", 0x0814),
	LANG("Polish", "pl", 0x0415),
	LANG("Portuguese - Portugal", "pt_pt", 0x0816),
	LANG("Portuguese - Brazil", "pt_br", 0x0416),
	LANG("Raeto-Romance", "rm", 0x0417),
	LANG("Romanian - Romania", "ro", 0x0418),
	LANG("Romanian - Republic of Moldova", "ro_mo", 0x0818),
	LANG("Russian", "ru", 0x0419),
	LANG("Russian - Republic of Moldova", "ru_mo", 0x0819),
	LANG("Sanskrit", "sa", 0x044F),
	LANG("Serbian - Cyrillic", "sr_sp", 0x0C1A),
	LANG("Serbian - Latin", "sr_sp", 0x081A),
	LANG("Setsuana", "tn", 0x0432),
	LANG("Slovenian", "sl", 0x0424),
	LANG("Slovak", "sk", 0x041B),
	LANG("Sorbian", "sb", 0x042E),
	LANG("Spanish - Spain (Traditional)", "es_es", 0x040A),
	LANG("Spanish - Argentina", "es_ar", 0x2C0A),
	LANG("Spanish - Bolivia", "es_bo", 0x400A),
	LANG("Spanish - Chile", "es_cl", 0x340A),
	LANG("Spanish - Colombia", "es_co", 0x240A),
	LANG("Spanish - Costa Rica", "es_cr", 0x140A),
	LANG("Spanish - Dominican Republic", "es_do", 0x1C0A),
	LANG("Spanish - Ecuador", "es_ec", 0x300A),
	LANG("Spanish - Guatemala", "es_gt", 0x100A),
	LANG("Spanish - Honduras", "es_hn", 0x480A),
	LANG("Spanish - Mexico", "es_mx", 0x080A),
	LANG("Spanish - Nicaragua", "es_ni", 0x4C0A),
	LANG("Spanish - Panama", "es_pa", 0x180A),
	LANG("Spanish - Peru", "es_pe", 0x280A),
	LANG("Spanish - Puerto Rico", "es_pr", 0x500A),
	LANG("Spanish - Paraguay", "es_py", 0x3C0A),
	LANG("Spanish - El Salvador", "es_sv", 0x440A),
	LANG("Spanish - Uruguay", "es_uy", 0x380A),
	LANG("Spanish - Venezuela", "es_ve", 0x200A),
	LANG("Southern Sotho", "st", 0x0430),
	LANG("Swahili", "sw", 0x0441),
	LANG("Swedish - Sweden", "sv_se", 0x041D),
	LANG("Swedish - Finland", "sv_fi", 0x081D),
	LANG("Tamil", "ta", 0x0449),
	LANG("Tatar", "tt", 0X0444),
	LANG("Thai", "th", 0x041E),
	LANG("Turkish", "tr", 0x041F),
	LANG("Tsonga", "ts", 0x0431),
	LANG("Ukrainian", "uk", 0x0422),
	LANG("Urdu", "ur", 0x0420),
	LANG("Uzbek - Cyrillic", "uz_uz", 0x0843),
	LANG("Uzbek – Latin", "uz_uz", 0x0443),
	LANG("Vietnamese", "vi", 0x042A),
	LANG("Xhosa", "xh", 0x0434),
	LANG("Yiddish", "yi", 0x043D),
	LANG("Zulu", "zu", 0x0435),
	LANG(NULL, NULL, 0x0),
};

uint16_t get_usb_code_for_current_locale(void)
{
	char *locale;
	char search_string[64];
	char *ptr;
	struct lang_map_entry *lang;

	/* Get the current locale. */
	locale = setlocale(0, NULL);
	if (!locale)
		return 0x0;

	/* Make a copy of the current locale string. */
	strncpy(search_string, locale, sizeof(search_string)-1);
	search_string[sizeof(search_string)-1] = '\0';

	/* Chop off the encoding part, and make it lower case. */
	ptr = search_string;
	while (*ptr) {
		*ptr = tolower(*ptr);
		if (*ptr == '.') {
			*ptr = '\0';
			break;
		}
		ptr++;
	}

	/* Find the entry which matches the string code of our locale. */
	lang = lang_map;
	while (lang->string_code) {
		if (!strcmp(lang->string_code, search_string)) {
			return lang->usb_code;
		}
		lang++;
	}

	/* There was no match. Find with just the language only. */
	/* Chop off the variant. Chop it off at the '_'. */
	ptr = search_string;
	while (*ptr) {
		*ptr = tolower(*ptr);
		if (*ptr == '_') {
			*ptr = '\0';
			break;
		}
		ptr++;
	}

#if 0 /* TODO: Do we need this? */
	/* Find the entry which matches the string code of our language. */
	lang = lang_map;
	while (lang->string_code) {
		if (!strcmp(lang->string_code, search_string)) {
			return lang->usb_code;
		}
		lang++;
	}
#endif

	/* Found nothing. */
	return 0x0;
}

#ifdef __cplusplus
}
#endif
