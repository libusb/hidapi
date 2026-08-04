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

/* See Apple Technical Note TN2187 for details on IOHidManager. */

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/USBSpec.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_error.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <wchar.h>
#include <locale.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include "hidapi_darwin.h"

/* The value of the first callback handle to be given upon registration */
/* Can be any arbitrary positive integer */
#define FIRST_HOTPLUG_CALLBACK_HANDLE 1

/* Barrier implementation because Mac OSX doesn't have pthread_barrier.
   It also doesn't have clock_gettime(). So much for POSIX and SUSv2.
   This implementation came from Brent Priddy and was posted on
   StackOverflow. It is used with his permission. */
typedef int pthread_barrierattr_t;
typedef struct pthread_barrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int trip_count;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
	(void) attr;

	if (count == 0) {
		errno = EINVAL;
		return -1;
	}

	if (pthread_mutex_init(&barrier->mutex, 0) != 0) {
		return -1;
	}
	if (pthread_cond_init(&barrier->cond, 0) != 0) {
		pthread_mutex_destroy(&barrier->mutex);
		return -1;
	}
	barrier->trip_count = count;
	barrier->count = 0;

	return 0;
}

static int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	pthread_cond_destroy(&barrier->cond);
	pthread_mutex_destroy(&barrier->mutex);
	return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *barrier)
{
	pthread_mutex_lock(&barrier->mutex);
	++(barrier->count);
	if (barrier->count >= barrier->trip_count) {
		barrier->count = 0;
		pthread_mutex_unlock(&barrier->mutex);
		pthread_cond_broadcast(&barrier->cond);
		return 1;
	}
	else {
		do {
			pthread_cond_wait(&barrier->cond, &(barrier->mutex));
		}
		while (barrier->count != 0);

		pthread_mutex_unlock(&barrier->mutex);
		return 0;
	}
}

static int return_data(hid_device *dev, unsigned char *data, size_t length);

/* Linked List of input reports received from the device. */
struct input_report {
	uint8_t *data;
	size_t len;
	struct input_report *next;
};

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

/* - Run context - */
static	IOHIDManagerRef hid_mgr = 0x0;
static	int is_macos_10_10_or_greater = 0;
static	IOOptionBits device_open_options = 0;
static	wchar_t *last_global_error_str = NULL;
/* --- */

struct hid_device_ {
	IOHIDDeviceRef device_handle;
	IOOptionBits open_options;
	int blocking;
	int disconnected;
	CFStringRef run_loop_mode;
	CFRunLoopRef run_loop;
	CFRunLoopSourceRef source;
	uint8_t *input_report_buf;
	CFIndex max_input_report_len;
	struct input_report *input_reports;
	struct hid_device_info* device_info;

	pthread_t thread;
	pthread_mutex_t mutex; /* Protects input_reports */
	pthread_cond_t condition;
	pthread_barrier_t barrier; /* Ensures correct startup sequence */
	pthread_barrier_t shutdown_barrier; /* Ensures correct shutdown sequence */
	int shutdown_thread;
	wchar_t *last_error_str;
	wchar_t *last_read_error_str;
};

static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (dev == NULL) {
		return NULL;
	}

	dev->device_handle = NULL;
	dev->open_options = device_open_options;
	dev->blocking = 1;
	dev->disconnected = 0;
	dev->run_loop_mode = NULL;
	dev->run_loop = NULL;
	dev->source = NULL;
	dev->input_report_buf = NULL;
	dev->input_reports = NULL;
	dev->device_info = NULL;
	dev->shutdown_thread = 0;
	dev->last_error_str = NULL;
	dev->last_read_error_str = NULL;

	/* Thread objects */
	pthread_mutex_init(&dev->mutex, NULL);
	pthread_cond_init(&dev->condition, NULL);
	pthread_barrier_init(&dev->barrier, NULL, 2);
	pthread_barrier_init(&dev->shutdown_barrier, NULL, 2);

	return dev;
}

static void free_hid_device(hid_device *dev)
{
	if (!dev)
		return;

	/* Delete any input reports still left over. */
	struct input_report *rpt = dev->input_reports;
	while (rpt) {
		struct input_report *next = rpt->next;
		free(rpt->data);
		free(rpt);
		rpt = next;
	}

	/* Free the string and the report buffer. The check for NULL
	   is necessary here as CFRelease() doesn't handle NULL like
	   free() and others do. */
	if (dev->run_loop_mode)
		CFRelease(dev->run_loop_mode);
	if (dev->source)
		CFRelease(dev->source);
	free(dev->input_report_buf);
	free(dev->last_error_str);
	free(dev->last_read_error_str);
	hid_free_enumeration(dev->device_info);

	/* Clean up the thread objects */
	pthread_barrier_destroy(&dev->shutdown_barrier);
	pthread_barrier_destroy(&dev->barrier);
	pthread_cond_destroy(&dev->condition);
	pthread_mutex_destroy(&dev->mutex);

	/* Free the structure itself. */
	free(dev);
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

/* Similar to register_error_str, but allows passing a format string with va_list args into this function. */
static void register_error_str_vformat(wchar_t **error_str, const char *format, va_list args)
{
	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, args);

	register_error_str(error_str, msg);
}

/* True when the calling thread is HIDAPI's internal hotplug event thread; used
   to suppress writes to the global error string made from that thread (see the
   definition after the hotplug context for the full rationale). Must be called
   with global_error_mutex held. */
static int hid_internal_on_event_thread(void);

/* Serializes the mutations of the global error string: the hotplug API is
   thread-safe and its failure paths (and the implicit hid_init()) may write
   the global error from multiple threads concurrently. */
static pthread_mutex_t global_error_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Set the last global error to be reported by hid_error(NULL).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored global error message is freed.
 * Use register_global_error(NULL) to indicate "no error". */
static void register_global_error(const char *msg)
{
	pthread_mutex_lock(&global_error_mutex);
	/* Honor the cross-backend contract (see hidapi.h): a global-error write
	   attempted on the internal hotplug event thread - e.g. from a
	   hid_hotplug_(de)register_callback() call re-entered from within a user
	   callback - must not touch the global error string. Per-device errors go
	   through register_error_str() with a different target and are unaffected;
	   only this process-global string is suppressed. */
	if (!hid_internal_on_event_thread())
		register_error_str(&last_global_error_str, msg);
	pthread_mutex_unlock(&global_error_mutex);
}

/* Similar to register_global_error, but allows passing a format string into this function. */
static void register_global_error_format(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	pthread_mutex_lock(&global_error_mutex);
	/* See register_global_error(): suppressed on the internal event thread. */
	if (!hid_internal_on_event_thread())
		register_error_str_vformat(&last_global_error_str, format, args);
	pthread_mutex_unlock(&global_error_mutex);
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


static CFArrayRef get_array_property(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
	if (ref != NULL && CFGetTypeID(ref) == CFArrayGetTypeID()) {
		return (CFArrayRef)ref;
	} else {
		return NULL;
	}
}

static int32_t get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef ref;
	int32_t value = 0;

	ref = IOHIDDeviceGetProperty(device, key);
	if (ref) {
		if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef) ref, kCFNumberSInt32Type, &value);
			return value;
		}
	}
	return 0;
}

static bool try_get_int_property(IOHIDDeviceRef device, CFStringRef key, int32_t *out_val)
{
	bool result = false;
	CFTypeRef ref;

	ref = IOHIDDeviceGetProperty(device, key);
	if (ref) {
		if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
			result = CFNumberGetValue((CFNumberRef) ref, kCFNumberSInt32Type, out_val);
		}
	}
	return result;
}

static bool try_get_ioregistry_int_property(io_service_t service, CFStringRef property, int32_t *out_val)
{
	bool result = false;
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, property, kCFAllocatorDefault, 0);

	if (ref) {
		if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
			result = CFNumberGetValue((CFNumberRef) ref, kCFNumberSInt32Type, out_val);
		}

		CFRelease(ref);
	}

	return result;
}

static CFArrayRef get_usage_pairs(IOHIDDeviceRef device)
{
	return get_array_property(device, CFSTR(kIOHIDDeviceUsagePairsKey));
}

static unsigned short get_vendor_id(IOHIDDeviceRef device)
{
	return get_int_property(device, CFSTR(kIOHIDVendorIDKey));
}

static unsigned short get_product_id(IOHIDDeviceRef device)
{
	return get_int_property(device, CFSTR(kIOHIDProductIDKey));
}

static int32_t get_max_report_length(IOHIDDeviceRef device)
{
	return get_int_property(device, CFSTR(kIOHIDMaxInputReportSizeKey));
}

static int get_string_property(IOHIDDeviceRef device, CFStringRef prop, wchar_t *buf, size_t len)
{
	CFStringRef str;

	if (!len)
		return 0;

	str = (CFStringRef) IOHIDDeviceGetProperty(device, prop);

	buf[0] = 0;

	if (str && CFGetTypeID(str) == CFStringGetTypeID()) {
		CFIndex str_len = CFStringGetLength(str);
		CFRange range;
		CFIndex used_buf_len;
		CFIndex chars_copied;

		len --;

		range.location = 0;
		range.length = ((size_t) str_len > len)? len: (size_t) str_len;
		chars_copied = CFStringGetBytes(str,
			range,
			kCFStringEncodingUTF32LE,
			(char) '?',
			FALSE,
			(UInt8*)buf,
			len * sizeof(wchar_t),
			&used_buf_len);

		if (chars_copied <= 0)
			buf[0] = 0;
		else
			buf[chars_copied] = 0;

		return 0;
	}
	else
		return -1;

}

static int get_serial_number(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
	return get_string_property(device, CFSTR(kIOHIDSerialNumberKey), buf, len);
}

static int get_manufacturer_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
	return get_string_property(device, CFSTR(kIOHIDManufacturerKey), buf, len);
}

static int get_product_string(IOHIDDeviceRef device, wchar_t *buf, size_t len)
{
	return get_string_property(device, CFSTR(kIOHIDProductKey), buf, len);
}


/* Implementation of wcsdup() for Mac. */
static wchar_t *dup_wcs(const wchar_t *s)
{
	size_t len = wcslen(s);
	wchar_t *ret = (wchar_t*) malloc((len+1)*sizeof(wchar_t));
	if (ret)
		wcscpy(ret, s);

	return ret;
}

