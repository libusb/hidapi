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
   - callbacks only lock, deep-copy the event into a log,
     unlock and return; they never call hid_enumerate/hid_open/
     hid_error(NULL);
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

/* Test-unique ids so enumeration/filtering cannot collide with real hardware
   (distinct from test_device_io.c's 0xF1D0:0x9001). */
#define TEST_VID      0xF1D0
#define TEST_PID      0x9002
#define TEST_PID_2    0x9003   /* second device, for the mid-pass stop test */
#define TEST_SERIAL   "HIDAPI-HOTPLUG-TEST"
#define TEST_SERIAL_2 "HIDAPI-HOTPLUG-TEST-2"

#define ALL_EVENTS (HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT)

/* Budget for one awaited event/predicate. The uhid provider is fast (10s is
   generous); the future rawgadget/win providers go through a full (virtual)
   USB stack, so their CMake target overrides this with 30s. */
#ifndef TEST_HOTPLUG_EVENT_TIMEOUT_MS
#define TEST_HOTPLUG_EVENT_TIMEOUT_MS 30000
#endif
#define EVENT_TIMEOUT_MS TEST_HOTPLUG_EVENT_TIMEOUT_MS

#define WAIT_TICK_MS 10

static int g_failures = 0;

#define CHECK(cond)                                                       \
	do {                                                              \
		if (!(cond)) {                                            \
			printf("    CHECK failed: %s (line %d)\n",       \
			       #cond, __LINE__);                         \
			fflush(stdout);                                  \
			g_failures++;                                    \
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
	printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", name);
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
	unsigned long long thread_id;         /* thread the callback ran on */
	int next_was_null;                    /* device->next == NULL held */
} hp_event;

static test_mutex g_log_lock;
static hp_event g_events[HP_MAX_EVENTS];
static int g_event_count;
static int g_event_overflow;
static int g_seq_counter;
static unsigned long long g_main_tid;

/* Deep-copy the fields the assertions need. Called from the callbacks, with
   g_log_lock held for the shortest possible time; the device pointer is only
   valid for the duration of the callback. */
static void hp_record(hid_hotplug_callback_handle handle,
                      struct hid_device_info *device,
                      hid_hotplug_event event)
{
	test_mutex_lock(&g_log_lock);
	if (g_event_count < HP_MAX_EVENTS) {
		hp_event *e = &g_events[g_event_count++];
		memset(e, 0, sizeof(*e));
		e->seq = g_seq_counter++;
		e->handle = handle;
		e->event = event;
		e->thread_id = test_thread_id();
		if (device) {
			e->vendor_id = device->vendor_id;
			e->product_id = device->product_id;
			e->next_was_null = (device->next == NULL);
			if (device->path)
				snprintf(e->path, sizeof(e->path), "%s", device->path);
			if (device->serial_number) {
				size_t i;
				for (i = 0; i + 1 < sizeof(e->serial) && device->serial_number[i]; i++) {
					wchar_t wc = device->serial_number[i];
					e->serial[i] = (wc > 0 && wc < 128) ? (char)wc : '?';
				}
				e->serial[i] = '\0';
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
		if (test_now_ms() >= deadline)
			return -1;
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
		if (test_now_ms() >= deadline)
			return -1;
		test_sleep_ms(WAIT_TICK_MS);
	}
}

/* Start-of-test reset. Also the global sweep for two invariants every event
   must satisfy: never delivered on the registering (main) thread, and never
   more events than the log can hold (an overflow would silently weaken the
   later absence assertions). */
static void hp_reset_log(const char *test_name)
{
	int i;
	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_event_count; i++) {
		if (g_events[i].thread_id == g_main_tid) {
			printf("    INVARIANT failed before %s: an event was "
			       "delivered on the registering thread\n", test_name);
			fflush(stdout);
			g_failures++;
			break;
		}
	}
	if (g_event_overflow) {
		printf("    INVARIANT failed before %s: event log overflow\n", test_name);
		fflush(stdout);
		g_failures++;
	}
	g_event_count = 0;
	g_event_overflow = 0;
	test_mutex_unlock(&g_log_lock);
}

/* ------------------------------------------------------------------ */
/* Callbacks. Per the synchronization discipline they only lock,       */
/* deep-copy, append, unlock and return.                               */

/* Plain recorder. */
static int HID_API_CALL cb_log(hid_hotplug_callback_handle callback_handle,
                               struct hid_device_info *device,
                               hid_hotplug_event event, void *user_data)
{
	(void)user_data;
	hp_record(callback_handle, device, event);
	return 0;
}

/* Recorder that asks to be deregistered (returns 1) on the first event for
   the test's primary device. */
static int HID_API_CALL cb_return1_on_ours(hid_hotplug_callback_handle callback_handle,
                                           struct hid_device_info *device,
                                           hid_hotplug_event event, void *user_data)
{
	(void)user_data;
	hp_record(callback_handle, device, event);
	if (device && device->vendor_id == TEST_VID && device->product_id == TEST_PID)
		return 1;
	return 0;
}

/* Recorder that asks to be deregistered on its very first event, whichever
   device it is for (the ENUMERATE snapshot order is unspecified). */
static int HID_API_CALL cb_return1_first(hid_hotplug_callback_handle callback_handle,
                                         struct hid_device_info *device,
                                         hid_hotplug_event event, void *user_data)
{
	(void)user_data;
	hp_record(callback_handle, device, event);
	return 1;
}

/* T14: signals "entered", stays inside the callback for a while, then signals
   "exited". Lets the main thread observe that deregistration blocks until an
   in-progress invocation has completed. The context is heap-allocated and
   freed right after deregistration returns: if the backend ever invoked the
   callback again, ASan would flag the use-after-free below. */
typedef struct slow_ctx {
	int entered;
	int exited;
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
	test_sleep_ms(250);
	test_mutex_lock(&g_log_lock);
	ctx->exited = 1;
	test_mutex_unlock(&g_log_lock);
	return 0;
}

/* T15: on the first ARRIVED for the primary device, registers a child
   callback WITH ENUMERATE and deregisters itself - both from within the
   callback (the hotplug API is documented re-entrant). */
typedef struct parent_ctx {
	int acted;
	int child_rc;
	hid_hotplug_callback_handle child_handle;
	int self_dereg_rc;
} parent_ctx;

static int HID_API_CALL cb_parent(hid_hotplug_callback_handle callback_handle,
                                  struct hid_device_info *device,
                                  hid_hotplug_event event, void *user_data)
{
	parent_ctx *ctx = (parent_ctx *)user_data;
	int act = 0;

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
		int rc = hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS,
		                                       HID_API_HOTPLUG_ENUMERATE,
		                                       cb_log, NULL, &child);
		int dereg_rc = hid_hotplug_deregister_callback(callback_handle);
		test_mutex_lock(&g_log_lock);
		ctx->child_rc = rc;
		ctx->child_handle = child;
		ctx->self_dereg_rc = dereg_rc;
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
		if (test_now_ms() >= deadline)
			return -1;
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
/* hid_hotplug_register_callback() wrote to *callback_handle.          */
static int t6_async_delivery(void)
{
	hid_hotplug_callback_handle h = 0;
	hp_event ev;

	CHECK(ensure_present() == 0);
	hp_reset_log("T6");

	step("register with ENUMERATE while the device is present");
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS,
	                                    HID_API_HOTPLUG_ENUMERATE,
	                                    cb_log, NULL, &h) == 0);
	CHECK(h > 0);

	step("wait for the synthetic ARRIVED");
	CHECK(hp_wait_count_at_least(0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	CHECK(hp_find_first(&ev, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                    TEST_PID, TEST_SERIAL) == 0);
	CHECK(ev.thread_id != g_main_tid);   /* asynchronous delivery */
	CHECK(ev.handle == h);               /* handle parameter == *callback_handle */

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
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS,
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
static int t8a_return_deregisters(void)
{
	hid_hotplug_callback_handle h_ret = 0, h_bar = 0;

	CHECK(ensure_present() == 0);
	hp_reset_log("T8a");

	step("register the returns-1 callback and a barrier callback");
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_return1_on_ours, NULL, &h_ret) == 0);
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_bar) == 0);

	step("unplug: both callbacks see the LEFT; the first returns 1");
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
static int t8b_return_stops_pass(void)
{
	test_virtual_device *vdev2 = NULL;
	hid_hotplug_callback_handle h_once = 0, h_probe = 0;
	int rc;

	CHECK(ensure_present() == 0);

	step("create the second device");
	rc = test_virtual_device_create(&vdev2, TEST_VID, TEST_PID_2, TEST_SERIAL_2);
	CHECK(rc == TEST_VDEV_OK && vdev2 != NULL);
	if (hp_wait_enumerated(TEST_PID_2, TEST_SERIAL_2, 1, EVENT_TIMEOUT_MS) != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"second device did not enumerate");
	}

	hp_reset_log("T8b");

	step("register a returns-1-immediately callback with ENUMERATE (both devices match)");
	rc = hid_hotplug_register_callback(TEST_VID, 0, ALL_EVENTS,
	                                   HID_API_HOTPLUG_ENUMERATE,
	                                   cb_return1_first, NULL, &h_once);
	if (rc != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"registration failed");
	}

	step("wait for its single snapshot event");
	if (hp_wait_count_at_least(h_once, 0, 0, NULL, 1, EVENT_TIMEOUT_MS) != 0) {
		test_virtual_device_destroy(vdev2);
		CHECK(!"the returns-1 callback never fired");
	}

	step("barrier: a fresh ENUMERATE registration sees both devices");
	rc = hid_hotplug_register_callback(TEST_VID, 0, ALL_EVENTS,
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
			printf("    CHECK failed: the returns-1 callback saw %d events "
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

	(void)hid_hotplug_deregister_callback(h_probe);
	test_virtual_device_destroy(vdev2);
	(void)hp_wait_enumerated(TEST_PID_2, TEST_SERIAL_2, 0, EVENT_TIMEOUT_MS);
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
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS,
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
	int i, all_next_null = 1;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T10");

	step("register (no ENUMERATE) while the device is absent");
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
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

	test_mutex_lock(&g_log_lock);
	for (i = 0; i < g_event_count; i++)
		if (g_events[i].handle == h && !g_events[i].next_was_null)
			all_next_null = 0;
	test_mutex_unlock(&g_log_lock);
	CHECK(all_next_null); /* device->next == NULL on EVERY invocation */

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
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
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

	CHECK(ensure_absent() == 0);
	hp_reset_log("T12");

	step("register exact / vid-only / wrong-vid / wildcard callbacks");
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_match) == 0);
	CHECK(hid_hotplug_register_callback(TEST_VID, 0, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_vid) == 0);
	CHECK(hid_hotplug_register_callback(TEST_VID ^ 0x0001, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_wrong) == 0);
	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0,
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

	CHECK(hid_hotplug_deregister_callback(h_match) == 0);
	CHECK(hid_hotplug_deregister_callback(h_vid) == 0);
	CHECK(hid_hotplug_deregister_callback(h_wrong) == 0);
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
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_log, NULL, &h_a) == 0);
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
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
static int t14_deregister_postcondition(void)
{
	hid_hotplug_callback_handle h_slow = 0, h_bar = 0;
	slow_ctx *ctx;
	int exited;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T14");

	ctx = (slow_ctx *)calloc(1, sizeof(*ctx));
	CHECK(ctx != NULL);

	step("register the slow callback and a barrier callback");
	if (hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                  cb_slow, ctx, &h_slow) != 0) {
		free(ctx);
		CHECK(!"registration failed");
	}
	if (hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                  cb_log, NULL, &h_bar) != 0) {
		(void)hid_hotplug_deregister_callback(h_slow);
		free(ctx);
		CHECK(!"barrier registration failed");
	}

	step("plug and wait for the slow callback to enter");
	if (test_virtual_device_replug(g_vdev) != TEST_VDEV_OK
	    || hp_wait_flag(&ctx->entered, EVENT_TIMEOUT_MS) != 0) {
		(void)hid_hotplug_deregister_callback(h_slow);
		(void)hid_hotplug_deregister_callback(h_bar);
		free(ctx);
		CHECK(!"the slow callback never entered");
	}

	step("deregister while the callback is (still) inside its invocation");
	CHECK(hid_hotplug_deregister_callback(h_slow) == 0);
	test_mutex_lock(&g_log_lock);
	exited = ctx->exited;
	test_mutex_unlock(&g_log_lock);
	CHECK(exited == 1); /* deregistration waited for the invocation */

	step("free the callback's resources and keep events flowing");
	free(ctx);
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_bar, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	CHECK(hid_hotplug_deregister_callback(h_bar) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T15: register and deregister from within a callback: on its first   */
/* ARRIVED the parent registers a child callback with ENUMERATE (the   */
/* child must see the device exactly once, via its snapshot) and       */
/* deregisters itself.                                                 */
static int t15_reentrant_registration(void)
{
	static parent_ctx ctx; /* static: zeroed, outlives any late invocation */
	hid_hotplug_callback_handle h_parent = 0, h_child = 0;
	int child_rc, self_dereg_rc;

	CHECK(ensure_absent() == 0);
	hp_reset_log("T15");
	memset(&ctx, 0, sizeof(ctx));

	step("register the parent callback");
	CHECK(hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                    cb_parent, &ctx, &h_parent) == 0);

	step("plug: the parent registers the child and deregisters itself");
	CHECK(test_virtual_device_replug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_flag(&ctx.acted, EVENT_TIMEOUT_MS) == 0);

	test_mutex_lock(&g_log_lock);
	child_rc = ctx.child_rc;
	h_child = ctx.child_handle;
	self_dereg_rc = ctx.self_dereg_rc;
	test_mutex_unlock(&g_log_lock);
	CHECK(child_rc == 0);
	CHECK(h_child > 0);
	CHECK(self_dereg_rc == 0); /* deregistering itself, mid-callback, works */

	step("the child sees the device exactly once (via its snapshot)");
	CHECK(hp_wait_count_at_least(h_child, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);

	step("unplug (barrier for the exactly-once assertion)");
	CHECK(test_virtual_device_unplug(g_vdev) == TEST_VDEV_OK);
	CHECK(hp_wait_count_at_least(h_child, HID_API_HOTPLUG_EVENT_DEVICE_LEFT,
	                             TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) == 0);
	CHECK(hp_count(h_child, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
	               TEST_PID, TEST_SERIAL) == 1);

	step("the parent saw only its one ARRIVED and its handle is dead");
	CHECK(hp_count(h_parent, 0, 0, NULL) == 1);
	CHECK(hid_hotplug_deregister_callback(h_parent) == -1);

	CHECK(hid_hotplug_deregister_callback(h_child) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
	int rc;
	hid_hotplug_callback_handle probe = 0;

	g_main_tid = test_thread_id();
	test_mutex_init(&g_log_lock);

	if (hid_init() != 0) {
		printf("hid_init() failed\n");
		return EXIT_FAILURE;
	}

	step("probe hotplug support");
	if (hid_hotplug_register_callback(TEST_VID, TEST_PID, ALL_EVENTS, 0,
	                                  cb_log, NULL, &probe) != 0) {
		printf("hotplug reported unsupported here - skipping\n");
		hid_exit();
		return EXIT_SKIP;
	}
	(void)hid_hotplug_deregister_callback(probe);

	step("create virtual device");
	rc = test_virtual_device_create(&g_vdev, TEST_VID, TEST_PID, TEST_SERIAL);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("virtual device unavailable on this host - skipping\n");
		hid_exit();
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK || !g_vdev) {
		printf("failed to create virtual device (rc=%d)\n", rc);
		hid_exit();
		return EXIT_FAILURE;
	}

	/* Probed before waiting for enumeration so that providers without
	   presence toggling skip instantly instead of after a full wait. */
	step("probe unplug/replug support");
	rc = test_virtual_device_unplug(g_vdev);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("this provider cannot toggle device presence - skipping\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK
	    || hp_wait_enumerated(TEST_PID, TEST_SERIAL, 0, EVENT_TIMEOUT_MS) != 0
	    || test_virtual_device_replug(g_vdev) != TEST_VDEV_OK) {
		printf("unplug/replug probe failed\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		return EXIT_FAILURE;
	}

	/* The readiness barrier doubling as the presence probe: a virtual
	   device that never enumerates means this host cannot run the test
	   (same skip semantics as the device-I/O test). */
	step("wait for the device to enumerate");
	if (hp_wait_enumerated(TEST_PID, TEST_SERIAL, 1, EVENT_TIMEOUT_MS) != 0) {
		printf("virtual device did not enumerate - skipping\n");
		test_virtual_device_destroy(g_vdev);
		hid_exit();
		return EXIT_SKIP;
	}

	printf("running hotplug tests...\n");
	fflush(stdout);

	printf("T6: asynchronous delivery + handle parameter\n");
	report("T6 async_delivery", t6_async_delivery());
	printf("T7: exactly-once (ENUMERATE pass vs live events)\n");
	report("T7 exactly_once", t7_exactly_once());
	printf("T8a: non-zero callback return deregisters\n");
	report("T8a return_deregisters", t8a_return_deregisters());
	printf("T8b: non-zero return stops the rest of the ENUMERATE pass\n");
	report("T8b return_stops_pass", t8b_return_stops_pass());
	printf("T9: ENUMERATE pass delivered before live events\n");
	report("T9 pass_before_live", t9_pass_before_live());
	printf("T10: live ARRIVED/LEFT payloads\n");
	report("T10 live_payloads", t10_live_payloads());
	printf("T11: LEFT without ENUMERATE (zero-window proof)\n");
	report("T11 left_without_enumerate", t11_left_without_enumerate());
	printf("T12: VID/PID filtering\n");
	report("T12 vid_pid_filtering", t12_vid_pid_filtering());
	printf("T13: dispatch in registration order\n");
	report("T13 dispatch_order", t13_dispatch_order());
	printf("T14: deregistration post-condition\n");
	report("T14 deregister_postcondition", t14_deregister_postcondition());
	printf("T15: register/deregister from within a callback\n");
	report("T15 reentrant_registration", t15_reentrant_registration());

	hp_reset_log("(final sweep)"); /* global invariants over the last test */

	test_virtual_device_destroy(g_vdev);
	hid_exit();
	test_mutex_destroy(&g_log_lock);

	printf("%s hotplug (%d failed checks)\n",
	       g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
