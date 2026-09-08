/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Tier-2 hotplug tests, run against a virtual HID device whose
 presence can be toggled (test_virtual_device_unplug/_replug):
 asynchronous delivery, the exactly-once ENUMERATE pass,
 callback-return deregistration, pass-before-live ordering,
 ARRIVED/LEFT payloads, VID/PID filtering, dispatch order,
 deregistration post-conditions and re-entrant (in-callback)
 registration.

 Synchronization discipline (hotplug tests are notoriously
 flaky when built on sleeps):
   - recording callbacks only lock, deep-copy the event into a log,
     unlock and return; they never call hid_enumerate/hid_open/
     hid_error(NULL);
   - dedicated scenarios hold a callback on a bounded gate or call the
     permitted re-entrant hotplug register/deregister APIs;
   - every expectation is awaited with a deadline-based
     predicate poll (hp_wait_*), never a bare sleep;
   - ABSENCE of an event is asserted behind an event barrier
     (a later event that is provably ordered after the missing
     one), never behind a time window.

 All assertions filter on the test's own VID/PID/serial: real
 devices may be present on the host and may generate events
 concurrently.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hidapi.h>

#include "test_virtual_device.h"
#include "test_platform.h"

/* CTest treats this exit code as "skipped" (see SKIP_RETURN_CODE in CMake). */
#define EXIT_SKIP 77

/* Test-unique ids so enumeration/filtering cannot collide with real hardware.
   On Linux/macOS the device is created on demand, so the primary uses a PID
   distinct from test_device_io.c's 0x9001. On Windows the virtual device is a
   single pre-installed static driver (src/tests/windows/driver) whose identity
   is fixed, so the primary must match it (PID 0x9001, serial == the driver's
   VHIDMINI_SERIAL_NUMBER_STRING); the second device has no counterpart there and
   is reported UNAVAILABLE by the Windows provider. */
#define TEST_VID      0xF1D0
#if defined(_WIN32)
#define TEST_PID      0x9001   /* the static vhidmini driver's HIDMINI_PID */
#else
#define TEST_PID      0x9002
#endif
#define TEST_PID_2    0x9003   /* second device, for the mid-pass stop test */
#define TEST_SERIAL   "HIDAPI-HOTPLUG-TEST"
#define TEST_SERIAL_2 "HIDAPI-HOTPLUG-TEST-2"

#define ALL_EVENTS (HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT)

/* Budget for one awaited event/predicate. The uhid provider is fast (10s is
   generous); the rawgadget/win providers go through a full (virtual)
   USB stack, so their CMake target overrides this with 30s. */
#ifndef TEST_HOTPLUG_EVENT_TIMEOUT_MS
#define TEST_HOTPLUG_EVENT_TIMEOUT_MS 30000
#endif
#define EVENT_TIMEOUT_MS TEST_HOTPLUG_EVENT_TIMEOUT_MS

#define WAIT_TICK_MS 10

static int g_failures = 0;
static int g_skipped = 0;
static test_atomic_int g_deadline_failed;

static void hp_cleanup_callbacks(void);

#define CHECK(cond)                                                       \
	do {                                                              \
		if (!(cond)) {                                            \
			printf("    CHECK failed: %s (line %d)\n",       \
			       #cond, __LINE__);                         \
			fflush(stdout);                                  \
			g_failures++;                                    \
			hp_cleanup_callbacks();                          \
			return -1;                                       \
		}                                                        \
	} while (0)

/* Print a flushed progress marker so a hang is localised on a CTest timeout. */
static void step(const char *what)
{
	printf("    -> %s\n", what);
	fflush(stdout);
}

static void report(const char *name, int rc)
{
	hp_cleanup_callbacks();
	if (rc == EXIT_SKIP)
		g_skipped++;
	printf("%s %s\n", rc == EXIT_SKIP ? "SKIP" : (rc == 0 ? "PASS" : "FAIL"), name);
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* The event log. One global, ordered log shared by every callback:    */
/* cross-callback ordering assertions (dispatch order, barriers) fall  */
/* out of the log order itself.                                        */

#define HP_MAX_EVENTS 128
#define HP_PATH_MAX   256
#define HP_SERIAL_MAX 64

typedef struct hp_event {
	int seq;                              /* global arrival order */
	hid_hotplug_callback_handle handle;   /* the callback_handle parameter */
	hid_hotplug_event event;
	unsigned short vendor_id;
	unsigned short product_id;
	char path[HP_PATH_MAX];
	char serial[HP_SERIAL_MAX];           /* narrowed; "" when NULL */
	wchar_t serial_full[HP_SERIAL_MAX];
	wchar_t manufacturer[HP_PATH_MAX];
	wchar_t product[HP_PATH_MAX];
	int path_was_null, serial_was_null, manufacturer_was_null, product_was_null;
	unsigned short release_number, usage_page, usage;
	int interface_number;
	hid_bus_type bus_type;
	int device_was_null, event_valid, string_truncated;
	unsigned long long thread_id;         /* thread the callback ran on */
	int next_was_null;                    /* device->next == NULL held */
} hp_event;

static test_mutex g_log_lock;
static hp_event g_events[HP_MAX_EVENTS];
static int g_event_count;
static int g_event_overflow;
static int g_seq_counter;
static unsigned long long g_main_tid;

/* Include registrations made from callbacks; drain before their contexts
   leave scope on a failed CHECK, and before starting another scenario. */
static hid_hotplug_callback_handle g_handles[256];
static int g_handle_count;
static hid_hotplug_callback_handle g_retired[128];
static int g_retired_count;
static int g_late_callback;

static void hp_mark_retired(hid_hotplug_callback_handle handle)
{
	test_mutex_lock(&g_log_lock);
	if (g_retired_count < (int)(sizeof(g_retired) / sizeof(g_retired[0])))
		g_retired[g_retired_count++] = handle;
	else
		g_event_overflow = 1;
	test_mutex_unlock(&g_log_lock);
}

static int hp_register(unsigned short vid, unsigned short pid, int events,
                        int flags, hid_hotplug_callback_fn callback,
                        void *user_data, hid_hotplug_callback_handle *handle)
{
	int rc = hid_hotplug_register_callback(vid, pid, events, flags,
	                                       callback, user_data, handle);
	if (rc == 0) {
		test_mutex_lock(&g_log_lock);
		if (g_handle_count == (int)(sizeof(g_handles) / sizeof(g_handles[0]))) {
			fprintf(stderr, "callback cleanup list overflow\n");
			fflush(stderr);
			_Exit(EXIT_FAILURE);
		}
		g_handles[g_handle_count++] = *handle;
		test_mutex_unlock(&g_log_lock);
	}
	return rc;
}

static void hp_cleanup_callbacks(void)
{
	for (;;) {
		hid_hotplug_callback_handle handle;
		test_mutex_lock(&g_log_lock);
		if (!g_handle_count) {
			test_mutex_unlock(&g_log_lock);
			return;
		}
		handle = g_handles[--g_handle_count];
		test_mutex_unlock(&g_log_lock);
		/* A non-zero callback return or explicit teardown may have removed it. */
		(void)hid_hotplug_deregister_callback(handle);
	}
}

static void hp_copy_wide(wchar_t *out, size_t capacity, const wchar_t *in,
                          int *truncated)
{
	if (in) {
		size_t n = wcslen(in);
		if (n >= capacity) {
			*truncated = 1;
			n = capacity - 1;
		}
		memcpy(out, in, n * sizeof(*out));
		out[n] = L'\0';
	}
}

/* Non-empty environment variable check. MSVC's /W4 /WX flags getenv() as
   deprecated (C4996), so use the Win32 API there. */
static int hp_env_set(const char *name)
{
#ifdef _WIN32
	return GetEnvironmentVariableA(name, NULL, 0) != 0;
#else
	const char *v = getenv(name);
	return v != NULL && v[0] != '\0';
#endif
}

/* Deep-copy the fields the assertions need. Called from the callbacks, with
   g_log_lock held for the shortest possible time; the device pointer is only
   valid for the duration of the callback. */
static void hp_record(hid_hotplug_callback_handle handle,
                      struct hid_device_info *device,
                      hid_hotplug_event event)
{
	int i;
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_retired_count; i++)
		if (g_retired[i] == handle)
			g_late_callback = 1;
	if (g_event_count < HP_MAX_EVENTS) {
		hp_event *e = &g_events[g_event_count++];
		memset(e, 0, sizeof(*e));
		e->seq = g_seq_counter++;
		e->handle = handle;
		e->event = event;
		e->device_was_null = (device == NULL);
		e->event_valid = (event == HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED
		                  || event == HID_API_HOTPLUG_EVENT_DEVICE_LEFT);
		e->thread_id = test_thread_id();
		if (device) {
			e->vendor_id = device->vendor_id;
			e->product_id = device->product_id;
			e->next_was_null = (device->next == NULL);
			e->release_number = device->release_number;
			e->usage_page = device->usage_page;
			e->usage = device->usage;
			e->interface_number = device->interface_number;
			e->bus_type = device->bus_type;
			e->path_was_null = (device->path == NULL);
			e->serial_was_null = (device->serial_number == NULL);
			e->manufacturer_was_null = (device->manufacturer_string == NULL);
			e->product_was_null = (device->product_string == NULL);
			hp_copy_wide(e->serial_full, HP_SERIAL_MAX, device->serial_number, &e->string_truncated);
			hp_copy_wide(e->manufacturer, HP_PATH_MAX, device->manufacturer_string, &e->string_truncated);
			hp_copy_wide(e->product, HP_PATH_MAX, device->product_string, &e->string_truncated);
			if (device->path) {
				if (strlen(device->path) >= sizeof(e->path))
					e->string_truncated = 1;
				snprintf(e->path, sizeof(e->path), "%s", device->path);
			}
			if (device->serial_number) {
				size_t k;
				for (k = 0; k + 1 < sizeof(e->serial) && device->serial_number[k]; k++) {
					wchar_t wc = device->serial_number[k];
					e->serial[k] = (wc > 0 && wc < 128) ? (char)wc : '?';
				}
				e->serial[k] = '\0';
			}
		}
	} else {
		g_event_overflow = 1;
	}
	test_mutex_unlock(&g_log_lock);
}

/* Does a logged event match? 0 acts as a wildcard for handle/event/pid;
   NULL for serial. A non-zero pid additionally requires the test VID. */
static int hp_match(const hp_event *e, hid_hotplug_callback_handle handle,
                    int event_mask, unsigned short pid, const char *serial)
{
	if (handle != 0 && e->handle != handle)
		return 0;
	if (event_mask != 0 && !(e->event & event_mask))
		return 0;
	if (pid != 0 && (e->vendor_id != TEST_VID || e->product_id != pid))
		return 0;
	if (serial != NULL && strcmp(e->serial, serial) != 0)
		return 0;
	return 1;
}

static int hp_count(hid_hotplug_callback_handle handle, int event_mask,
                    unsigned short pid, const char *serial)
{
	int i, n = 0;
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_event_count; i++)
		if (hp_match(&g_events[i], handle, event_mask, pid, serial))
			n++;
	test_mutex_unlock(&g_log_lock);
	return n;
}