/* Initialize the IOHIDManager. Return 0 for success and -1 for failure. */
static int init_hid_manager(void)
{
	/* Initialize all the HID Manager Objects */
	hid_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (hid_mgr) {
		IOHIDManagerSetDeviceMatching(hid_mgr, NULL);
		IOHIDManagerScheduleWithRunLoop(hid_mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		return 0;
	}

	register_global_error("Failed to create IOHIDManager");
	return -1;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

struct hid_hotplug_callback {
    hid_hotplug_callback_handle handle;
    unsigned short vendor_id;
    unsigned short product_id;
    int events; /* bitmask of hid_hotplug_event */
    void *user_data;
    hid_hotplug_callback_fn callback;

    /* Snapshot of the matching devices connected at registration time,
       to be delivered ("replayed") as synthetic HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED
       events on the event thread (HID_API_HOTPLUG_ENUMERATE); NULL once delivered */
    struct hid_device_info *replay;

    /* Pointer to the next notification */
    struct hid_hotplug_callback *next;
};

/* When a HID device is removed, we are no longer able to generate a path for it, but we can still match io_service_t */
struct hid_device_info_ex
{
	struct hid_device_info info;
	io_service_t service;
};

/* --- Hotplug locking: the one global lock order ---

   Two locks are involved in the hotplug machinery:

     (1) hid_hotplug_context.mutex - recursive; guards ALL of the hotplug
         context: the callback list, the device cache and every lifecycle flag
         (thread_state, thread_needs_join, join_in_progress, exiting, ...) as
         well as the CoreFoundation references of the event thread. It is held
         for the whole duration of every callback invocation, and it is
         re-entrant so that a callback may call hid_hotplug_register_callback()
         or hid_hotplug_deregister_callback() from the event thread itself -
         which the API documentation guarantees cannot deadlock.

     (2) global_error_mutex - a leaf lock, held only while the global error
         string is replaced. Nothing is ever acquired while it is held.

   The startup barrier's internal lock (inside pthread_barrier_wait()) is a leaf
   as well.

   GLOBAL LOCK ORDER:
       hid_hotplug_context.mutex  ->  { global_error_mutex, startup_barrier }

   The hotplug mutex is always the OUTERMOST lock; no code holding a leaf lock
   ever tries to acquire it, so no cycle can exist. In particular there is
   deliberately NO bootstrap/startup mutex: a lock ordered *outside* the hotplug
   mutex is fundamentally incompatible with registering from inside a callback
   (which is entered with the hotplug mutex already held), so the one-time
   initialization uses pthread_once(), and the hid_exit() teardown is guarded
   from *inside* the hotplug mutex by the `exiting` flag.

   pthread_join() is only ever called with the hotplug mutex released, and
   pthread_cond_wait() only with exactly one recursion level held (see
   hid_internal_hotplug_collect_thread()).

   The only exception to "all context state is accessed under the mutex" is the
   event thread's startup phase - everything it does before reaching the startup
   barrier: the registering thread that started it holds the mutex and is parked
   at that barrier, so the event thread has exclusive access to the context and
   MUST NOT take the mutex there (that would deadlock against the parked
   registrant). The barrier is the release/acquire edge that publishes what the
   thread has set up. */

static struct hid_hotplug_context {
	/* MacOS specific notification handles */
	IOHIDManagerRef manager;

	/* Thread and RunLoop for the manager to work in */
	pthread_t thread;
	CFRunLoopRef run_loop;
	CFRunLoopSourceRef source;
	CFRunLoopSourceRef replay_source; /* Delivers the initial HID_API_HOTPLUG_ENUMERATE pass of new registrations */
	CFStringRef run_loop_mode;
	pthread_barrier_t startup_barrier; /* Ensures correct startup sequence */

	/* Lifecycle of the event thread: 0 = starting, 1 = running, 2 = stopping or
	   stopped. Only ever read and written under the mutex - the event thread
	   itself never writes it before the startup barrier (the registering thread
	   publishes the startup result, see startup_ok) */
	int thread_state;

	/* HIDAPI unique callback handle counter */
	hid_hotplug_callback_handle next_handle;

	pthread_mutex_t mutex;
	pthread_cond_t join_done; /* Broadcast once the stopped event thread has been collected */

	/* Boolean flags */
	unsigned char mutex_ready; /* The mutex and the condition variable are usable (written once, under pthread_once) */
	unsigned char mutex_in_use;
	unsigned char cb_list_dirty;
	unsigned char thread_needs_join; /* Event thread was started and has not been collected yet */
	unsigned char join_in_progress; /* A thread is currently joining the event thread (with the mutex released) */
	unsigned char exiting; /* hid_exit() is tearing the hotplug machinery down */

	/* Set while the event thread has not passed its startup barrier yet.
	   Read and written ONLY on the event thread (the registering thread sets it
	   before pthread_create(), which is a synchronization point), so it needs no
	   lock - and it must not: during that phase the mutex is held by the parked
	   registrant */
	unsigned char startup_phase;

	/* Written by the event thread before the startup barrier, read by the
	   registering thread after it (the barrier is the synchronization edge) */
	unsigned char startup_ok;

	/* Identity of the running hotplug event thread. Published by that thread as
	   its first action and cleared in its epilogue, so it is valid exactly while
	   an event thread exists. Guarded by global_error_mutex (a leaf mutex), NOT
	   the hotplug mutex: the global-error writer consults it while holding
	   global_error_mutex and must never take the hotplug mutex it may already
	   hold. Read only via hid_internal_on_event_thread(). */
	pthread_t event_thread_id;
	unsigned char event_thread_id_valid;

	/* Linked list of the hotplug callbacks */
	struct hid_hotplug_callback *hotplug_cbs;

	/* Linked list of the device infos (mandatory when the device is disconnected) */
	struct hid_device_info *devs;
} hid_hotplug_context; /* zero-initialized (static storage) */

/* The hotplug mutex and condition variable are created exactly once and are
   never destroyed: they live for the lifetime of the process, so that no thread
   can ever lock a mutex that hid_exit() destroyed underneath it */
static pthread_once_t hid_hotplug_init_once = PTHREAD_ONCE_INIT;

/* HIDAPI's public API contract (see hidapi.h) is that HIDAPI calls made from
   within a hotplug callback do not update the global error string: the callback
   runs on this internal event thread, and an application cannot serialize a
   hid_error(NULL) read against a write from that thread - that would be a
   use-after-free of last_global_error_str. This mirrors the libusb and linux
   backends, which likewise suppress such writes. A callback may re-enter the
   public hid_hotplug_register_callback()/hid_hotplug_deregister_callback(),
   whose success and failure paths both write the global error; those writes are
   suppressed via this check in register_global_error()[_format]().
   Returns non-zero when the caller is the hotplug event thread. Must be called
   with global_error_mutex held (the event_thread_id* fields are guarded by it),
   which the global-error writer already holds. */
static int hid_internal_on_event_thread(void)
{
	return hid_hotplug_context.event_thread_id_valid
		&& pthread_equal(pthread_self(), hid_hotplug_context.event_thread_id);
}

static void hid_internal_hotplug_remove_postponed(void)
{
	/* Unregister the callbacks whose removal was postponed */
	/* This function is always called inside a locked mutex */
	/* However, any actions are only allowed if the mutex is NOT in use and if the DIRTY flag is set */
	if (!hid_hotplug_context.mutex_ready || hid_hotplug_context.mutex_in_use || !hid_hotplug_context.cb_list_dirty) {
		return;
	}

	/* Traverse the list of callbacks and check if any were marked for removal */
	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback *callback = *current;
		if (!callback->events) {
			*current = (*current)->next;
			hid_free_enumeration(callback->replay);
			free(callback);
			continue;
		}
		current = &callback->next;
	}

	/* Clear the flag so we don't start the cycle unless necessary */
	hid_hotplug_context.cb_list_dirty = 0;
}

/* Releases everything that only the collector of the event thread may release.
   Called with the hotplug mutex held, either by the thread that has just joined
   the event thread, or by the event thread itself when nobody is joining it (it
   then detaches itself - see hid_internal_hotplug_thread_epilogue()).
   Both participants have left the startup barrier by then: the registering
   thread holds the mutex across the barrier and releases it only afterwards, so
   acquiring the mutex proves it is out. */
static void hid_internal_hotplug_release_thread(void)
{
	hid_hotplug_context.thread_needs_join = 0;

	pthread_barrier_destroy(&hid_hotplug_context.startup_barrier);

	/* The run loop sources are created by the event thread, but the thread does
	   not release them while it winds down: the references must stay valid so
	   that the run loop can still be woken up until the thread is collected. */
	if (hid_hotplug_context.source) {
		CFRelease(hid_hotplug_context.source);
		hid_hotplug_context.source = NULL;
	}
	if (hid_hotplug_context.replay_source) {
		CFRelease(hid_hotplug_context.replay_source);
		hid_hotplug_context.replay_source = NULL;
	}
	hid_hotplug_context.run_loop = NULL;
}

/* Collects (joins) the event thread once it has been told to stop, and releases
   what only the collector may release. Serializes concurrent joiners and waits
   out a join running on another thread.
   Must be called with the hotplug mutex NOT held by the calling thread, except
   from the event thread itself, where it is a guaranteed no-op (the
   pthread_equal() check below) - that is what keeps pthread_cond_wait() from
   ever being reached with the recursive mutex locked more than once. */
static void hid_internal_hotplug_collect_thread(void)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	while (hid_hotplug_context.thread_needs_join
	       && hid_hotplug_context.hotplug_cbs == NULL
	       && hid_hotplug_context.thread_state == 2
	       && !pthread_equal(pthread_self(), hid_hotplug_context.thread)) {
		if (hid_hotplug_context.join_in_progress) {
			/* Another thread is already joining: wait for it to finish.
			   A condition variable (and not a spin) is essential: the joiner is
			   blocked in pthread_join() waiting for the event thread, which may
			   still need this very mutex to finish an in-flight dispatch. */
			pthread_cond_wait(&hid_hotplug_context.join_done, &hid_hotplug_context.mutex);
			continue;
		}

		hid_hotplug_context.join_in_progress = 1;
		pthread_mutex_unlock(&hid_hotplug_context.mutex);

		/* Join with the mutex released: the exiting thread may still need the
		   mutex to finish an in-flight callback dispatch (issue #794 and the
		   matching cross-thread deadlock). No new event thread can be started
		   while thread_needs_join is set, so the thread handle is stable. */
		pthread_join(hid_hotplug_context.thread, NULL);

		pthread_mutex_lock(&hid_hotplug_context.mutex);
		hid_hotplug_context.join_in_progress = 0;
		hid_internal_hotplug_release_thread();

		/* Wake the threads waiting for this join to complete */
		pthread_cond_broadcast(&hid_hotplug_context.join_done);
	}

	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Must be called with the hotplug mutex held */
static void hid_internal_hotplug_cleanup(void)
{
	if (!hid_hotplug_context.mutex_ready || hid_hotplug_context.mutex_in_use) {
		return;
	}

	/* Before checking if the list is empty, clear any entries whose removal was postponed first */
	hid_internal_hotplug_remove_postponed();

	if (hid_hotplug_context.hotplug_cbs != NULL) {
		return;
	}

	/* Cleanup connected device list */
	hid_free_enumeration(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;

	if (!hid_hotplug_context.thread_needs_join) {
		/* The event thread is not running */
		return;
	}

	if (hid_hotplug_context.thread_state != 2) {
		/* Cause hotplug_thread() to stop. */
		hid_hotplug_context.thread_state = 2;

		/* Wake up the run thread's event loop so that the thread can exit.
		   Both references are still alive: they are only released once the
		   thread has been collected, which cannot happen while this thread
		   holds the mutex. */
		if (hid_hotplug_context.source != NULL && hid_hotplug_context.run_loop != NULL) {
			CFRunLoopSourceSignal(hid_hotplug_context.source);
			CFRunLoopWakeUp(hid_hotplug_context.run_loop);
		}
	}

	/* The join is never performed here: this function runs with the mutex
	   held, and the exiting thread may still need the mutex to finish an
	   in-flight dispatch (it may even be the current thread - issue #794).
	   The stopped thread is collected by hid_internal_hotplug_collect_thread()
	   from the public entry points, with the mutex released. */
}

/* The one-time hotplug initialization, run by pthread_once(). On failure
   mutex_ready is left at 0 and the hotplug API stays unavailable. */
static void hid_internal_hotplug_init_once(void)
{
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0) {
		return;
	}

	/* The mutex must be recursive: a callback runs with it held and is allowed
	   to call hid_hotplug_register_callback()/hid_hotplug_deregister_callback() */
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
		pthread_mutexattr_destroy(&attr);
		return;
	}

	if (pthread_mutex_init(&hid_hotplug_context.mutex, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return;
	}

	pthread_mutexattr_destroy(&attr);

	if (pthread_cond_init(&hid_hotplug_context.join_done, NULL) != 0) {
		pthread_mutex_destroy(&hid_hotplug_context.mutex);
		return;
	}

	hid_hotplug_context.next_handle = FIRST_HOTPLUG_CALLBACK_HANDLE;

	/* Publish the mutex as usable, last */
	hid_hotplug_context.mutex_ready = 1;
}

/* Ensures the hotplug mutex is created. Returns 0 when the hotplug machinery is
   usable, -1 when it could not be initialized (the caller must then fail with a
   retrievable error - locking an uninitialized mutex is undefined behavior).
   pthread_once() provides both the one-time guarantee and the memory
   synchronization for the read of mutex_ready below. */
static int hid_internal_hotplug_init(void)
{
	pthread_once(&hid_hotplug_init_once, hid_internal_hotplug_init_once);

	return hid_hotplug_context.mutex_ready ? 0 : -1;
}

/* Tears the hotplug machinery down (from hid_exit()). Leaves `exiting` set, so
   that a concurrent hid_hotplug_register_callback()/hid_hotplug_deregister_callback()
   fails instead of racing the rest of hid_exit(); hid_internal_hotplug_exit_done()
   clears it once hid_exit() is finished. */
static void hid_internal_hotplug_exit(void)
{
	struct hid_hotplug_callback **current;

	if (hid_internal_hotplug_init() != 0) {
		/* The hotplug mutex could not be created: nothing can ever have been
		   registered, and there is nothing to tear down */
		return;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* Close the hotplug API for the duration of the teardown */
	hid_hotplug_context.exiting = 1;

	/* Remove all callbacks from the list */
	current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback* next = (*current)->next;
		hid_free_enumeration((*current)->replay);
		free(*current);
		*current = next;
	}
	hid_internal_hotplug_cleanup();
	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	/* Join the stopped event thread, with the hotplug mutex released */
	hid_internal_hotplug_collect_thread();

	/* The hotplug mutex is deliberately NOT destroyed: another thread may be
	   about to lock it (it only has to observe `exiting` afterwards), and
	   destroying a mutex under it would be undefined behavior. It costs nothing
	   to keep it for the lifetime of the process. */
}

/* Re-opens the hotplug API after hid_exit() has finished. */
static void hid_internal_hotplug_exit_done(void)
{
	if (hid_internal_hotplug_init() != 0) {
		return;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* The event thread has been collected by now, so it no longer uses the mode */
	if (hid_hotplug_context.run_loop_mode) {
		CFRelease(hid_hotplug_context.run_loop_mode);
		hid_hotplug_context.run_loop_mode = NULL;
	}

	hid_hotplug_context.exiting = 0;

	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Initialize the IOHIDManager if necessary. This is the public function, and
   it is safe to call this function repeatedly. Return 0 for success and -1
   for failure. */
int HID_API_EXPORT hid_init(void)
{
	register_global_error(NULL);

	if (!hid_mgr) {
		is_macos_10_10_or_greater = (kCFCoreFoundationVersionNumber >= 1151.16); /* kCFCoreFoundationVersionNumber10_10 */
		hid_darwin_set_open_exclusive(1); /* Backward compatibility */

		return init_hid_manager();
	}
	/* Already initialized. */
	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
	/* The hotplug thread and the callbacks are stopped/freed unconditionally:
	   hid_hotplug_register_callback() may have initialized the library implicitly
	   without ever creating hid_mgr.
	   This leaves the hotplug API closed (`exiting`), so that a concurrent
	   registration cannot re-enter hid_init() while hid_mgr is being destroyed
	   below */
	hid_internal_hotplug_exit();

	if (hid_mgr) {
		/* Close the HID manager. */
		IOHIDManagerClose(hid_mgr, kIOHIDOptionsTypeNone);
		CFRelease(hid_mgr);
		hid_mgr = NULL;
	}

	/* Free global error message */
	register_global_error(NULL);

	/* Re-open the hotplug API: the library may be initialized again */
	hid_internal_hotplug_exit_done();

	return 0;
}

static int hid_internal_match_device_id(unsigned short vendor_id, unsigned short product_id, unsigned short expected_vendor_id, unsigned short expected_product_id)
{
	return (expected_vendor_id == 0x0 || vendor_id == expected_vendor_id) && (expected_product_id == 0x0 || product_id == expected_product_id);
}

static void process_pending_events(void)
{
	SInt32 res;
	do {
		res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, FALSE);
	} while (res != kCFRunLoopRunFinished && res != kCFRunLoopRunTimedOut);
}

static int read_usb_interface_from_hid_service_parent(io_service_t hid_service)
{
	int32_t result = -1;
	bool success = false;
	io_registry_entry_t current = IO_OBJECT_NULL;
	kern_return_t res;
	int parent_number = 0;

	res = IORegistryEntryGetParentEntry(hid_service, kIOServicePlane, &current);
	while (KERN_SUCCESS == res
			/* Only search up to 3 parent entries.
			 * With the default driver - the parent-of-interest supposed to be the first one,
			 * but lets assume some custom drivers or so, with deeper tree. */
			&& parent_number < 3) {
		io_registry_entry_t parent = IO_OBJECT_NULL;
		int32_t interface_number = -1;
		parent_number++;

		success = try_get_ioregistry_int_property(current, CFSTR(kUSBInterfaceNumber), &interface_number);
		if (success) {
			result = interface_number;
			break;
		}

		res = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent);
		if (parent) {
			IOObjectRelease(current);
			current = parent;
		}

	}

	if (current) {
		IOObjectRelease(current);
		current = IO_OBJECT_NULL;
	}

	return result;
}

static struct hid_device_info *create_device_info_with_usage(IOHIDDeviceRef dev, int32_t usage_page, int32_t usage)
{
	unsigned short dev_vid;
	unsigned short dev_pid;
	enum { BufLen = 256 };
	wchar_t buf[BufLen];
	CFTypeRef transport_prop;

	struct hid_device_info_ex *cur_dev_ex;
	struct hid_device_info *cur_dev;
	io_service_t hid_service;
	kern_return_t res;
	uint64_t entry_id = 0;

	if (dev == NULL) {
		return NULL;
	}

	/* A small trick to store an io_service_t tag along with hid_device_info for matching info with unplugged device */
	cur_dev_ex = (struct hid_device_info_ex *)calloc(1, sizeof(struct hid_device_info_ex));
	if (cur_dev_ex == NULL) {
		return NULL;
	}
	cur_dev = &(cur_dev_ex->info);

	dev_vid = get_vendor_id(dev);
	dev_pid = get_product_id(dev);

	cur_dev->usage_page = usage_page;
	cur_dev->usage = usage;

	/* Fill out the record */
	cur_dev->next = NULL;

	/* Fill in the path (as a unique ID of the service entry) */
	cur_dev->path = NULL;
	hid_service = IOHIDDeviceGetService(dev);
	if (hid_service != MACH_PORT_NULL) {
		res = IORegistryEntryGetRegistryEntryID(hid_service, &entry_id);
		cur_dev_ex->service = hid_service;
	}
	else {
		res = KERN_INVALID_ARGUMENT;
	}

	if (res == KERN_SUCCESS) {
		/* max value of entry_id(uint64_t) is 18446744073709551615 which is 20 characters long,
		   so for (max) "path" string 'DevSrvsID:18446744073709551615' we would need
		   9+1+20+1=31 bytes buffer, but allocate 32 for simple alignment */
		const size_t path_len = 32;
		cur_dev->path = (char *) calloc(1, path_len);
		if (cur_dev->path != NULL) {
			snprintf(cur_dev->path, path_len, "DevSrvsID:%llu", entry_id);
		}
	}

	if (cur_dev->path == NULL) {
		/* for whatever reason, trying to keep it a non-NULL string */
		cur_dev->path = strdup("");
	}

	/* Serial Number */
	get_serial_number(dev, buf, BufLen);
	cur_dev->serial_number = dup_wcs(buf);

	/* Manufacturer and Product strings */
	get_manufacturer_string(dev, buf, BufLen);
	cur_dev->manufacturer_string = dup_wcs(buf);
	get_product_string(dev, buf, BufLen);
	cur_dev->product_string = dup_wcs(buf);

	/* VID/PID */
	cur_dev->vendor_id = dev_vid;
	cur_dev->product_id = dev_pid;

	/* Release Number */
	cur_dev->release_number = get_int_property(dev, CFSTR(kIOHIDVersionNumberKey));

	/* Interface Number.
	 * We can only retrieve the interface number for USB HID devices.
	 * See below */
	cur_dev->interface_number = -1;

	/* Bus Type */
	transport_prop = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));

	if (transport_prop != NULL && CFGetTypeID(transport_prop) == CFStringGetTypeID()) {
		if (CFStringCompare((CFStringRef)transport_prop, CFSTR(kIOHIDTransportUSBValue), 0) == kCFCompareEqualTo) {
			int32_t interface_number = -1;
			cur_dev->bus_type = HID_API_BUS_USB;

			/* A IOHIDDeviceRef used to have this simple property,
			 * until macOS 13.3 - we will try to use it. */
			if (try_get_int_property(dev, CFSTR(kUSBInterfaceNumber), &interface_number)) {
				cur_dev->interface_number = interface_number;
			} else {
				/* Otherwise fallback to io_service_t property.
				 * (of one of the parent services). */
				cur_dev->interface_number = read_usb_interface_from_hid_service_parent(hid_service);

				/* If the above doesn't work -
				 * no (known) fallback exists at this point. */
			}

		/* Match "Bluetooth", "BluetoothLowEnergy" and "Bluetooth Low Energy" strings */
		} else if (CFStringHasPrefix((CFStringRef)transport_prop, CFSTR(kIOHIDTransportBluetoothValue))) {
			cur_dev->bus_type = HID_API_BUS_BLUETOOTH;
		} else if (CFStringCompare((CFStringRef)transport_prop, CFSTR(kIOHIDTransportI2CValue), 0) == kCFCompareEqualTo) {
			cur_dev->bus_type = HID_API_BUS_I2C;
		} else  if (CFStringCompare((CFStringRef)transport_prop, CFSTR(kIOHIDTransportSPIValue), 0) == kCFCompareEqualTo) {
			cur_dev->bus_type = HID_API_BUS_SPI;
		}
	}

	return cur_dev;
}

static struct hid_device_info *create_device_info(IOHIDDeviceRef device)
{
	const int32_t primary_usage_page = get_int_property(device, CFSTR(kIOHIDPrimaryUsagePageKey));
	const int32_t primary_usage = get_int_property(device, CFSTR(kIOHIDPrimaryUsageKey));

	/* Primary should always be first, to match previous behavior. */
	struct hid_device_info *root = create_device_info_with_usage(device, primary_usage_page, primary_usage);
	struct hid_device_info *cur = root;

	if (!root)
		return NULL;

	CFArrayRef usage_pairs = get_usage_pairs(device);

	if (usage_pairs != NULL) {
		struct hid_device_info *next = NULL;
		for (CFIndex i = 0; i < CFArrayGetCount(usage_pairs); i++) {
			CFTypeRef dict = CFArrayGetValueAtIndex(usage_pairs, i);
			if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
				continue;
			}

			CFTypeRef usage_page_ref, usage_ref;
			int32_t usage_page, usage;

			if (!CFDictionaryGetValueIfPresent((CFDictionaryRef)dict, CFSTR(kIOHIDDeviceUsagePageKey), &usage_page_ref) ||
			    !CFDictionaryGetValueIfPresent((CFDictionaryRef)dict, CFSTR(kIOHIDDeviceUsageKey), &usage_ref) ||
					CFGetTypeID(usage_page_ref) != CFNumberGetTypeID() ||
					CFGetTypeID(usage_ref) != CFNumberGetTypeID() ||
					!CFNumberGetValue((CFNumberRef)usage_page_ref, kCFNumberSInt32Type, &usage_page) ||
					!CFNumberGetValue((CFNumberRef)usage_ref, kCFNumberSInt32Type, &usage)) {
					continue;
			}
			if (usage_page == primary_usage_page && usage == primary_usage)
				continue; /* Already added. */

			next = create_device_info_with_usage(device, usage_page, usage);
			cur->next = next;
			if (next != NULL) {
				cur = next;
			}
		}
	}

	return root;
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	CFIndex num_devices;
	int i;

	/* Set up the HID Manager if it hasn't been done */
	if (hid_init() < 0) {
		return NULL;
	}
	/* register_global_error: global error is set/reset by hid_init */

	/* give the IOHIDManager a chance to update itself */
	process_pending_events();

	/* Get a list of the Devices */
	CFMutableDictionaryRef matching = NULL;
	if (vendor_id != 0 || product_id != 0) {
		matching = CFDictionaryCreateMutable(kCFAllocatorDefault, kIOHIDOptionsTypeNone, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

		if (matching && vendor_id != 0) {
			CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &vendor_id);
			CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), v);
			CFRelease(v);
		}

		if (matching && product_id != 0) {
			CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &product_id);
			CFDictionarySetValue(matching, CFSTR(kIOHIDProductIDKey), p);
			CFRelease(p);
		}
	}
	IOHIDManagerSetDeviceMatching(hid_mgr, matching);
	if (matching != NULL) {
		CFRelease(matching);
	}

	CFSetRef device_set = IOHIDManagerCopyDevices(hid_mgr);

	IOHIDDeviceRef *device_array = NULL;

	if (device_set != NULL) {
		/* Convert the list into a C array so we can iterate easily. */
		num_devices = CFSetGetCount(device_set);
		device_array = (IOHIDDeviceRef*) calloc(num_devices, sizeof(IOHIDDeviceRef));
		CFSetGetValues(device_set, (const void **) device_array);
	} else {
		num_devices = 0;
	}

	/* Iterate over each device, making an entry for it. */
	for (i = 0; i < num_devices; i++) {

		IOHIDDeviceRef dev = device_array[i];
		if (!dev) {
			continue;
		}

		struct hid_device_info *tmp = create_device_info(dev);
		if (tmp == NULL) {
			continue;
		}

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

	free(device_array);
	if (device_set != NULL)
		CFRelease(device_set);

	if (root == NULL) {
		if (vendor_id == 0 && product_id == 0) {
			register_global_error("No HID devices found in the system.");
		} else {
			register_global_error("No HID devices with requested VID/PID found in the system.");
		}
	}

	return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
	/* This function is identical to the Linux version. Platform independent. */
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

/* Makes a deep copy of a single hid_device_info entry (the next pointer of
   the copy is always NULL). Returns NULL on allocation failure.
   Every field is copied by hand: this function must be updated whenever
   struct hid_device_info gains a new field.
   Note the allocation asymmetry with the hotplug device cache: the entries of
   the cache are allocated as struct hid_device_info_ex (they carry the
   io_service_t used to recognize a device on removal), while a copy made here
   is a plain struct hid_device_info. A copy must therefore never be passed to
   match_ref_to_info() or added to the device cache - it is only ever handed to
   a hotplug callback, and freed with hid_free_enumeration() like any other
   hid_device_info. */
static struct hid_device_info *hid_internal_copy_device_info(const struct hid_device_info *src)
{
	struct hid_device_info *dst = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
	if (dst == NULL) {
		return NULL;
	}

	dst->path = src->path ? strdup(src->path) : NULL;
	dst->vendor_id = src->vendor_id;
	dst->product_id = src->product_id;
	dst->serial_number = src->serial_number ? dup_wcs(src->serial_number) : NULL;
	dst->release_number = src->release_number;
	dst->manufacturer_string = src->manufacturer_string ? dup_wcs(src->manufacturer_string) : NULL;
	dst->product_string = src->product_string ? dup_wcs(src->product_string) : NULL;
	dst->usage_page = src->usage_page;
	dst->usage = src->usage;
	dst->interface_number = src->interface_number;
	dst->bus_type = src->bus_type;
	dst->next = NULL;

	/* Treat a failed string copy as a failed allocation */
	if ((src->path && !dst->path)
		|| (src->serial_number && !dst->serial_number)
		|| (src->manufacturer_string && !dst->manufacturer_string)
		|| (src->product_string && !dst->product_string)) {
		hid_free_enumeration(dst);
		return NULL;
	}

	return dst;
}

/* Delivers the pending synthetic HID_API_HOTPLUG_ENUMERATE events (the initial
   pass) of a single callback. Called on the event thread, with the mutex held
   and mutex_in_use set. */
static void hid_internal_hotplug_replay_one(struct hid_hotplug_callback *callback)
{
	while (callback->replay != NULL) {
		struct hid_device_info *info = callback->replay;
		callback->replay = info->next;
		info->next = NULL;

		/* Skip the delivery if the callback got deregistered meanwhile */
		if (callback->events & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED) {
			int result = (*callback->callback)(callback->handle, info, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, callback->user_data);
			if (result) {
				/* The callback asked to be deregistered: mark it for removal
				   and drop the rest of its initial pass */
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
				hid_free_enumeration(callback->replay);
				callback->replay = NULL;
			}
		}

		hid_free_enumeration(info);
	}
}

/* Dispatches one event to every matching callback. Called on the event thread
   only, and only once it has passed its startup barrier. */
static void hid_internal_invoke_callbacks(struct hid_device_info *info, hid_hotplug_event event)
{
	/* Defensive: during the startup phase the callback list is provably empty
	   (the first registration inserts its callback only after the barrier) and
	   the hotplug mutex is held by the registering thread parked at that
	   barrier - locking it here would deadlock it and this thread forever */
	if (hid_hotplug_context.startup_phase) {
		return;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	unsigned char old_state = hid_hotplug_context.mutex_in_use;
	hid_hotplug_context.mutex_in_use = 1;

	/* Freeze the dispatch at the last callback registered at this moment:
	   a callback registered from within a callback must not receive the
	   in-flight event - its HID_API_HOTPLUG_ENUMERATE snapshot (taken at
	   registration) and the subsequent events cover it with no losses or
	   duplicates. The list is append-only while mutex_in_use is set. */
	struct hid_hotplug_callback *stop_after = hid_hotplug_context.hotplug_cbs;
	while (stop_after != NULL && stop_after->next != NULL) {
		stop_after = stop_after->next;
	}

	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback *callback = *current;
		/* The initial HID_API_HOTPLUG_ENUMERATE pass (if still pending) is always
		   delivered before any live events for the callback: the replay source
		   might not have fired yet - flush it first */
		if (callback->replay != NULL) {
			hid_internal_hotplug_replay_one(callback);
		}
		if ((callback->events & event) && hid_internal_match_device_id(info->vendor_id, info->product_id,
																	   callback->vendor_id, callback->product_id)) {
			int result = callback->callback(callback->handle, info, event, callback->user_data);
			/* If the result is non-zero, we mark the callback for removal */
			/* Do not use the deregister call as it locks the mutex, and we are currently in a lock */
			if (result) {
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			}
		}
		if (callback == stop_after) {
			break;
		}
		current = &callback->next;
	}

	hid_hotplug_context.mutex_in_use = old_state;
	hid_internal_hotplug_remove_postponed();
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Matches an IOHIDDeviceRef against an entry of the hotplug device cache.
   The entries of the cache are allocated as struct hid_device_info_ex and carry
   the io_service_t of the device: the path cannot be regenerated once the device
   is gone. Never pass an entry that did not come from the cache (see
   hid_internal_copy_device_info()). */
static int match_ref_to_info(IOHIDDeviceRef device, struct hid_device_info *info)
{
	if (!device || !info) {
		return 0;
	}

	struct hid_device_info_ex* ex = (struct hid_device_info_ex*)info;
	io_service_t service = IOHIDDeviceGetService(device);

	/* MACH_PORT_NULL is not a valid identity: two devices that both lack a
	   service must not be treated as the same device (that would make the
	   arrival dedupe suppress the second one, and a removal evict the wrong
	   cache entry). */
	return (service != MACH_PORT_NULL && service == ex->service);
}

/* Returns non-zero when the device is already in the hotplug device cache.
   Called on the event thread, with the mutex held (or during its startup phase,
   where the thread has exclusive access to the context). */
static int hid_internal_hotplug_is_known_device(IOHIDDeviceRef device)
{
	struct hid_device_info *info;

	for (info = hid_hotplug_context.devs; info != NULL; info = info->next) {
		if (match_ref_to_info(device, info)) {
			return 1;
		}
	}

	return 0;
}

static void hid_internal_hotplug_connect_callback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
	struct hid_device_info *info;

	/* The event thread does not lock the mutex and does not dispatch anything
	   before it has passed the startup barrier (the whole initial enumeration is
	   such a window): the mutex is held by the registering thread parked at that
	   barrier - locking it here would deadlock - and the callback list is
	   provably empty then, so there is nothing to dispatch to. The device still
	   goes into the cache: that is what the initial HID_API_HOTPLUG_ENUMERATE
	   snapshot is taken from. */
	const int startup = hid_hotplug_context.startup_phase;

	(void) context;
	(void) result;
	(void) sender;

	/* A device without a backing io_service_t carries no usable identity (see
	   match_ref_to_info()): once cached it would match neither the arrival
	   dedupe nor its own removal, so it would be reported more than once and
	   never evicted. Keep it consistently invisible instead. */
	if (!device || IOHIDDeviceGetService(device) == MACH_PORT_NULL) {
		return;
	}

	if (!startup) {
		/* Lock the mutex to avoid race conditions */
		pthread_mutex_lock(&hid_hotplug_context.mutex);
	}

	/* Once the run loop runs, the IOHIDManager re-reports every device that was
	   already connected when it was opened. Those devices are all in the cache -
	   it is completed synchronously during the thread's startup, before any
	   callback can be registered - so they are NOT new arrivals and must never be
	   dispatched as live events. This is what makes the snapshot boundary
	   deterministic instead of dependent on how long the initial matching burst
	   takes to be delivered. */
	if (hid_internal_hotplug_is_known_device(device)) {
		if (!startup) {
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
		}
		return;
	}

	info = create_device_info(device);
	if (!info) {
		/* Out of memory on the live-arrival path: the device ends up neither in
		   the cache nor in an event, so "reported exactly once" is best effort
		   here. The registration-time paths are hardened against this -
		   hid_internal_hotplug_build_device_cache() fails the startup and
		   hid_hotplug_register_callback() fails the registration rather than
		   commit a partial initial pass - because both still have a caller to
		   report the failure to. An IOKit callback has none, and the
		   IOHIDManager does not re-report the device, so there is nothing left
		   to fail or retry against. (During the startup phase the device is
		   still picked up by hid_internal_hotplug_build_device_cache(), which
		   does fail loudly if it cannot allocate either.) */
		if (!startup) {
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
		}
		return;
	}

	/* Append all we got to the end of the device list BEFORE invoking any
	   callbacks: a callback registering with HID_API_HOTPLUG_ENUMERATE from
	   within a callback must find the arriving entries in its snapshot,
	   as it does not receive the in-flight events */
	if (hid_hotplug_context.devs != NULL) {
		struct hid_device_info* last = hid_hotplug_context.devs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = info;
	}
	else {
		hid_hotplug_context.devs = info;
	}

	if (!startup) {
		/* Invoke the callbacks for each entry; device->next must be NULL for
		   every delivery, so each entry is temporarily severed, truncating the
		   device cache at that entry for the duration of its dispatch (a
		   snapshot taken during the dispatch then ends at the delivered entry) */
		struct hid_device_info *info_cur = info;
		while (info_cur)
		{
			struct hid_device_info *info_next = info_cur->next;
			info_cur->next = NULL;
			hid_internal_invoke_callbacks(info_cur, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED);
			info_cur->next = info_next;
			info_cur = info_next;
		}

		/* Clean up if the last callback was removed during the events */
		hid_internal_hotplug_cleanup();
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
	}
}

static void hid_internal_hotplug_disconnect_callback(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
	struct hid_device_info **current;

	/* Same guard as in the connect callback - and it covers the dispatch, not
	   just the lock: a device removed while the event thread is still starting
	   up (the whole initial enumeration is such a window) must not lock the
	   mutex held by the registrant parked at the startup barrier, nor dispatch
	   anything. There is no callback to notify at that point either: dropping
	   the device from the cache is all that is needed, and the registration's
	   HID_API_HOTPLUG_ENUMERATE snapshot - copied from the cache only after this
	   thread reaches the barrier - then simply does not contain it. */
	const int startup = hid_hotplug_context.startup_phase;

	(void) context;
	(void) result;
	(void) sender;

	if (!startup) {
		pthread_mutex_lock(&hid_hotplug_context.mutex);
	}

	for (current = &hid_hotplug_context.devs; *current;) {
		struct hid_device_info* info = *current;
		if (match_ref_to_info(device, info)) {
			/* If the IOHIDDeviceRef device that's left matches this HID device, we detach it from the list */
			*current = info->next;
			info->next = NULL;
			if (!startup) {
				hid_internal_invoke_callbacks(info, HID_API_HOTPLUG_EVENT_DEVICE_LEFT);
			}
			/* Free every removed device */
			hid_free_enumeration(info);
		} else {
			current = &info->next;
		}
	}

	if (!startup) {
		/* Clean up if the last callback was removed */
		hid_internal_hotplug_cleanup();
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
	}
}

static void hotplug_stop_callback(void* context)
{
	(void) context;
	CFRunLoopStop(hid_hotplug_context.run_loop);
}

static void hotplug_replay_callback(void* context)
{
	(void) context;

	/* Defensive, same as in hid_internal_invoke_callbacks(): the replay source is
	   only ever signalled by hid_hotplug_register_callback() under the mutex and
	   after the startup barrier, so it cannot be performed by the startup drain -
	   where taking the mutex would deadlock against the parked registrant */
	if (hid_hotplug_context.startup_phase) {
		return;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	unsigned char old_state = hid_hotplug_context.mutex_in_use;
	hid_hotplug_context.mutex_in_use = 1;

	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback != NULL; callback = callback->next) {
		if (callback->replay != NULL) {
			hid_internal_hotplug_replay_one(callback);
		}
	}

	hid_hotplug_context.mutex_in_use = old_state;

	/* An initial-pass callback may have deregistered the last callback: clean up if so */
	hid_internal_hotplug_cleanup();

	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

/* Lets the hotplug IOHIDManager process the device-matching events it queued for
   the already connected devices, exactly like the process_pending_events() call
   hid_enumerate() makes before IOHIDManagerCopyDevices().

   This is NOT the snapshot boundary - hid_internal_hotplug_build_device_cache()
   below is - and nothing depends on it draining the burst completely: it can
   only ADD devices to the cache, never move one from the initial snapshot to the
   live events. It runs on the event thread during its startup phase, so the
   connect/disconnect callbacks it triggers only maintain the cache and dispatch
   nothing (no callback is registered yet, and the mutex must not be taken - see
   the locking note at the top of the hotplug code). */
static void hid_internal_hotplug_drain_pending_events(void)
{
	SInt32 res;
	do {
		res = CFRunLoopRunInMode(hid_hotplug_context.run_loop_mode, 0.001, FALSE);
	} while (res != kCFRunLoopRunFinished && res != kCFRunLoopRunTimedOut && res != kCFRunLoopRunStopped);
}

/* Completes the initial device cache from the devices the hotplug IOHIDManager
   matches right now. Runs on the event thread during its startup phase, i.e.
   before any callback can be registered and without the mutex.

   This is the deterministic boundary between "was already connected" and
   "arrived live": IOHIDManagerCopyDevices() answers synchronously - which is
   exactly what hid_enumerate() relies on - so, unlike a timed pump of the run
   loop, the completeness of the snapshot does not depend on how long the initial
   matching burst takes. Every device connected at this point ends up in the
   cache; when the run loop later delivers the matching events for those same
   devices, they are recognized as already known and dropped (see
   hid_internal_hotplug_connect_callback()), so they can never surface as live
   arrivals.

   Devices already added to the cache (by the drain above) are kept: the two
   sources are merged by io_service_t.

   Returns 0 on success, -1 on failure. */
static int hid_internal_hotplug_build_device_cache(void)
{
	CFSetRef device_set;
	CFIndex num_devices;
	CFIndex i;
	IOHIDDeviceRef *device_array;
	struct hid_device_info *tail = hid_hotplug_context.devs;

	while (tail != NULL && tail->next != NULL) {
		tail = tail->next;
	}

	device_set = IOHIDManagerCopyDevices(hid_hotplug_context.manager);
	if (device_set == NULL) {
		/* No device is currently matched: an empty cache is a valid snapshot */
		return 0;
	}

	num_devices = CFSetGetCount(device_set);
	if (num_devices <= 0) {
		CFRelease(device_set);
		return 0;
	}

	device_array = (IOHIDDeviceRef*) calloc((size_t) num_devices, sizeof(IOHIDDeviceRef));
	if (device_array == NULL) {
		CFRelease(device_set);
		return -1;
	}
	CFSetGetValues(device_set, (const void **) device_array);

	for (i = 0; i < num_devices; i++) {
		struct hid_device_info *info;

		if (device_array[i] == NULL) {
			continue;
		}

		/* Same identity requirement as the live-arrival path: an entry with no
		   backing io_service_t could never be deduped against, nor evicted */
		if (IOHIDDeviceGetService(device_array[i]) == MACH_PORT_NULL) {
			continue;
		}

		/* Already in the cache (the drain got to it first) */
		if (hid_internal_hotplug_is_known_device(device_array[i])) {
			continue;
		}

		info = create_device_info(device_array[i]);
		if (info == NULL) {
			/* Out of memory: fail the startup rather than commit a snapshot
			   that is missing a connected device (it would later be reported as
			   a live arrival, which is exactly what the snapshot must prevent) */
			free(device_array);
			CFRelease(device_set);
			return -1;
		}

		if (tail != NULL) {
			tail->next = info;
		}
		else {
			hid_hotplug_context.devs = info;
		}

		/* A device contributes one entry per usage pair */
		while (info->next != NULL) {
			info = info->next;
		}
		tail = info;
	}

	free(device_array);
	CFRelease(device_set);

	return 0;
}

/* Runs at the very end of the event thread. If no other thread is joining it,
   the thread detaches itself and releases its own resources here: otherwise a
   callback that deregisters the last callback from within a callback (including
   by returning non-zero) would leave an unjoined thread, two run loop sources
   and the run loop behind until the next register/deregister/hid_exit() - which
   may never come. */
static void hid_internal_hotplug_thread_epilogue(void)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	/* The event thread is exiting: stop suppressing global-error writes for its
	   pthread id. Cleared under the hotplug mutex - before the thread is detached
	   or collected, and thus before any replacement event thread can be started
	   and publish its own id - so a later thread's id can never be clobbered.
	   Ordering is hotplug mutex -> global_error_mutex, the same order the
	   global-error writer uses when it is called under the hotplug mutex. */
	pthread_mutex_lock(&global_error_mutex);
	hid_hotplug_context.event_thread_id_valid = 0;
	pthread_mutex_unlock(&global_error_mutex);

	if (hid_hotplug_context.thread_needs_join && !hid_hotplug_context.join_in_progress) {
		/* Nobody is inside pthread_join() on this thread, and nobody can enter
		   it any more: the decision is taken under the mutex on both sides (see
		   hid_internal_hotplug_collect_thread()), so there is no double join and
		   no join of a detached thread. */
		pthread_detach(pthread_self());
		hid_internal_hotplug_release_thread();
		pthread_cond_broadcast(&hid_hotplug_context.join_done);
	}

	/* Past this point the thread must not touch the context any more: as soon as
	   the mutex is released, a new event thread may be started */
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
}

static void* hotplug_thread(void* user_data)
{
	int manager_opened = 0;

	(void) user_data;

	/* Publish this thread's identity as the very first action, before anything
	   here can attempt a global-error write, so that any such write on this
	   internal event thread - notably from a user callback that re-enters
	   hid_hotplug_(de)register_callback() - is suppressed (see
	   hid_internal_on_event_thread()). Uses global_error_mutex only: the hotplug
	   mutex must not be taken during the startup phase (the registrant holds it,
	   parked at the startup barrier). */
	pthread_mutex_lock(&global_error_mutex);
	hid_hotplug_context.event_thread_id = pthread_self();
	hid_hotplug_context.event_thread_id_valid = 1;
	pthread_mutex_unlock(&global_error_mutex);

	/* Startup phase: the registering thread holds the hotplug mutex and is
	   parked at the startup barrier, so this thread has exclusive access to the
	   context - and it MUST NOT take the mutex until the barrier has been passed
	   (see the locking note at the top of the hotplug code). No hotplug callback
	   can be dispatched here either: none is registered yet (the first one is
	   inserted only after the barrier). */

	/* The device cache is empty at this point: the event thread is only ever
	   started with no callbacks registered, which is exactly when the previous
	   cache was freed by hid_internal_hotplug_cleanup() */
	hid_free_enumeration(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;

	/* Store a reference to this runloop if we ever need to wake it up - e.g. if we have no callbacks left or hid_exit was called */
	hid_hotplug_context.run_loop = CFRunLoopGetCurrent();

	if (!hid_hotplug_context.run_loop_mode) {
		const char *str = "HIDAPI_hotplug";
		hid_hotplug_context.run_loop_mode = CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);
	}

	if (hid_hotplug_context.run_loop_mode) {
		hid_hotplug_context.manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	}

	if (hid_hotplug_context.manager) {
		CFRunLoopSourceContext ctx;

		/* Ensure the manager runs in this thread */
		IOHIDManagerScheduleWithRunLoop(hid_hotplug_context.manager, hid_hotplug_context.run_loop, hid_hotplug_context.run_loop_mode);

		/* Create the RunLoopSource which is used to signal the
		   event loop to stop when hid_internal_hotplug_cleanup() is called. */
		memset(&ctx, 0, sizeof(ctx));
		ctx.version = 0;
		ctx.perform = &hotplug_stop_callback;
		hid_hotplug_context.source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0/*order*/, &ctx);

		/* Create the RunLoopSource used to deliver the initial
		   HID_API_HOTPLUG_ENUMERATE pass of new registrations on this thread. */
		memset(&ctx, 0, sizeof(ctx));
		ctx.version = 0;
		ctx.perform = &hotplug_replay_callback;
		hid_hotplug_context.replay_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0/*order*/, &ctx);

		if (hid_hotplug_context.source && hid_hotplug_context.replay_source) {
			CFRunLoopAddSource(hid_hotplug_context.run_loop, hid_hotplug_context.source, hid_hotplug_context.run_loop_mode);
			CFRunLoopAddSource(hid_hotplug_context.run_loop, hid_hotplug_context.replay_source, hid_hotplug_context.run_loop_mode);

			/* Set the manager to receive events for ALL HID devices */
			IOHIDManagerSetDeviceMatching(hid_hotplug_context.manager, NULL);

			/* Install callbacks. They only ever fire from this thread's run loop
			   (the manager is scheduled in a private run loop mode of this
			   thread), i.e. from the startup drain below or from the event loop
			   once the startup barrier is passed. */
			IOHIDManagerRegisterDeviceMatchingCallback(hid_hotplug_context.manager,
														hid_internal_hotplug_connect_callback,
														NULL);

			IOHIDManagerRegisterDeviceRemovalCallback(hid_hotplug_context.manager,
														hid_internal_hotplug_disconnect_callback,
														NULL);

			/* Opening the manager enqueues the device-matching events for all
			   the devices that are already connected */
			if (IOHIDManagerOpen(hid_hotplug_context.manager, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
				manager_opened = 1;

				/* Give the manager a chance to process what it just enqueued
				   (best effort; not a fence - see the function comment) ... */
				hid_internal_hotplug_drain_pending_events();

				/* ... and then take the authoritative snapshot of the connected
				   devices synchronously: THIS - and not a timed pump of the run
				   loop - is the boundary between the initial
				   HID_API_HOTPLUG_ENUMERATE pass and the live events */
				if (hid_internal_hotplug_build_device_cache() == 0) {
					hid_hotplug_context.startup_ok = 1;
				}
			}
		}
	}

	/* Hand the startup result over to hid_hotplug_register_callback(), which is
	   waiting at the barrier and publishes it (thread_state) under the mutex.
	   The barrier also publishes everything this thread has set up so far. */
	pthread_barrier_wait(&hid_hotplug_context.startup_barrier);

	/* The context is shared again from here on: the mutex is required */
	hid_hotplug_context.startup_phase = 0;

	if (hid_hotplug_context.startup_ok) {
		for (;;) {
			SInt32 code;
			int stop;

			/* All of the lifecycle state is read under the mutex */
			pthread_mutex_lock(&hid_hotplug_context.mutex);
			stop = (hid_hotplug_context.thread_state == 2);
			pthread_mutex_unlock(&hid_hotplug_context.mutex);

			if (stop) {
				break;
			}

			code = CFRunLoopRunInMode(hid_hotplug_context.run_loop_mode, 1000/*sec*/, FALSE);

			if (code == kCFRunLoopRunTimedOut || code == kCFRunLoopRunHandledSource) {
				continue;
			}

			/* The run loop is gone: either the stop source stopped it
			   (thread_state is already 2), or it exited on its own. Publish the
			   shutdown under the mutex, so that a concurrent registration cannot
			   observe a running thread (thread_state 1) and signal-and-wake a run
			   loop that is winding down; a registration that finds the thread
			   stopped while callbacks are still registered fails instead of
			   attaching to a dead thread. */
			pthread_mutex_lock(&hid_hotplug_context.mutex);
			hid_hotplug_context.thread_state = 2;
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			break;
		}
	}
	/* else: the startup failed - hid_hotplug_register_callback() fails the
	   registration and collects this thread (or lets it detach itself below);
	   the run loop sources (if any got created) and the startup barrier are
	   released by whoever collects it */

	/* Kill the manager. No mutex is needed (and none may be held across
	   IOHIDManagerClose()): nothing else ever touches the manager, and no other
	   thread may start a new event thread or release the run loop mode before
	   this thread has been collected - which cannot happen before the epilogue
	   below, i.e. after the last use of the run loop and of its mode here. */
	if (hid_hotplug_context.manager) {
		if (manager_opened) {
			IOHIDManagerClose(hid_hotplug_context.manager, kIOHIDOptionsTypeNone);
		}

		IOHIDManagerUnscheduleFromRunLoop(hid_hotplug_context.manager, hid_hotplug_context.run_loop, hid_hotplug_context.run_loop_mode);

		CFRelease(hid_hotplug_context.manager);
		hid_hotplug_context.manager = NULL;
	}

	hid_internal_hotplug_thread_epilogue();

	return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_register_callback(unsigned short vendor_id, unsigned short product_id, int events, int flags, hid_hotplug_callback_fn callback, void *user_data, hid_hotplug_callback_handle *callback_handle)
{
	struct hid_hotplug_callback* hotplug_cb;

	/* No events are ever delivered for a failed registration */
	if (callback_handle != NULL) {
		*callback_handle = 0;
	}

	/* Check params */
	if (events == 0
		|| (events & ~(HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT))
		|| (flags & ~(HID_API_HOTPLUG_ENUMERATE))
		|| callback == NULL) {
		register_global_error("hid_hotplug_register_callback: invalid arguments");
		return -1;
	}

	/* Create the hotplug mutex, exactly once. There is deliberately no
	   bootstrap lock around this: any lock ordered outside the hotplug mutex
	   would deadlock against a registration made from within a callback (which
	   already holds the hotplug mutex) - see the locking note above. */
	if (hid_internal_hotplug_init() != 0) {
		register_global_error("hid_hotplug_register_callback: failed to initialize the hotplug mutex");
		return -1;
	}

	/* Lock the mutex to avoid race conditions */
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	if (hid_hotplug_context.exiting) {
		/* hid_exit() is tearing the machinery down: it invalidates every
		   callback handle, so there is nothing to register into */
		register_global_error("hid_hotplug_register_callback: hid_exit() is in progress");
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	/* The registration initializes the library implicitly (as if by hid_init()).
	   Done under the hotplug mutex, together with the `exiting` check above, so
	   that it cannot race hid_exit() destroying hid_mgr (hid_exit() keeps
	   `exiting` set across the whole of its teardown).
	   NOTE: hid_init() schedules the global IOHIDManager on the run loop of the
	   CURRENT thread. When the implicit initialization happens here, that is the
	   registering thread rather than the thread that later calls hid_enumerate()
	   or hid_open() - those pump their own run loop, so they no longer service
	   the manager's run loop source. They do not depend on it (the manager
	   answers IOHIDManagerCopyDevices() synchronously), but an application that
	   wants the classic behavior should call hid_init() explicitly, from the
	   thread it uses HIDAPI on, before registering a hotplug callback. */
	if (!hid_mgr && hid_init() != 0) {
		/* register_global_error: global error is already set by hid_init */
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	hotplug_cb = (struct hid_hotplug_callback*)calloc(1, sizeof(struct hid_hotplug_callback));

	if (hotplug_cb == NULL) {
		register_global_error("hid_hotplug_register_callback: failed to allocate a callback");
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	/* Fill out the record */
	hotplug_cb->next = NULL;
	hotplug_cb->replay = NULL;
	hotplug_cb->vendor_id = vendor_id;
	hotplug_cb->product_id = product_id;
	hotplug_cb->events = events;
	hotplug_cb->user_data = user_data;
	hotplug_cb->callback = callback;

	/* If a stopped event thread has not been collected (joined) yet, collect
	   it before the machinery can be restarted; the join must not happen with
	   the mutex held, so drop the mutex for the collection and re-check.
	   Never entered on the event thread itself: the list cannot be empty
	   while a callback dispatch is in flight. */
	while (hid_hotplug_context.hotplug_cbs == NULL && hid_hotplug_context.thread_needs_join) {
		/* Make sure the stop was actually requested (idempotent) */
		hid_internal_hotplug_cleanup();
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		hid_internal_hotplug_collect_thread();
		pthread_mutex_lock(&hid_hotplug_context.mutex);

		/* hid_exit() may have started while the mutex was released */
		if (hid_hotplug_context.exiting) {
			register_global_error("hid_hotplug_register_callback: hid_exit() is in progress");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			return -1;
		}
	}

	/* A stopped event thread while callbacks are still registered means the
	   thread stopped on its own (an unsolicited run loop exit): a solicited stop
	   is only ever requested once the callback list is empty. The machinery is
	   dead - it can deliver neither the initial pass nor any live event - so the
	   registration must fail rather than silently attach to it.
	   (With no callbacks left, the loop above has already collected the thread
	   and a fresh one is started below.) */
	if (hid_hotplug_context.hotplug_cbs != NULL && hid_hotplug_context.thread_state == 2) {
		register_global_error("hid_hotplug_register_callback: the hotplug event thread has stopped");
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		free(hotplug_cb);
		return -1;
	}

	/* Handles are not recycled even on overflow: recycling could collide with a live handle */
	if (hid_hotplug_context.next_handle == INT_MAX) {
		register_global_error("hid_hotplug_register_callback: out of callback handles");
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		free(hotplug_cb);
		return -1;
	}

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
		if (pthread_barrier_init(&hid_hotplug_context.startup_barrier, NULL, 2) != 0) {
			register_global_error("hid_hotplug_register_callback: failed to create the startup barrier");
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			return -1;
		}

		/* Set up the state the event thread starts from. The thread must not
		   touch the mutex before the startup barrier - this thread holds it and
		   parks at that barrier - so it never writes thread_state itself: it
		   reports its result in startup_ok, which is published here instead. */
		hid_hotplug_context.thread_state = 0;
		hid_hotplug_context.startup_ok = 0;
		hid_hotplug_context.startup_phase = 1;

		if (pthread_create(&hid_hotplug_context.thread, NULL, hotplug_thread, NULL) != 0) {
			register_global_error("hid_hotplug_register_callback: failed to create the hotplug events thread");
			hid_hotplug_context.startup_phase = 0;
			pthread_barrier_destroy(&hid_hotplug_context.startup_barrier);
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			free(hotplug_cb);
			return -1;
		}

		hid_hotplug_context.thread_needs_join = 1;

		/* Wait for the thread to finish setting up - without it the callback may be registered too early*/

		pthread_barrier_wait(&hid_hotplug_context.startup_barrier);

		/* Publish the thread's startup result */
		hid_hotplug_context.thread_state = hid_hotplug_context.startup_ok ? 1 : 2;

		if (!hid_hotplug_context.startup_ok) {
			/* The thread failed to set up the device monitoring and is exiting:
			   it must be collected (joined) with the mutex released */
			register_global_error("hid_hotplug_register_callback: failed to start the device monitoring");

			/* Free whatever the thread may have cached before it failed
			   (the callback list is empty, so this also stops nothing and
			   re-signals nothing: thread_state is already 2) */
			hid_internal_hotplug_cleanup();

			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			hid_internal_hotplug_collect_thread();
			free(hotplug_cb);
			return -1;
		}

		/* Don't forget to actually register the callback */
		hid_hotplug_context.hotplug_cbs = hotplug_cb;
	}

	if ((flags & HID_API_HOTPLUG_ENUMERATE) && (events & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)) {
		int snapshot_ok = 1;
		struct hid_device_info *dev_info = hid_hotplug_context.devs;
		struct hid_device_info **replay_tail = &hotplug_cb->replay;

		/* Take a snapshot of the already connected matching devices: it is
		   delivered ("replayed") as synthetic arrival events on the event
		   thread, never from within this call */
		for (; dev_info != NULL; dev_info = dev_info->next) {
			struct hid_device_info *dev_info_copy;
			if (!hid_internal_match_device_id(dev_info->vendor_id, dev_info->product_id, hotplug_cb->vendor_id, hotplug_cb->product_id)) {
				continue;
			}
			dev_info_copy = hid_internal_copy_device_info(dev_info);
			if (dev_info_copy == NULL) {
				snapshot_ok = 0;
				break;
			}
			*replay_tail = dev_info_copy;
			replay_tail = &dev_info_copy->next;
		}

		if (!snapshot_ok) {
			/* Fail the registration rather than deliver a partial initial pass.
			   The mutex has been held since before the callback became visible,
			   so it has not been invoked yet: it is safe to detach and free it. */
			struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
			while (*current != NULL && *current != hotplug_cb) {
				current = &(*current)->next;
			}
			if (*current != NULL) {
				*current = hotplug_cb->next;
			}
			hid_free_enumeration(hotplug_cb->replay);
			free(hotplug_cb);

			register_global_error("hid_hotplug_register_callback: failed to take a snapshot of the connected devices");

			/* Stop the event thread if no other callbacks are left,
			   and collect it with the mutex released */
			hid_internal_hotplug_cleanup();
			pthread_mutex_unlock(&hid_hotplug_context.mutex);
			hid_internal_hotplug_collect_thread();
			return -1;
		}

		if (hotplug_cb->replay != NULL && hid_hotplug_context.thread_state == 1) {
			/* Ask the event thread to deliver the initial pass */
			CFRunLoopSourceSignal(hid_hotplug_context.replay_source);
			CFRunLoopWakeUp(hid_hotplug_context.run_loop);
		}
	}

	/* Return the allocated handle: written before any events can be delivered,
	   as the events are only ever delivered under this mutex */
	if (callback_handle != NULL) {
		*callback_handle = hotplug_cb->handle;
	}

	/* Clear the stale global error on success, like hid_init()/hid_enumerate() do */
	register_global_error(NULL);

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	return 0;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_deregister_callback(hid_hotplug_callback_handle callback_handle)
{
	int result = -1;

	if (callback_handle <= 0) {
		register_global_error("hid_hotplug_deregister_callback: not a registered callback handle");
		return -1;
	}

	/* The mutex is created here as well: this may be the first hotplug call */
	if (hid_internal_hotplug_init() != 0) {
		register_global_error("hid_hotplug_deregister_callback: failed to initialize the hotplug mutex");
		return -1;
	}

	pthread_mutex_lock(&hid_hotplug_context.mutex);

	if (hid_hotplug_context.exiting) {
		/* hid_exit() is tearing the machinery down and invalidates every handle:
		   deregistering is a no-op */
		register_global_error("hid_hotplug_deregister_callback: hid_exit() is in progress");
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	if (hid_hotplug_context.hotplug_cbs == NULL) {
		register_global_error("hid_hotplug_deregister_callback: no callbacks are registered");
	}
	else {
		/* Remove this notification: the entries already marked for removal are
		   skipped, so that a handle cannot be deregistered a second time */
		for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
			if ((*current)->handle == callback_handle && (*current)->events != 0) {
				/* Free the undelivered initial pass: once deregistered, the callback must never fire */
				hid_free_enumeration((*current)->replay);
				(*current)->replay = NULL;
				/* Check if we were already in a locked state, as we are NOT allowed to remove any callbacks if we are */
				if (hid_hotplug_context.mutex_in_use) {
					(*current)->events = 0;
					hid_hotplug_context.cb_list_dirty = 1;
				} else {
					struct hid_hotplug_callback *next = (*current)->next;
					free(*current);
					*current = next;
				}
				result = 0;
				break;
			}
		}

		if (result != 0) {
			register_global_error("hid_hotplug_deregister_callback: unknown callback handle");
		}

		hid_internal_hotplug_cleanup();
	}

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	/* If this deregistration stopped the event thread, join it with the mutex
	   released. A no-op when called from within a callback (the event thread
	   cannot join itself): the thread then detaches and releases itself in its
	   epilogue - see hid_internal_hotplug_thread_epilogue() */
	hid_internal_hotplug_collect_thread();

	return result;
}

hid_device * HID_API_EXPORT hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* This function is identical to the Linux version. Platform independent. */

	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device * handle = NULL;

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
		handle = hid_open_path(path_to_open);
	} else {
		register_global_error("Device with requested VID/PID/(SerialNumber) not found");
	}

	hid_free_enumeration(devs);

	return handle;
}

static void hid_device_removal_callback(void *context, IOReturn result,
                                        void *sender)
{
	(void) result;
	(void) sender;

	/* Stop the Run Loop for this device. */
	hid_device *d = (hid_device*) context;

	d->disconnected = 1;
	CFRunLoopStop(d->run_loop);
}

/* The Run Loop calls this function for each input report received.
   This function puts the data into a linked list to be picked up by
   hid_read(). */
static void hid_report_callback(void *context, IOReturn result, void *sender,
                         IOHIDReportType report_type, uint32_t report_id,
                         uint8_t *report, CFIndex report_length)
{
	(void) result;
	(void) sender;
	(void) report_type;
	(void) report_id;

	struct input_report *rpt;
	hid_device *dev = (hid_device*) context;

	/* Make a new Input Report object */
	rpt = (struct input_report*) calloc(1, sizeof(struct input_report));
	rpt->data = (uint8_t*) calloc(1, report_length);
	memcpy(rpt->data, report, report_length);
	rpt->len = report_length;
	rpt->next = NULL;

	/* Lock this section */
	pthread_mutex_lock(&dev->mutex);

	/* Attach the new report object to the end of the list. */
	if (dev->input_reports == NULL) {
		/* The list is empty. Put it at the root. */
		dev->input_reports = rpt;
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

	/* Signal a waiting thread that there is data. */
	pthread_cond_signal(&dev->condition);

	/* Unlock */
	pthread_mutex_unlock(&dev->mutex);

}

/* This gets called when the read_thread's run loop gets signaled by
   hid_close(), and serves to stop the read_thread's run loop. */
static void perform_signal_callback(void *context)
{
	hid_device *dev = (hid_device*) context;
	CFRunLoopStop(dev->run_loop); /*TODO: CFRunLoopGetCurrent()*/
}

static void *read_thread(void *param)
{
	hid_device *dev = (hid_device*) param;
	SInt32 code;

	/* Move the device's run loop to this thread. */
	IOHIDDeviceScheduleWithRunLoop(dev->device_handle, CFRunLoopGetCurrent(), dev->run_loop_mode);

	/* Create the RunLoopSource which is used to signal the
	   event loop to stop when hid_close() is called. */
	CFRunLoopSourceContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.version = 0;
	ctx.info = dev;
	ctx.perform = &perform_signal_callback;
	dev->source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0/*order*/, &ctx);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), dev->source, dev->run_loop_mode);

	/* Store off the Run Loop so it can be stopped from hid_close()
	   and on device disconnection. */
	dev->run_loop = CFRunLoopGetCurrent();

	/* Notify the main thread that the read thread is up and running. */
	pthread_barrier_wait(&dev->barrier);

	/* Run the Event Loop. CFRunLoopRunInMode() will dispatch HID input
	   reports into the hid_report_callback(). */
	while (!dev->shutdown_thread && !dev->disconnected) {
		code = CFRunLoopRunInMode(dev->run_loop_mode, 1000/*sec*/, FALSE);
		/* Return if the device has been disconnected */
		if (code == kCFRunLoopRunFinished || code == kCFRunLoopRunStopped) {
			dev->disconnected = 1;
			break;
		}


		/* Break if The Run Loop returns Finished or Stopped. */
		if (code != kCFRunLoopRunTimedOut &&
		    code != kCFRunLoopRunHandledSource) {
			/* There was some kind of error. Setting
			   shutdown seems to make sense, but
			   there may be something else more appropriate */
			dev->shutdown_thread = 1;
			break;
		}
	}

	/* Now that the read thread is stopping, Wake any threads which are
	   waiting on data (in hid_read_timeout()). Do this under a mutex to
	   make sure that a thread which is about to go to sleep waiting on
	   the condition actually will go to sleep before the condition is
	   signaled. */
	pthread_mutex_lock(&dev->mutex);
	pthread_cond_broadcast(&dev->condition);
	pthread_mutex_unlock(&dev->mutex);

	/* Wait here until hid_close() is called and makes it past
	   the call to CFRunLoopWakeUp(). This thread still needs to
	   be valid when that function is called on the other thread. */
	pthread_barrier_wait(&dev->shutdown_barrier);

	return NULL;
}