/* Copy the first matching event out of the log. Returns 0 when found. */
static int hp_find_first(hp_event *out, hid_hotplug_callback_handle handle,
                         int event_mask, unsigned short pid, const char *serial)
{
	int i, found = -1;
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_event_count; i++) {
		if (hp_match(&g_events[i], handle, event_mask, pid, serial)) {
			*out = g_events[i];
			found = 0;
			break;
		}
	}
	test_mutex_unlock(&g_log_lock);
	return found;
}

/* Deadline-based predicate poll: the ONLY way the tests wait. */
static int hp_wait_count_at_least(hid_hotplug_callback_handle handle,
                                  int event_mask, unsigned short pid,
                                  const char *serial, int min_count,
                                  int timeout_ms)
{
	long long deadline = test_now_ms() + timeout_ms;
	for (;;) {
		if (hp_count(handle, event_mask, pid, serial) >= min_count)
			return 0;
		if (test_now_ms() >= deadline) {
			test_atomic_store(&g_deadline_failed, 1);
			return -1;
		}
		test_sleep_ms(WAIT_TICK_MS);
	}
}

/* Wait for *flag (read under the log lock) to become non-zero. */
static int hp_wait_flag(const int *flag, int timeout_ms)
{
	long long deadline = test_now_ms() + timeout_ms;
	for (;;) {
		int set;
		test_mutex_lock(&g_log_lock);
		set = *flag;
		test_mutex_unlock(&g_log_lock);
		if (set)
			return 0;
		if (test_now_ms() >= deadline) {
			test_atomic_store(&g_deadline_failed, 1);
			return -1;
		}
		test_sleep_ms(WAIT_TICK_MS);
	}
}

/* Drain callbacks before sweeping every event's payload/thread invariants
   and resetting the log. Test-device strings must fit the log's buffers. */
static void hp_reset_log(const char *test_name)
{
	int i;
	hp_cleanup_callbacks();
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_event_count; i++) {
		if (g_events[i].thread_id == g_main_tid) {
			printf("    INVARIANT failed before %s: an event was "
			       "delivered on the application's main thread\n", test_name);
			fflush(stdout);
			g_failures++;
			break;
		}
		if (g_events[i].device_was_null || !g_events[i].event_valid
		    || !g_events[i].next_was_null
		    || (g_events[i].vendor_id == TEST_VID
		        && (g_events[i].product_id == TEST_PID || g_events[i].product_id == TEST_PID_2)
		        && g_events[i].string_truncated)) {
			printf("    INVARIANT failed before %s: invalid or truncated event payload\n", test_name);
			fflush(stdout);
			g_failures++;
		}
	}
	if (g_event_overflow) {
		printf("    INVARIANT failed before %s: event log overflow\n", test_name);
		fflush(stdout);
		g_failures++;
	}
	if (g_late_callback) {
		printf("    INVARIANT failed before %s: callback after deregistration returned\n", test_name);
		fflush(stdout);
		g_failures++;
	}
	g_event_count = 0;
	g_event_overflow = 0;
	g_retired_count = 0;
	g_late_callback = 0;
	test_mutex_unlock(&g_log_lock);
}

/* ------------------------------------------------------------------ */
/* Recording callbacks only copy into the log; dedicated callbacks     */
/* below deliberately hold a gate or exercise re-entrant hotplug calls. */

/* Plain recorder. */
static int HID_API_CALL cb_log(hid_hotplug_callback_handle callback_handle,
                               struct hid_device_info *device,
                               hid_hotplug_event event, void *user_data)
{
	(void)user_data;
	hp_record(callback_handle, device, event);
	return 0;
}

/* Recorder that asks to be deregistered (returns the supplied non-zero value) on the first event for
   the test's primary device. */
static int HID_API_CALL cb_return_on_ours(hid_hotplug_callback_handle callback_handle,
                                           struct hid_device_info *device,
                                           hid_hotplug_event event, void *user_data)
{
	hp_record(callback_handle, device, event);
	if (device && device->vendor_id == TEST_VID && device->product_id == TEST_PID)
		return *(int *)user_data;
	return 0;
}

/* Recorder that asks to be deregistered on its very first event, whichever
   device it is for (the ENUMERATE snapshot order is unspecified). */
static int HID_API_CALL cb_return_first(hid_hotplug_callback_handle callback_handle,
                                         struct hid_device_info *device,
                                         hid_hotplug_event event, void *user_data)
{
	hp_record(callback_handle, device, event);
	return *(int *)user_data;
}

/* T14: signals "entered", waits on a bounded release gate, then signals
   "exited". Lets the main thread observe that deregistration blocks until an
   in-progress invocation has completed. The context is heap-allocated and
   freed right after deregistration returns: if the backend ever invoked the
   callback again, ASan would flag the use-after-free below. */
typedef struct slow_ctx {
	int entered;
	int exited;
	int release;
	int expired;
	int exit_seq;
} slow_ctx;