/* \p path must be one of:
     - in format 'DevSrvsID:<RegistryEntryID>' (as returned by hid_enumerate);
     - a valid path to an IOHIDDevice in the IOService plane (as returned by IORegistryEntryGetPath,
       e.g.: "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/EHC1@1D,7/AppleUSBEHCI/PLAYSTATION(R)3 Controller@fd120000/IOUSBInterface@0/IOUSBHIDDriver");
   Second format is for compatibility with paths accepted by older versions of HIDAPI.
*/
static io_registry_entry_t hid_open_service_registry_from_path(const char *path)
{
	if (path == NULL)
		return MACH_PORT_NULL;

	/* Get the IORegistry entry for the given path */
	if (strncmp("DevSrvsID:", path, 10) == 0) {
		char *endptr;
		uint64_t entry_id = strtoull(path + 10, &endptr, 10);
		if (*endptr == '\0') {
			return IOServiceGetMatchingService((mach_port_t) 0, IORegistryEntryIDMatching(entry_id));
		}
	}
	else {
		/* Fallback to older format of the path */
		return IORegistryEntryFromPath((mach_port_t) 0, path);
	}

	return MACH_PORT_NULL;
}

hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
	hid_device *dev = NULL;
	io_registry_entry_t entry = MACH_PORT_NULL;
	IOReturn ret = kIOReturnInvalid;
	char str[32];

	/* Set up the HID Manager if it hasn't been done */
	if (hid_init() < 0) {
		return NULL;
	}
	/* register_global_error: global error is set/reset by hid_init */

	dev = new_hid_device();
	if (!dev) {
		register_global_error("Couldn't allocate memory");
		return NULL;
	}

	/* Get the IORegistry entry for the given path */
	entry = hid_open_service_registry_from_path(path);
	if (entry == MACH_PORT_NULL) {
		/* Path wasn't valid (maybe device was removed?) */
		register_global_error("hid_open_path: device mach entry not found with the given path");
		goto return_error;
	}

	/* Create an IOHIDDevice for the entry */
	dev->device_handle = IOHIDDeviceCreate(kCFAllocatorDefault, entry);
	if (dev->device_handle == NULL) {
		/* Error creating the HID device */
		register_global_error("hid_open_path: failed to create IOHIDDevice from the mach entry");
		goto return_error;
	}

	/* Open the IOHIDDevice */
	ret = IOHIDDeviceOpen(dev->device_handle, dev->open_options);
	if (ret != kIOReturnSuccess) {
		register_global_error_format("hid_open_path: failed to open IOHIDDevice from mach entry: (0x%08X) %s", ret, mach_error_string(ret));
		goto return_error;
	}

	/* Create the buffers for receiving data */
	dev->max_input_report_len = (CFIndex) get_max_report_length(dev->device_handle);
	dev->input_report_buf = (uint8_t*) calloc(dev->max_input_report_len, sizeof(uint8_t));

	/* Create the Run Loop Mode for this device.
	   printing the reference seems to work. */
	snprintf(str, sizeof(str), "HIDAPI_%p", (void*) dev->device_handle);
	dev->run_loop_mode =
		CFStringCreateWithCString(NULL, str, kCFStringEncodingASCII);

	/* Attach the device to a Run Loop */
	IOHIDDeviceRegisterInputReportCallback(
		dev->device_handle, dev->input_report_buf, dev->max_input_report_len,
		&hid_report_callback, dev);
	IOHIDDeviceRegisterRemovalCallback(dev->device_handle, hid_device_removal_callback, dev);

	/* Start the read thread */
	pthread_create(&dev->thread, NULL, read_thread, dev);

	/* Wait here for the read thread to be initialized. */
	pthread_barrier_wait(&dev->barrier);

	IOObjectRelease(entry);
	return dev;

return_error:
	if (dev->device_handle != NULL)
		CFRelease(dev->device_handle);

	if (entry != MACH_PORT_NULL)
		IOObjectRelease(entry);

	free_hid_device(dev);
	return NULL;
}

static int set_report(hid_device *dev, IOHIDReportType type, const unsigned char *data, size_t length)
{
	const unsigned char *data_to_send = data;
	CFIndex length_to_send = length;
	IOReturn res;
	unsigned char report_id;

	if (!data || (length == 0)) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	report_id = data[0];

	if (report_id == 0x0) {
		/* Not using numbered Reports.
		   Don't send the report number. */
		data_to_send = data+1;
		length_to_send = length-1;
	}

	/* Avoid crash if the device has been unplugged. */
	if (dev->disconnected) {
		register_device_error(dev, "Device is disconnected");
		return -1;
	}

	res = IOHIDDeviceSetReport(dev->device_handle,
	                           type,
	                           report_id,
	                           data_to_send, length_to_send);

	if (res != kIOReturnSuccess) {
		register_device_error_format(dev, "IOHIDDeviceSetReport failed: (0x%08X) %s", res, mach_error_string(res));
		return -1;
	}

	return (int) length;
}

static int get_report(hid_device *dev, IOHIDReportType type, unsigned char *data, size_t length)
{
	unsigned char *report = data;
	CFIndex report_length = length;
	IOReturn res = kIOReturnSuccess;
	unsigned char report_id;

	if (!data || (length == 0)) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	report_id = data[0];

	if (report_id == 0x0) {
		/* Not using numbered Reports.
		   Don't send the report number. */
		report = data+1;
		report_length = length-1;
	}

	/* Avoid crash if the device has been unplugged. */
	if (dev->disconnected) {
		register_device_error(dev, "Device is disconnected");
		return -1;
	}

	res = IOHIDDeviceGetReport(dev->device_handle,
	                           type,
	                           report_id,
	                           report, &report_length);

	if (res != kIOReturnSuccess) {
		register_device_error_format(dev, "IOHIDDeviceGetReport failed: (0x%08X) %s", res, mach_error_string(res));
		return -1;
	}

	if (report_id == 0x0) { /* 0 report number still present at the beginning */
		report_length++;
	}

	return (int) report_length;
}