static int HID_API_CALL cb_slow(hid_hotplug_callback_handle callback_handle,
                                struct hid_device_info *device,
                                hid_hotplug_event event, void *user_data)
{
	slow_ctx *ctx = (slow_ctx *)user_data;
	hp_record(callback_handle, device, event);
	test_mutex_lock(&g_log_lock);
	ctx->entered = 1;
	test_mutex_unlock(&g_log_lock);
	if (hp_wait_flag(&ctx->release, EVENT_TIMEOUT_MS) != 0) {
		test_mutex_lock(&g_log_lock);
		ctx->expired = 1;
		test_mutex_unlock(&g_log_lock);
	}
	test_mutex_lock(&g_log_lock);
	ctx->exited = 1;
	ctx->exit_seq = g_seq_counter++;
	test_mutex_unlock(&g_log_lock);
	return 0;
}

/* T15: on the first ARRIVED for the primary device, registers a child
   callback WITH ENUMERATE and deregisters itself - both from within the
   callback (the hotplug API is documented re-entrant). */
typedef struct parent_ctx {
	int acted;   /* run-once guard, taken by the first qualifying ARRIVED */
	int done;    /* published LAST, after the results below are stored */
	int child_rc;
	hid_hotplug_callback_handle child_handle;
	int self_dereg_rc;
	int invalid_dereg_rc;
	int parent_active;
	int child_nested;
} parent_ctx;

static int HID_API_CALL cb_child(hid_hotplug_callback_handle handle,
                                 struct hid_device_info *device,
                                 hid_hotplug_event event, void *user_data)
{
	parent_ctx *ctx = (parent_ctx *)user_data;
	test_mutex_lock(&g_log_lock);
	if (ctx->parent_active)
		ctx->child_nested = 1;
	test_mutex_unlock(&g_log_lock);
	hp_record(handle, device, event);
	return 0;
}