int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	return set_report(dev, kIOHIDReportTypeOutput, data, length);
}

/* Helper function, so that this isn't duplicated in hid_read(). */
static int return_data(hid_device *dev, unsigned char *data, size_t length)
{
	/* Copy the data out of the linked list item (rpt) into the
	   return buffer (data), and delete the liked list item. */
	struct input_report *rpt = dev->input_reports;
	size_t len = (length < rpt->len)? length: rpt->len;
	if (data != NULL) {
		memcpy(data, rpt->data, len);
	}
	dev->input_reports = rpt->next;
	free(rpt->data);
	free(rpt);
	return (int) len;
}

static int cond_wait(hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	while (!dev->input_reports) {
		int res = pthread_cond_wait(cond, mutex);
		if (res != 0)
			return res;

		/* A res of 0 means we may have been signaled or it may
		   be a spurious wakeup. Check to see that there's actually
		   data in the queue before returning, and if not, go back
		   to sleep. See the pthread_cond_timedwait() man page for
		   details. */

		if (dev->shutdown_thread || dev->disconnected) {
			return -1;
		}
	}

	return 0;
}

static int cond_timedwait(hid_device *dev, pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	while (!dev->input_reports) {
		int res = pthread_cond_timedwait(cond, mutex, abstime);
		if (res != 0)
			return res;

		/* A res of 0 means we may have been signaled or it may
		   be a spurious wakeup. Check to see that there's actually
		   data in the queue before returning, and if not, go back
		   to sleep. See the pthread_cond_timedwait() man page for
		   details. */

		if (dev->shutdown_thread || dev->disconnected) {
			return -1;
		}
	}

	return 0;
}