static int HID_API_CALL cb_parent(hid_hotplug_callback_handle callback_handle,
                                  struct hid_device_info *device,
                                  hid_hotplug_event event, void *user_data)
{
	parent_ctx *ctx = (parent_ctx *)user_data;
	int act = 0;

	test_mutex_lock(&g_log_lock);
	ctx->parent_active = 1;
	test_mutex_unlock(&g_log_lock);
	hp_record(callback_handle, device, event);

	if (event == HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED
	    && device && device->vendor_id == TEST_VID && device->product_id == TEST_PID) {
		test_mutex_lock(&g_log_lock);
		if (!ctx->acted) {
			ctx->acted = 1;
			act = 1;
		}
		test_mutex_unlock(&g_log_lock);
	}

	if (act) {
		hid_hotplug_callback_handle child = 0;
		int rc = hp_register(TEST_VID, TEST_PID, ALL_EVENTS,
		                                       HID_API_HOTPLUG_ENUMERATE,
		                                       cb_child, ctx, &child);
		int dereg_rc = hid_hotplug_deregister_callback(callback_handle);
		int invalid_rc = hid_hotplug_deregister_callback(0);
		/* Store the results and only then publish 'done', in one locked
		   section: the main thread waits on 'done', so it can never observe
		   the results half-written. */
		test_mutex_lock(&g_log_lock);
		ctx->child_rc = rc;
		ctx->child_handle = child;
		ctx->self_dereg_rc = dereg_rc;
		ctx->invalid_dereg_rc = invalid_rc;
		ctx->parent_active = 0;
		ctx->done = 1;
		test_mutex_unlock(&g_log_lock);
	} else {
		test_mutex_lock(&g_log_lock);
		ctx->parent_active = 0;
		test_mutex_unlock(&g_log_lock);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Device-presence plumbing                                            */

static test_virtual_device *g_vdev;   /* primary device (TEST_PID) */

/* One hid_enumerate() pass: is a device with this pid+serial visible?
   Only ever called from the main thread (HIDAPI's general thread-safety
   rule), and never from inside a callback. */
static int hp_enumerated_now(unsigned short pid, const char *serial)
{
	struct hid_device_info *devs = hid_enumerate(TEST_VID, pid);
	struct hid_device_info *cur;
	int found = 0;
	for (cur = devs; cur; cur = cur->next) {
		size_t i;
		char narrow[HP_SERIAL_MAX] = "";
		if (!cur->serial_number)
			continue;
		for (i = 0; i + 1 < sizeof(narrow) && cur->serial_number[i]; i++) {
			wchar_t wc = cur->serial_number[i];
			narrow[i] = (wc > 0 && wc < 128) ? (char)wc : '?';
		}
		narrow[i] = '\0';
		if (strcmp(narrow, serial) == 0) {
			found = 1;
			break;
		}
	}
	hid_free_enumeration(devs);
	return found;
}

/* Readiness barrier: poll enumeration until the device is (not) visible. */
static int hp_wait_enumerated(unsigned short pid, const char *serial,
                              int present, int timeout_ms)
{
	long long deadline = test_now_ms() + timeout_ms;
	for (;;) {
		if (hp_enumerated_now(pid, serial) == present)
			return 0;
		if (test_now_ms() >= deadline) {
			test_atomic_store(&g_deadline_failed, 1);
			return -1;
		}
		test_sleep_ms(50);
	}
}

/* Establish a known device state at the start of a test, whatever a previous
   (possibly failed) test left behind. */
static int ensure_present(void)
{
	if (!hp_enumerated_now(TEST_PID, TEST_SERIAL))
		(void)test_virtual_device_replug(g_vdev);
	return hp_wait_enumerated(TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS);
}

static int ensure_absent(void)
{
	if (hp_enumerated_now(TEST_PID, TEST_SERIAL))
		(void)test_virtual_device_unplug(g_vdev);
	return hp_wait_enumerated(TEST_PID, TEST_SERIAL, 0, EVENT_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* T6: events are delivered asynchronously (never on the registering   */
/* thread) and the callback receives the same handle that              */
/* hp_register() wrote to *callback_handle.          */
typedef struct publication_ctx {
	hid_hotplug_callback_handle *out_handle;
	int published;
} publication_ctx;

static int HID_API_CALL cb_publication(hid_hotplug_callback_handle handle,
                                       struct hid_device_info *device,
                                       hid_hotplug_event event, void *user_data)
{
	publication_ctx *ctx = (publication_ctx *)user_data;
	/* *out_handle is the registering thread's local written by
	   hid_hotplug_register_callback(). Reading it here without application
	   synchronization is legitimate only because the contract requires that
	   write to be ordered before any event can be delivered; that ordering
	   is exactly what T6 checks. */
	int published = (*ctx->out_handle == handle);
	test_mutex_lock(&g_log_lock);
	ctx->published = published;
	test_mutex_unlock(&g_log_lock);
	hp_record(handle, device, event);
	return 0;
}

static int t6_async_delivery(void)
{
	hid_hotplug_callback_handle h = 0;
	hp_event ev;
	publication_ctx ctx = { &h, 0 };
	int published;

	CHECK(ensure_present() == 0);
	hp_reset_log("T6");

	step("register with ENUMERATE while the device is present");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS,
	                                    HID_API_HOTPLUG_ENUMERATE,
	                                    cb_publication, &ctx, &h) == 0);
	CHECK(h > 0);

	step("wait for the synthetic ARRIVED");
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	CHECK(hp_find_first(&ev, h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(ev.thread_id != g_main_tid);   /* asynchronous delivery */
	CHECK(ev.handle == h);               /* handle parameter == *callback_handle */
	test_mutex_lock(&g_log_lock);
	published = ctx.published;
	test_mutex_unlock(&g_log_lock);
	CHECK(published);

	CHECK(hid_hotplug_deregister_callback(h) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T7: each connection is reported exactly once - by the ENUMERATE     */
/* pass or as a live event, never both. The LEFT of a subsequent       */
/* unplug is the barrier proving no duplicate ARRIVED was in flight.   */
static int t7_exactly_once(void)
{
	hid_hotplug_callback_handle h = 0;

	CHECK(ensure_present() == 0);
	hp_reset_log("T7");

	step("register with ENUMERATE while the device is present");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS,
	                                    HID_API_HOTPLUG_ENUMERATE,
	                                    cb_log, NULL, &h) == 0);

	step("wait for the synthetic ARRIVED");
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("unplug; the LEFT is the exactly-once barrier");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);

	step("replug: the reconnection is one more ARRIVED");
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 2, EVENT_TIMEOUT_MS) == 0);

	step("unplug again (barrier for the second ARRIVED)");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 2, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 2);

	CHECK(hid_hotplug_deregister_callback(h) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T8a: a non-zero callback return value deregisters the callback: the */
/* handle is dead (-1) and no further events reach it. The barrier is  */
/* a second, still-registered callback observing a later event the     */
/* first one must not see.                                             */
static int t8a_return_deregisters(int result)
{
	hid_hotplug_callback_handle h_ret = 0, h_bar = 0;

	CHECK(ensure_present() == 0);
	hp_reset_log("T8a");

	step("register the non-zero-return callback and a barrier callback");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_return_on_ours, &result, &h_ret) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_bar) == 0);

	step("unplug: both callbacks see the LEFT; the first returns non-zero");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_ret, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_wait_count_at_least(h_bar, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("replug: only the barrier callback may see the ARRIVED");
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_bar, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	CHECK(hp_count(h_ret, 0, 0, NULL) == 1); /* exactly the one LEFT */
	step("the handle was already freed by the non-zero return");
	CHECK(hid_hotplug_deregister_callback(h_ret) == -1);

	CHECK(hid_hotplug_deregister_callback(h_bar) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T8b: a non-zero return during the ENUMERATE pass stops the          */
/* remainder of the pass: with TWO matching devices present, the       */
/* callback is invoked exactly once. A later ENUMERATE registration    */
/* observing both devices is the barrier.                              */
static int t8b_return_stops_pass(int result)
{
	test_virtual_device *vdev2 = NULL;
	hid_hotplug_callback_handle h_once = 0, h_probe = 0;
	int rc;

	CHECK(ensure_present() == 0);

	step("create the second device");
	rc = test_virtual_device_create(&vdev2, TEST_VID, TEST_PID_2, TEST_SERIAL_2);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		/* Some providers (raw-gadget: a single dummy_udc.0) can only expose one
		   device at a time. This sub-test needs two concurrent devices, so skip
		   it here rather than failing -- it is not counted as a failure. */
		printf("    T8b needs a second concurrent device, unavailable on this "
		       "provider - skipping this sub-test\n");
		fflush(stdout);
		return EXIT_SKIP;
	}
	CHECK(rc == TEST_VDEV_OK && vdev2 != NULL);
	if (hp_wait_enumerated(TEST_PID_2, TEST_SERIAL_2, 1, EVENT_TIMEOUT_MS) != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"second device did not enumerate");
	}

	hp_reset_log("T8b");

	step("register a non-zero-return callback with ENUMERATE (both devices match)");
	rc = hp_register(TEST_VID, 0, ALL_EVENTS,
	                                   HID_API_HOTPLUG_ENUMERATE,
	                                   cb_return_first, &result, &h_once);
	if (rc != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"registration failed");
	}

	step("wait for its single snapshot event");
	if (hp_wait_count_at_least(h_once, 0, 0, NULL, 1, EVENT_TIMEOUT_MS) != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"the non-zero-return callback never fired");
	}

	step("barrier: a fresh ENUMERATE registration sees both devices");
	rc = hp_register(TEST_VID, 0, ALL_EVENTS,
	                                   HID_API_HOTPLUG_ENUMERATE,
	                                   cb_log, NULL, &h_probe);
	if (rc != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"barrier registration failed");
	}
	rc = 0;
	if (hp_wait_count_at_least(h_probe, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                           TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) != 0)
		rc = -1;
	if (hp_wait_count_at_least(h_probe, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                           TEST_PID_2, TEST_SERIAL_2, 1, EVENT_TIMEOUT_MS) != 0)
		rc = -1;

	if (rc == 0) {
		/* Exactly one invocation total; which device is unspecified. */
		if (hp_count(h_once, 0, 0, NULL) != 1) {
			printf("    CHECK failed: the non-zero-return callback saw %d events "
			       "(expected 1) (line %d)\n",
			       hp_count(h_once, 0, 0, NULL), __LINE__);
			fflush(stdout);
			rc = -1;
		}
		if (hid_hotplug_deregister_callback(h_once) != -1) {
			printf("    CHECK failed: h_once was still registered (line %d)\n", __LINE__);
			fflush(stdout);
			rc = -1;
		}
	}

	hp_cleanup_callbacks();
	test_virtual_device_destroy(vdev2);
	if (hp_wait_enumerated(TEST_PID_2, TEST_SERIAL_2, 0, EVENT_TIMEOUT_MS) != 0)
		rc = -1;
	if (rc != 0) {
		g_failures++;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* T9: the ENUMERATE pass is delivered before live events: even when   */
/* the device is unplugged immediately after registration, the LEFT    */
/* must be preceded by the snapshot ARRIVED (same path).               */
static int t9_pass_before_live(void)
{
	hid_hotplug_callback_handle h = 0;
	hp_event arrived, left;

	CHECK(ensure_present() == 0);
	hp_reset_log("T9");

	step("register with ENUMERATE and unplug immediately");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS,
	                                    HID_API_HOTPLUG_ENUMERATE,
	                                    cb_log, NULL, &h) == 0);
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);

	step("wait for the LEFT");
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("the ARRIVED must already be logged, before the LEFT");
	CHECK(hp_find_first(&arrived, h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_find_first(&left, h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(arrived.seq < left.seq);
	CHECK(strcmp(arrived.path, left.path) == 0);
	CHECK(hp_count(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);

	CHECK(hid_hotplug_deregister_callback(h) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T10: live ARRIVED/LEFT payloads: matching VID/PID/serial on         */
/* arrival; the LEFT carries the same path and intact strings; and     */
/* device->next == NULL on every invocation.                           */
static int t10_live_payloads(void)
{
	hid_hotplug_callback_handle h = 0;
	hp_event arrived, left;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T10");

	step("register (no ENUMERATE) while the device is absent");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h) == 0);

	step("plug: live ARRIVED");
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_find_first(&arrived, h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(arrived.vendor_id == TEST_VID);
	CHECK(arrived.product_id == TEST_PID);
	CHECK(strcmp(arrived.serial, TEST_SERIAL) == 0);
	CHECK(arrived.path[0] != '\0');

	step("unplug: live LEFT correlates by path, strings intact");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_find_first(&left, h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(strcmp(left.path, arrived.path) == 0);
	CHECK(strcmp(left.serial, TEST_SERIAL) == 0);
	CHECK(left.vendor_id == TEST_VID && left.product_id == TEST_PID);

	CHECK(left.path_was_null == arrived.path_was_null);
	CHECK(left.serial_was_null == arrived.serial_was_null);
	CHECK(wcscmp(left.serial_full, arrived.serial_full) == 0);
	CHECK(left.manufacturer_was_null == arrived.manufacturer_was_null);
	CHECK(left.product_was_null == arrived.product_was_null);
	CHECK(wcscmp(left.manufacturer, arrived.manufacturer) == 0);
	CHECK(wcscmp(left.product, arrived.product) == 0);
	#if defined(_WIN32) || defined(TEST_VDEV_HAS_MANUFACTURER)
	CHECK(!arrived.manufacturer_was_null && arrived.manufacturer[0] != L'\0');
#endif
	CHECK(!arrived.product_was_null && arrived.product[0] != L'\0');
	CHECK(left.release_number == arrived.release_number);
	CHECK(left.usage_page == arrived.usage_page);
	CHECK(left.usage == arrived.usage);
	CHECK(left.interface_number == arrived.interface_number);
	CHECK(left.bus_type == arrived.bus_type);

	CHECK(hid_hotplug_deregister_callback(h) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T11: without ENUMERATE there is no synthetic ARRIVED, yet the LEFT  */
/* of an already-present device is still delivered. Because the pass   */
/* precedes live events, receiving the LEFT with no prior ARRIVED      */
/* proves no synthetic event was pending (zero-window proof).          */
static int t11_left_without_enumerate(void)
{
	hid_hotplug_callback_handle h = 0;

	CHECK(ensure_present() == 0);
	hp_reset_log("T11");

	step("register WITHOUT ENUMERATE while the device is present");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h) == 0);

	step("unplug: the LEFT must still be delivered");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("zero synthetic ARRIVED (the LEFT is the barrier)");
	CHECK(hp_count(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 0);

	CHECK(hid_hotplug_deregister_callback(h) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T12: VID/PID filtering: exact and vid-only filters and the wildcard */
/* see the event; a non-matching filter does not. The wildcard         */
/* (registered last, dispatch is in registration order) anchors the    */
/* absence assertion.                                                  */
static int t12_vid_pid_filtering(void)
{
	hid_hotplug_callback_handle h_match = 0, h_vid = 0, h_wrong = 0, h_wild = 0;
	hid_hotplug_callback_handle h_pid = 0, h_wrongpid = 0;
	unsigned short wrong_pid = 1;
	while (wrong_pid == TEST_PID || wrong_pid == TEST_PID_2)
		wrong_pid++;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T12");

	step("register exact / vid-only / wrong-vid / wildcard callbacks");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_match) == 0);
	CHECK(hp_register(TEST_VID, 0, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_vid) == 0);
	CHECK(hp_register(TEST_VID ^ 0x0001, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_wrong) == 0);
	CHECK(hp_register(0, TEST_PID, ALL_EVENTS, 0, cb_log, NULL, &h_pid) == 0);
	CHECK(hp_register(TEST_VID, wrong_pid, ALL_EVENTS, 0, cb_log, NULL, &h_wrongpid) == 0);
	CHECK(hp_register(0, 0, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_wild) == 0);

	step("plug the device");
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);

	step("exact, vid-only and wildcard callbacks see the ARRIVED");
	CHECK(hp_wait_count_at_least(h_match, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_wait_count_at_least(h_vid, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_wait_count_at_least(h_wild, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("the non-matching callback saw nothing of our device");
	/* The wildcard is dispatched after h_wrong (registration order), so once
	   the wildcard has logged the event, h_wrong's turn is provably over. */
	CHECK(hp_count(h_wrong, 0, TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_count(h_wrongpid, 0, TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_count(h_pid, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_wild, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h_match, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, TEST_PID, TEST_SERIAL) == 1);
	CHECK(hp_count(h_vid, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, TEST_PID, TEST_SERIAL) == 1);
	CHECK(hp_count(h_pid, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, TEST_PID, TEST_SERIAL) == 1);
	CHECK(hp_count(h_wrong, 0, TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_count(h_wrongpid, 0, TEST_PID, TEST_SERIAL) == 0);

	CHECK(hid_hotplug_deregister_callback(h_match) == 0);
	CHECK(hid_hotplug_deregister_callback(h_vid) == 0);
	CHECK(hid_hotplug_deregister_callback(h_wrong) == 0);
	CHECK(hid_hotplug_deregister_callback(h_pid) == 0);
	CHECK(hid_hotplug_deregister_callback(h_wrongpid) == 0);
	CHECK(hid_hotplug_deregister_callback(h_wild) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T13: one event is dispatched to every matching callback in          */
/* registration order.                                                 */
static int t13_dispatch_order(void)
{
	hid_hotplug_callback_handle h_a = 0, h_b = 0;
	hp_event ev_a, ev_b;

	CHECK(ensure_present() == 0);
	hp_reset_log("T13");

	step("register two matching callbacks");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_a) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_b) == 0);

	step("unplug: both see the LEFT");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_a, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_wait_count_at_least(h_b, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("registration order == dispatch order");
	CHECK(hp_find_first(&ev_a, h_a, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_find_first(&ev_b, h_b, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(ev_a.seq < ev_b.seq);

	CHECK(hid_hotplug_deregister_callback(h_a) == 0);
	CHECK(hid_hotplug_deregister_callback(h_b) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T14: hid_hotplug_deregister_callback() (from a non-event thread)    */
/* returns only after an in-progress invocation has completed; then    */
/* the callback's resources can be freed safely even though more       */
/* events keep flowing (an ASan leg would catch a use-after-free).     */
typedef struct deregister_ctx {
	hid_hotplug_callback_handle handle;
	int started, done, rc, return_seq;
} deregister_ctx;

static void hp_join_or_exit(test_thread *thread)
{
	if (test_thread_join_timeout(thread, EVENT_TIMEOUT_MS) != 0) {
		fprintf(stderr, "hotplug helper failed to join; shared state is still in use\n");
		fflush(stderr);
		_Exit(EXIT_FAILURE);
	}
}

static void deregister_thread(void *arg)
{
	deregister_ctx *ctx = (deregister_ctx *)arg;
	int rc;
	test_mutex_lock(&g_log_lock);
	ctx->started = 1;
	test_mutex_unlock(&g_log_lock);
	rc = hid_hotplug_deregister_callback(ctx->handle);
	if (rc == 0)
		hp_mark_retired(ctx->handle);
	test_mutex_lock(&g_log_lock);
	ctx->rc = rc;
	ctx->return_seq = g_seq_counter++;
	ctx->done = 1;
	test_mutex_unlock(&g_log_lock);
}

static void replug_thread(void *arg)
{
	int *rc = (int *)arg;
	*rc = test_virtual_device_replug(g_vdev);
}

static int t14_deregister_postcondition(void)
{
	hid_hotplug_callback_handle h_slow = 0, h_bar = 0;
	slow_ctx *ctx;
	deregister_ctx dereg = { 0, 0, 0, -2, 0 };
	test_thread plug_thread, dereg_thread;
	hp_event barrier;
	int plug_rc = TEST_VDEV_ERROR, entered, started = -1, early = 0;
	int worker_started = 0, expired, exit_seq;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T14");
	ctx = (slow_ctx *)calloc(1, sizeof(*ctx));
	CHECK(ctx != NULL);
	if (hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_slow, ctx, &h_slow) != 0
	    || hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_log, NULL, &h_bar) != 0
	    || test_thread_start(&plug_thread, replug_thread, &plug_rc) != 0) {
		hp_cleanup_callbacks();
		free(ctx);
		CHECK(!"failed to prepare gated callback");
	}

	/* Replug may wait for the platform to process arrival, so it must not prevent the
	   application from releasing the callback's gate. */
	entered = hp_wait_flag(&ctx->entered, EVENT_TIMEOUT_MS);
	if (entered == 0) {
		dereg.handle = h_slow;
		worker_started = (test_thread_start(&dereg_thread, deregister_thread, &dereg) == 0);
		if (worker_started) {
			started = hp_wait_flag(&dereg.started, EVENT_TIMEOUT_MS);
			/* This bounded observation rejects an early return while parked;
			   the sequence checks below also cover a delayed helper start. */
			{
				long long deadline = test_now_ms() + 100;
				do {
					test_mutex_lock(&g_log_lock);
					early = dereg.done;
					test_mutex_unlock(&g_log_lock);
					if (early)
						break;
					test_sleep_ms(WAIT_TICK_MS);
				} while (test_now_ms() < deadline);
			}
		}
	}
	test_mutex_lock(&g_log_lock);
	ctx->release = 1;
	test_mutex_unlock(&g_log_lock);
	hp_join_or_exit(&plug_thread);
	if (worker_started)
		hp_join_or_exit(&dereg_thread);
	/* Keep storage alive even on a failed start/entry/early-return path. */
	(void)hid_hotplug_deregister_callback(h_slow);
	test_mutex_lock(&g_log_lock);
	expired = ctx->expired;
	exit_seq = ctx->exit_seq;
	test_mutex_unlock(&g_log_lock);
	free(ctx);
	CHECK(entered == 0 && worker_started && started == 0);
	CHECK(plug_rc == TEST_VDEV_OK);
	CHECK(!early && !expired);
	CHECK(dereg.rc == 0 && exit_seq < dereg.return_seq);
	CHECK(hp_wait_count_at_least(h_bar, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_find_first(&barrier, h_bar, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(exit_seq < barrier.seq);

	step("free the callback's resources and keep events flowing");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_bar, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h_slow, 0, 0, NULL) == 1);
	CHECK(hid_hotplug_deregister_callback(h_bar) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T15: register and deregister from within a callback: on its first   */
/* ARRIVED the parent registers a child callback with ENUMERATE (the   */
/* child must see the device exactly once, by snapshot or live delivery) */
/* and deregisters itself.                                             */
static int t15_reentrant_registration(void)
{
	static parent_ctx ctx; /* static: zeroed, outlives any late invocation */
	hid_hotplug_callback_handle h_parent = 0, h_child = 0;
	int child_rc, self_dereg_rc;
	int invalid_rc, error_unchanged, nested, plug_rc, done;
	wchar_t *saved_error;
	size_t error_size;
	hp_event parent_event, child_event;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T15");
	memset(&ctx, 0, sizeof(ctx));

	step("register the parent callback");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_parent, &ctx, &h_parent) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, NULL, NULL, &h_child) == -1);
	error_size = (wcslen(hid_error(NULL)) + 1) * sizeof(wchar_t);
	saved_error = (wchar_t *)malloc(error_size);
	CHECK(saved_error != NULL);
	memcpy(saved_error, hid_error(NULL), error_size);

	step("plug: the parent registers the child and deregisters itself");
	plug_rc = test_virtual_device_replug(g_vdev);
	done = hp_wait_flag(&ctx.done, EVENT_TIMEOUT_MS);
	error_unchanged = (wcscmp(hid_error(NULL), saved_error) == 0);
	free(saved_error);
	CHECK(plug_rc == TEST_VDEV_OK && done == 0);
	CHECK(error_unchanged);

	test_mutex_lock(&g_log_lock);
	child_rc = ctx.child_rc;
	h_child = ctx.child_handle;
	self_dereg_rc = ctx.self_dereg_rc;
	invalid_rc = ctx.invalid_dereg_rc;
	test_mutex_unlock(&g_log_lock);
	CHECK(child_rc == 0);
	CHECK(h_child > 0);
	CHECK(self_dereg_rc == 0); /* deregistering itself, mid-callback, works */
	CHECK(invalid_rc == -1);

	step("the child sees the device exactly once (snapshot or live delivery)");
	CHECK(hp_wait_count_at_least(h_child, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("unplug (barrier for the exactly-once assertion)");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_child, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h_child, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	               TEST_PID, TEST_SERIAL) == 1);
	test_mutex_lock(&g_log_lock);
	nested = ctx.child_nested;
	test_mutex_unlock(&g_log_lock);
	CHECK(!nested);
	CHECK(hp_find_first(&parent_event, h_parent, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_find_first(&child_event, h_child, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	/* Both callbacks ran on HIDAPI's internal event context, never on the
	   registering (main) thread. The contract only promises that the context
	   is not the application's thread: the Windows backend delivers from a
	   threadpool / CM notification thread, so the two invocations may carry
	   different thread ids. */
	CHECK(parent_event.thread_id != g_main_tid);
	CHECK(child_event.thread_id != g_main_tid);

	step("the parent saw only its one ARRIVED and its handle is dead");
	CHECK(hp_count(h_parent, 0, 0, NULL) == 1);
	CHECK(hid_hotplug_deregister_callback(h_parent) == -1);

	CHECK(hid_hotplug_deregister_callback(h_child) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */

/* T17: removing a later callback from inside dispatch cancels its turn. */
typedef struct remove_ctx {
	hid_hotplug_callback_handle other;
	int rc;
} remove_ctx;

static int HID_API_CALL cb_remove_other(hid_hotplug_callback_handle handle,
                                        struct hid_device_info *device,
                                        hid_hotplug_event event, void *user_data)
{
	remove_ctx *ctx = (remove_ctx *)user_data;
	hp_record(handle, device, event);
	if (event == HID_API_HOTPLUG_EVENT_DEVICE_LEFT) {
		hid_hotplug_callback_handle other;
		int rc;
		test_mutex_lock(&g_log_lock);
		other = ctx->other;
		test_mutex_unlock(&g_log_lock);
		rc = hid_hotplug_deregister_callback(other);
		test_mutex_lock(&g_log_lock);
		ctx->rc = rc;
		test_mutex_unlock(&g_log_lock);
	}
	return 0;
}

static int t17_remove_other(void)
{
	hid_hotplug_callback_handle a = 0, b = 0, c = 0;
	remove_ctx ctx = { 0, -2 };
	int rc;
	CHECK(ensure_present() == 0);
	hp_reset_log("T17");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_remove_other, &ctx, &a) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_log, NULL, &b) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_log, NULL, &c) == 0);
	test_mutex_lock(&g_log_lock);
	ctx.other = b;
	test_mutex_unlock(&g_log_lock);
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(c, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	test_mutex_lock(&g_log_lock);
	rc = ctx.rc;
	test_mutex_unlock(&g_log_lock);
	CHECK(rc == 0);
	CHECK(hp_count(b, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, 0, NULL) == 0);
	CHECK(hid_hotplug_deregister_callback(b) == -1);
	CHECK(hid_hotplug_deregister_callback(a) == 0);
	CHECK(hid_hotplug_deregister_callback(c) == 0);
	return 0;
}

/* T18: immediate deregistration cancels any still-pending snapshot work.
   The log outlives user_data, so late calls are visible even without ASan. */
static int HID_API_CALL cb_heap_record(hid_hotplug_callback_handle handle,
                                       struct hid_device_info *device,
                                       hid_hotplug_event event, void *user_data)
{
	int *count = (int *)user_data;
	hp_record(handle, device, event);
	test_mutex_lock(&g_log_lock);
	(*count)++;
	test_mutex_unlock(&g_log_lock);
	return 0;
}

static int t18_immediate_deregister(void)
{
	int i;
	CHECK(ensure_present() == 0);
	for (i = 0; i < 50; i++) {
		hid_hotplug_callback_handle h = 0, barrier = 0;
		int *count, before, rc;
		hp_reset_log("T18");
		count = (int *)calloc(1, sizeof(*count));
		CHECK(count != NULL);
		rc = hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
		                   cb_heap_record, count, &h);
		if (rc == 0)
			rc = hid_hotplug_deregister_callback(h);
		if (rc == 0)
			hp_mark_retired(h);
		/* Retry cleanup only on failure; a second cancellation could mask a bug. */
		if (rc != 0)
			hp_cleanup_callbacks();
		test_mutex_lock(&g_log_lock);
		before = *count;
		test_mutex_unlock(&g_log_lock);
		free(count);
		CHECK(rc == 0);
		CHECK(before <= 1);
		CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
		                   cb_log, NULL, &barrier) == 0);
		CHECK(hp_wait_count_at_least(barrier, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
		                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
		CHECK(hp_count(h, 0, 0, NULL) == before);
		CHECK(hid_hotplug_deregister_callback(h) == -1);
		hp_cleanup_callbacks();
	}
	return 0;
}

/* T11b: masks filter both synthetic and live events. */
static int t11b_event_masks(void)
{
	hid_hotplug_callback_handle left = 0, arrived = 0, barrier = 0;
	CHECK(ensure_present() == 0);
	hp_reset_log("T11b");
	CHECK(hp_register(TEST_VID, TEST_PID, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                   HID_API_HOTPLUG_ENUMERATE, cb_log, NULL, &left) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                   0, cb_log, NULL, &arrived) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0, cb_log, NULL, &barrier) == 0);
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(left, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_wait_count_at_least(barrier, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(left, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, 0, NULL) == 0);
	CHECK(hp_count(left, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, 0, NULL) == 1);
	CHECK(hp_count(arrived, HID_API_HOTPLUG_EVENT_DEVICE_LEFT, 0, NULL) == 0);
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(barrier, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(arrived, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);
	CHECK(hp_count(left, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, 0, NULL) == 0);
	CHECK(hid_hotplug_deregister_callback(left) == 0);
	CHECK(hid_hotplug_deregister_callback(arrived) == 0);
	CHECK(hid_hotplug_deregister_callback(barrier) == 0);
	return 0;
}

/* Registration may wait for an already-running snapshot callback. */
typedef struct register_ctx {
	hid_hotplug_callback_fn callback;
	void *user_data;
	hid_hotplug_callback_handle handle;
	int rc;
} register_ctx;

static void register_thread(void *arg)
{
	register_ctx *ctx = (register_ctx *)arg;
	ctx->rc = hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
	                       ctx->callback, ctx->user_data, &ctx->handle);
}

/* T9b: force the disconnect to happen while the initial pass is active. */
static int t9b_parked_snapshot(void)
{
	slow_ctx gate = { 0 };
	register_ctx reg = { cb_slow, &gate, 0, -2 };
	test_thread thread;
	hp_event arrived, left;
	int entered, unplug_rc = TEST_VDEV_ERROR, expired;
	CHECK(ensure_present() == 0);
	hp_reset_log("T9b");
	CHECK(test_thread_start(&thread, register_thread, &reg) == 0);
	entered = hp_wait_flag(&gate.entered, EVENT_TIMEOUT_MS);
	if (entered == 0)
		unplug_rc = test_virtual_device_unplug(g_vdev);
	test_mutex_lock(&g_log_lock);
	gate.release = 1;
	test_mutex_unlock(&g_log_lock);
	hp_join_or_exit(&thread);
	CHECK(entered == 0 && unplug_rc == TEST_VDEV_OK && reg.rc == 0);
	CHECK(hp_wait_count_at_least(reg.handle, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hid_hotplug_deregister_callback(reg.handle) == 0);
	test_mutex_lock(&g_log_lock);
	expired = gate.expired;
	test_mutex_unlock(&g_log_lock);
	CHECK(!expired);
	CHECK(hp_find_first(&arrived, reg.handle, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(hp_find_first(&left, reg.handle, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(arrived.seq < left.seq && strcmp(arrived.path, left.path) == 0);
	CHECK(hp_count(reg.handle, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);
	return 0;
}

/* T8c uses the callback parameter, including before register returns. */
static int HID_API_CALL cb_remove_self(hid_hotplug_callback_handle handle,
                                       struct hid_device_info *device,
                                       hid_hotplug_event event, void *user_data)
{
	int *result = (int *)user_data;
	int rc;
	hp_record(handle, device, event);
	rc = hid_hotplug_deregister_callback(handle);
	test_mutex_lock(&g_log_lock);
	*result = rc;
	test_mutex_unlock(&g_log_lock);
	return 0;
}

static int t8c_snapshot_self_deregister(void)
{
	hid_hotplug_callback_handle h = 0, barrier = 0;
	int result = -2, rc;
	CHECK(ensure_present() == 0);
	hp_reset_log("T8c");
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
	                   cb_remove_self, &result, &h) == 0);
	/* Await its own event first: snapshots of different registrations have
	   no cross-registration ordering guarantee. */
	CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
	                   cb_log, NULL, &barrier) == 0);
	CHECK(hp_wait_count_at_least(barrier, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	test_mutex_lock(&g_log_lock);
	rc = result;
	test_mutex_unlock(&g_log_lock);
	CHECK(rc == 0 && hp_count(h, 0, 0, NULL) == 1);
	CHECK(hid_hotplug_deregister_callback(h) == -1);
	CHECK(hid_hotplug_deregister_callback(barrier) == 0);
	return 0;
}

/* T18b publishes a child with queued ENUMERATE work while the parent holds
   the event context. A second application thread then cancels that child. */
typedef struct queued_ctx {
	slow_ctx gate;
	int *count;
	hid_hotplug_callback_handle child;
	int child_rc;
} queued_ctx;

static int HID_API_CALL cb_queue_child(hid_hotplug_callback_handle handle,
                                       struct hid_device_info *device,
                                       hid_hotplug_event event, void *user_data)
{
	queued_ctx *ctx = (queued_ctx *)user_data;
	hid_hotplug_callback_handle child = 0;
	int rc = hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
	                       cb_heap_record, ctx->count, &child);
	test_mutex_lock(&g_log_lock);
	ctx->child = child;
	ctx->child_rc = rc;
	test_mutex_unlock(&g_log_lock);
	cb_slow(handle, device, event, &ctx->gate);
	return 1;
}

static int t18b_queued_deregister(void)
{
	queued_ctx ctx = { 0 };
	register_ctx reg = { cb_queue_child, &ctx, 0, -2 };
	deregister_ctx dereg = { 0, 0, 0, -2, 0 };
	test_thread registration, cancellation;
	hid_hotplug_callback_handle barrier = 0;
	int entered, worker_started = 0, started = -1, before, expired, child_rc;
	CHECK(ensure_present() == 0);
	hp_reset_log("T18b");
	ctx.count = (int *)calloc(1, sizeof(*ctx.count));
	CHECK(ctx.count != NULL);
	if (test_thread_start(&registration, register_thread, &reg) != 0) {
		free(ctx.count);
		CHECK(!"failed to start snapshot registration");
	}
	entered = hp_wait_flag(&ctx.gate.entered, EVENT_TIMEOUT_MS);
	test_mutex_lock(&g_log_lock);
	dereg.handle = ctx.child;
	child_rc = ctx.child_rc;
	test_mutex_unlock(&g_log_lock);
	if (entered == 0 && child_rc == 0) {
		worker_started = (test_thread_start(&cancellation, deregister_thread, &dereg) == 0);
		if (worker_started)
			started = hp_wait_flag(&dereg.started, EVENT_TIMEOUT_MS);
	}
	test_mutex_lock(&g_log_lock);
	ctx.gate.release = 1;
	test_mutex_unlock(&g_log_lock);
	hp_join_or_exit(&registration);
	if (worker_started)
		hp_join_or_exit(&cancellation);
	if (!worker_started || dereg.rc != 0)
		hp_cleanup_callbacks();
	test_mutex_lock(&g_log_lock);
	before = *ctx.count;
	expired = ctx.gate.expired;
	test_mutex_unlock(&g_log_lock);
	free(ctx.count);
	CHECK(entered == 0 && !expired && reg.rc == 0 && child_rc == 0);
	CHECK(worker_started && started == 0 && dereg.rc == 0 && before <= 1);
	CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
	                   cb_log, NULL, &barrier) == 0);
	CHECK(hp_wait_count_at_least(barrier, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(dereg.handle, 0, 0, NULL) == before);
	CHECK(hid_hotplug_deregister_callback(dereg.handle) == -1);
	CHECK(hid_hotplug_deregister_callback(barrier) == 0);
	return 0;
}

/* T19: exit with a gated ENUMERATE callback, then reinitialize and deliver
   again. Old callback state stays alive across every initialization lifetime. */
typedef struct exit_ctx {
	slow_ctx gate;
	int exiting, returned, late, count;
} exit_ctx;

static int HID_API_CALL cb_exit_record(hid_hotplug_callback_handle handle,
                                       struct hid_device_info *device,
                                       hid_hotplug_event event, void *user_data)
{
	exit_ctx *ctx = (exit_ctx *)user_data;
	cb_slow(handle, device, event, &ctx->gate);
	test_mutex_lock(&g_log_lock);
	ctx->count++;
	if (ctx->returned)
		ctx->late = 1;
	test_mutex_unlock(&g_log_lock);
	return 0;
}

static void release_exit_thread(void *arg)
{
	exit_ctx *ctx = (exit_ctx *)arg;
	if (hp_wait_flag(&ctx->exiting, EVENT_TIMEOUT_MS) == 0) {
		long long deadline = test_now_ms() + 100;
		/* Keep the callback parked during the exit attempt, as in T14. */
		do {
			int returned;
			test_mutex_lock(&g_log_lock);
			returned = ctx->returned;
			test_mutex_unlock(&g_log_lock);
			if (returned)
				break;
			test_sleep_ms(WAIT_TICK_MS);
		} while (test_now_ms() < deadline);
	}
	test_mutex_lock(&g_log_lock);
	ctx->gate.release = 1;
	test_mutex_unlock(&g_log_lock);
}

static int t19_pending_exit(void)
{
	static exit_ctx ctxs[10];
	int i, late = 0, count = 0;
	CHECK(ensure_present() == 0);
	for (i = 0; i < 10; i++) {
		hid_hotplug_callback_handle h = 0, barrier = 0;
		test_thread release;
		int entered, started, exit_rc, exited, expired, post_count = 0;
		hp_reset_log("T19");
		CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
		                   cb_exit_record, &ctxs[i], &h) == 0);
		entered = hp_wait_flag(&ctxs[i].gate.entered, EVENT_TIMEOUT_MS);
		started = test_thread_start(&release, release_exit_thread, &ctxs[i]);
		if (started != 0) {
			test_mutex_lock(&g_log_lock);
			ctxs[i].gate.release = 1;
			test_mutex_unlock(&g_log_lock);
			CHECK(started == 0);
		}
		/* Lifecycle calls stay on the initializing thread, including on macOS. */
		test_mutex_lock(&g_log_lock);
		ctxs[i].exiting = 1;
		test_mutex_unlock(&g_log_lock);
		exit_rc = hid_exit();
		test_mutex_lock(&g_log_lock);
		ctxs[i].returned = 1;
		exited = ctxs[i].gate.exited;
		expired = ctxs[i].gate.expired;
		/* Handles need not remain unique across initialization lifetimes. */
		g_handle_count = 0;
		test_mutex_unlock(&g_log_lock);
		hp_join_or_exit(&release);
		CHECK(entered == 0 && !expired && exited && exit_rc == 0);
		CHECK(hid_init() == 0);
		CHECK(hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
		                   cb_heap_record, &post_count, &barrier) == 0);
		CHECK(hp_wait_flag(&post_count, EVENT_TIMEOUT_MS) == 0);
		hp_cleanup_callbacks();
	}
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < 10; i++) {
		late |= ctxs[i].late;
		count += ctxs[i].count;
	}
	test_mutex_unlock(&g_log_lock);
	printf("    pending-exit callback invocations: %d\n", count);
	CHECK(!late);
	return 0;
}

/* Optional arrival-versus-snapshot race; HIDAPI lifecycle calls remain on
   main, and the provider is used by only one thread at a time. */
static int t20_arrival_stress(void)
{
	int i;
	for (i = 0; i < 25; i++) {
		test_thread plug;
		hid_hotplug_callback_handle h = 0;
		int plug_rc = TEST_VDEV_ERROR, register_rc;
		CHECK(ensure_absent() == 0);
		hp_reset_log("T20");
		CHECK(test_thread_start(&plug, replug_thread, &plug_rc) == 0);
		register_rc = hp_register(TEST_VID, TEST_PID, ALL_EVENTS, HID_API_HOTPLUG_ENUMERATE,
		                           cb_log, NULL, &h);
		hp_join_or_exit(&plug);
		CHECK(register_rc == 0 && plug_rc == TEST_VDEV_OK);
		CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
		                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
		CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
		CHECK(hp_wait_count_at_least(h, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
		                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
		CHECK(hp_count(h, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, TEST_PID, TEST_SERIAL) == 1);
		CHECK(hid_hotplug_deregister_callback(h) == 0);
	}
	return 0;
}

#define RUN_TEST(name, call) do { \
	report(name, call); \
	if (test_atomic_load(&g_deadline_failed)) goto done; \
} while (0)

int main(void)
{
	int rc;
	hid_hotplug_callback_handle probe = 0;

	g_main_tid = test_thread_id();
	test_mutex_init(&g_log_lock);

	if (hid_init() != 0) {
		printf("hid_init() failed\n");
		test_mutex_destroy(&g_log_lock);
		return EXIT_FAILURE;
	}

	step("probe hotplug support");
	if (hp_register(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                  cb_log, NULL, &probe) != 0) {
		printf("hotplug reported unsupported here - skipping\n");
		hid_exit();
		test_mutex_destroy(&g_log_lock);
		return EXIT_SKIP;
	}
	(void)hid_hotplug_deregister_callback(probe);

	step("create virtual device");
	rc = test_virtual_device_create(&g_vdev, TEST_VID, TEST_PID, TEST_SERIAL);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("virtual device unavailable on this host - skipping\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		test_mutex_destroy(&g_log_lock);
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK || !g_vdev) {
		printf("failed to create virtual device (rc=%d)\n", rc);
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		test_mutex_destroy(&g_log_lock);
		return EXIT_FAILURE;
	}

	step("wait for initial device presence");
	if (hp_wait_enumerated(TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) != 0) {
		printf("virtual device did not initially enumerate - skipping\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		test_mutex_destroy(&g_log_lock);
		return EXIT_SKIP;
	}

	step("probe unplug support");
	rc = test_virtual_device_unplug(g_vdev);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("this provider cannot toggle device presence - skipping\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		test_mutex_destroy(&g_log_lock);
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK) {
		printf("unplug probe failed (rc=%d)\n", rc);
		goto probe_failed;
	}
	if (hp_wait_enumerated(TEST_PID, TEST_SERIAL, 0, EVENT_TIMEOUT_MS) != 0) {
		printf("device remained present after unplug probe\n");
		goto probe_failed;
	}
	rc = test_virtual_device_replug(g_vdev);
	if (rc != TEST_VDEV_OK) {
		printf("replug probe failed (rc=%d)\n", rc);
		goto probe_failed;
	}
	if (hp_wait_enumerated(TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) != 0) {
		printf("device did not reappear after successful unplug/replug\n");
		goto probe_failed;
	}

	printf("running hotplug tests...\n");
	fflush(stdout);

	printf("T6: asynchronous delivery + handle parameter\n");
	RUN_TEST("T6 async_delivery", t6_async_delivery());
	printf("T7: exactly-once (ENUMERATE pass vs live events)\n");
	RUN_TEST("T7 exactly_once", t7_exactly_once());
	printf("T8a: non-zero callback return deregisters\n");
	RUN_TEST("T8a return_deregisters", t8a_return_deregisters(1));
	RUN_TEST("T8a negative_return_deregisters", t8a_return_deregisters(-1));
	printf("T8b: non-zero return stops the rest of the ENUMERATE pass\n");
	RUN_TEST("T8b return_stops_pass", t8b_return_stops_pass(1));
	RUN_TEST("T8b negative_return_stops_pass", t8b_return_stops_pass(-1));
	printf("T9: ENUMERATE pass delivered before live events\n");
	RUN_TEST("T9 pass_before_live", t9_pass_before_live());
	printf("T10: live ARRIVED/LEFT payloads\n");
	RUN_TEST("T10 live_payloads", t10_live_payloads());
	printf("T11: LEFT without ENUMERATE (zero-window proof)\n");
	RUN_TEST("T11 left_without_enumerate", t11_left_without_enumerate());
	printf("T12: VID/PID filtering\n");
	RUN_TEST("T12 vid_pid_filtering", t12_vid_pid_filtering());
	printf("T13: dispatch in registration order\n");
	RUN_TEST("T13 dispatch_order", t13_dispatch_order());
	printf("T14: deregistration post-condition\n");
	RUN_TEST("T14 deregister_postcondition", t14_deregister_postcondition());
	printf("T15: register/deregister from within a callback\n");
	RUN_TEST("T15 reentrant_registration", t15_reentrant_registration());
	RUN_TEST("T11b event_masks", t11b_event_masks());
	RUN_TEST("T17 remove_other", t17_remove_other());
	RUN_TEST("T18 immediate_deregister", t18_immediate_deregister());
	RUN_TEST("T8c snapshot_self_deregister", t8c_snapshot_self_deregister());
	RUN_TEST("T9b parked_snapshot", t9b_parked_snapshot());
	RUN_TEST("T18b queued_deregister", t18b_queued_deregister());
	RUN_TEST("T19 pending_exit", t19_pending_exit());
	if (hp_env_set("HIDAPI_HOTPLUG_STRESS"))
		RUN_TEST("T20 arrival_stress", t20_arrival_stress());

done:
	hp_reset_log("(final sweep)"); /* global invariants over the last test */

	test_virtual_device_destroy(g_vdev);
	hid_exit();
	test_mutex_destroy(&g_log_lock);

	printf("%s hotplug (%d failed checks, %d skipped scenarios)\n",
	       g_failures == 0 ? "PASS" : "FAIL", g_failures, g_skipped);
	return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

probe_failed:
	test_virtual_device_destroy(g_vdev);
	hid_exit();
	test_mutex_destroy(&g_log_lock);
	return EXIT_FAILURE;
}