int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	int bytes_read = -1;

	if (!data || (length == 0)) {
		register_error_str(&dev->last_read_error_str, "Zero buffer/length");
		return -1;
	}

	register_error_str(&dev->last_read_error_str, NULL);

	/* Lock the access to the report list. */
	pthread_mutex_lock(&dev->mutex);

	/* There's an input report queued up. Return it. */
	if (dev->input_reports) {
		/* Return the first one */
		bytes_read = return_data(dev, data, length);
		goto ret;
	}

	/* Return if the device has been disconnected. */
	if (dev->disconnected) {
		bytes_read = -1;
		register_error_str(&dev->last_read_error_str, "hid_read_timeout: device disconnected");
		goto ret;
	}

	if (dev->shutdown_thread) {
		/* This means the device has been closed (or there
		   has been an error. An error code of -1 should
		   be returned. */
		bytes_read = -1;
		register_error_str(&dev->last_read_error_str, "hid_read_timeout: thread shutdown");
		goto ret;
	}

	/* There is no data. Go to sleep and wait for data. */

	if (milliseconds == -1) {
		/* Blocking */
		int res;
		res = cond_wait(dev, &dev->condition, &dev->mutex);
		if (res == 0)
			bytes_read = return_data(dev, data, length);
		else {
			/* There was an error, or a device disconnection. */
			register_error_str(&dev->last_read_error_str, "hid_read_timeout: error waiting for more data");
			bytes_read = -1;
		}
	}
	else if (milliseconds > 0) {
		/* Non-blocking, but called with timeout. */
		int res;
		struct timespec ts;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		TIMEVAL_TO_TIMESPEC(&tv, &ts);
		ts.tv_sec += milliseconds / 1000;
		ts.tv_nsec += (milliseconds % 1000) * 1000000;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}

		res = cond_timedwait(dev, &dev->condition, &dev->mutex, &ts);
		if (res == 0) {
			bytes_read = return_data(dev, data, length);
		} else if (res == ETIMEDOUT) {
			bytes_read = 0;
		} else {
			register_error_str(&dev->last_read_error_str, "hid_read_timeout: error waiting for more data");
			bytes_read = -1;
		}
	}
	else {
		/* Purely non-blocking */
		bytes_read = 0;
	}

ret:
	/* Unlock */
	pthread_mutex_unlock(&dev->mutex);
	return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

HID_API_EXPORT const wchar_t * HID_API_CALL hid_read_error(hid_device *dev)
{
	if (dev->last_read_error_str == NULL)
		return L"Success";
	return dev->last_read_error_str;
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
	/* All Nonblocking operation is handled by the library. */
	dev->blocking = !nonblock;

	return 0;
}

int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	return set_report(dev, kIOHIDReportTypeFeature, data, length);
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	return get_report(dev, kIOHIDReportTypeFeature, data, length);
}

int HID_API_EXPORT hid_send_output_report(hid_device *dev, const unsigned char *data, size_t length)
{
	return set_report(dev, kIOHIDReportTypeOutput, data, length);
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	return get_report(dev, kIOHIDReportTypeInput, data, length);
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	/* Disconnect the report callback before close.
	   See comment below.
	*/
	if (is_macos_10_10_or_greater || !dev->disconnected) {
		IOHIDDeviceRegisterInputReportCallback(
			dev->device_handle, dev->input_report_buf, dev->max_input_report_len,
			NULL, dev);
		IOHIDDeviceRegisterRemovalCallback(dev->device_handle, NULL, dev);
		IOHIDDeviceUnscheduleFromRunLoop(dev->device_handle, dev->run_loop, dev->run_loop_mode);
		IOHIDDeviceScheduleWithRunLoop(dev->device_handle, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
	}

	/* Cause read_thread() to stop. */
	dev->shutdown_thread = 1;

	/* Wake up the run thread's event loop so that the thread can exit. */
	CFRunLoopSourceSignal(dev->source);
	CFRunLoopWakeUp(dev->run_loop);

	/* Notify the read thread that it can shut down now. */
	pthread_barrier_wait(&dev->shutdown_barrier);

	/* Wait for read_thread() to end. */
	pthread_join(dev->thread, NULL);

	/* Close the OS handle to the device, but only if it's not
	   been unplugged. If it's been unplugged, then calling
	   IOHIDDeviceClose() will crash.

	   UPD: The crash part was true in/until some version of macOS.
	   Starting with macOS 10.15, there is an opposite effect in some environments:
	   crash happenes if IOHIDDeviceClose() is not called.
	   Not leaking a resource in all tested environments.
	*/
	if (is_macos_10_10_or_greater || !dev->disconnected) {
		IOHIDDeviceClose(dev->device_handle, dev->open_options);
	}

	/* Clear out the queue of received reports. */
	pthread_mutex_lock(&dev->mutex);
	while (dev->input_reports) {
		return_data(dev, NULL, 0);
	}
	pthread_mutex_unlock(&dev->mutex);
	CFRelease(dev->device_handle);

	free_hid_device(dev);
}

int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen)
	{
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info)
	{
		// hid_get_device_info will have set an error already
		return -1;
	}

	wcsncpy(string, info->manufacturer_string, maxlen);
	string[maxlen - 1] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	wcsncpy(string, info->product_string, maxlen);
	string[maxlen - 1] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	wcsncpy(string, info->serial_number, maxlen);
	string[maxlen - 1] = L'\0';

	return 0;
}

HID_API_EXPORT struct hid_device_info *HID_API_CALL hid_get_device_info(hid_device *dev) {
	if (dev->device_info) {
		register_device_error(dev, NULL);
	}
	else {
		dev->device_info = create_device_info(dev->device_handle);
		if (!dev->device_info) {
			register_device_error(dev, "Failed to create hid_device_info");
		}
	}

	return dev->device_info;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	(void) dev;
	(void) string_index;
	(void) string;
	(void) maxlen;

	register_device_error(dev, "hid_get_indexed_string: not available on this platform");
	return -1;
}

int HID_API_EXPORT_CALL hid_darwin_get_location_id(hid_device *dev, uint32_t *location_id)
{
	if (!location_id) {
		register_device_error(dev, "Location ID is NULL");
		return -1;
	}

	register_device_error(dev, NULL);

	int res = get_int_property(dev->device_handle, CFSTR(kIOHIDLocationIDKey));
	if (res != 0) {
		*location_id = (uint32_t) res;
		return 0;
	} else {
		register_device_error(dev, "Failed to get IOHIDLocationID property");
		return -1;
	}
}

void HID_API_EXPORT_CALL hid_darwin_set_open_exclusive(int open_exclusive)
{
	device_open_options = (open_exclusive == 0) ? kIOHIDOptionsTypeNone : kIOHIDOptionsTypeSeizeDevice;
}

int HID_API_EXPORT_CALL hid_darwin_get_open_exclusive(void)
{
	return (device_open_options == kIOHIDOptionsTypeSeizeDevice) ? 1 : 0;
}

int HID_API_EXPORT_CALL hid_darwin_is_device_open_exclusive(hid_device *dev)
{
	return (dev->open_options == kIOHIDOptionsTypeSeizeDevice) ? 1 : 0;
}

int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device *dev, unsigned char *buf, size_t buf_size)
{
	if (!buf || !buf_size) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	register_device_error(dev, NULL);

	CFTypeRef ref = IOHIDDeviceGetProperty(dev->device_handle, CFSTR(kIOHIDReportDescriptorKey));
	if (ref != NULL && CFGetTypeID(ref) == CFDataGetTypeID()) {
		CFDataRef report_descriptor = (CFDataRef) ref;
		const UInt8 *descriptor_buf = CFDataGetBytePtr(report_descriptor);
		const CFIndex descriptor_buf_len = CFDataGetLength(report_descriptor);
		size_t copy_len = (size_t) descriptor_buf_len;

		if (descriptor_buf == NULL || descriptor_buf_len < 0) {
			register_device_error(dev, "Zero descriptor from device");
			return -1;
		}

		if (buf_size < copy_len) {
			copy_len = buf_size;
		}

		memcpy(buf, descriptor_buf, copy_len);
		return copy_len;
	}
	else {
		register_device_error(dev, "Failed to get kIOHIDReportDescriptorKey property");
		return -1;
	}
}

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
