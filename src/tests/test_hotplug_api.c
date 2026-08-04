/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Tier-1 hotplug API tests: argument validation, callback-handle
 properties, implicit initialization, hid_exit() teardown and
 register/deregister thread-safety.

 These tests need NO device (virtual or real) and no privileges,
 so they run against every backend in the ordinary CI matrix.
 They only exercise the parts of the hotplug contract that are
 observable without a device event; the device-backed scenarios
 live in test_hotplug.c.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hidapi.h>

#include "test_platform.h"

/* CTest treats this exit code as "skipped" (see SKIP_RETURN_CODE in CMake). */
#define EXIT_SKIP 77

#define ALL_EVENTS (HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT)

/* Tier-1 runs with no device churn, so ABSENCE of callback invocations is
   checked with a short bounded settle window (there is no event to use as a
   barrier when the expectation is "no events at all"). */
#define SETTLE_MS 1000

/* How long the two churn threads of T16 keep registering/deregistering. */
#define CHURN_MS 2000

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
/* Shared callback state                                               */

static test_mutex g_lock;
static int g_cb_invocations;     /* every invocation of cb_record */
static int g_exit_returned;      /* set by T5 right after hid_exit() returns */
static int g_fired_after_exit;   /* cb_record ran after g_exit_returned was set */

static int HID_API_CALL cb_record(hid_hotplug_callback_handle callback_handle,
                                  struct hid_device_info *device,
                                  hid_hotplug_event event,
                                  void *user_data)
{
	(void)callback_handle;
	(void)device;
	(void)event;
	(void)user_data;
	test_mutex_lock(&g_lock);
	g_cb_invocations++;
	if (g_exit_returned)
		g_fired_after_exit = 1;
	test_mutex_unlock(&g_lock);
	return 0;
}

static int HID_API_CALL cb_noop(hid_hotplug_callback_handle callback_handle,
                                struct hid_device_info *device,
                                hid_hotplug_event event,
                                void *user_data)
{
	(void)callback_handle;
	(void)device;
	(void)event;
	(void)user_data;
	return 0;
}

/* ------------------------------------------------------------------ */
/* T4 doubles as the support probe: hid_hotplug_register_callback() as
   the very FIRST library call must initialize the library implicitly
   and succeed. If it fails, this backend/host has no hotplug support
   (e.g. a libusb without LIBUSB_CAP_HAS_HOTPLUG) and the whole test is
   skipped: the libusb backend checks the capability before validating
   arguments, so not even T1 is meaningful without support. */
static int t4_implicit_init_probe(int *supported)
{
	hid_hotplug_callback_handle handle = -123;
	int rc;

	*supported = 0;

	step("register as the very first library call");
	rc = hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_noop, NULL, &handle);
	if (rc != 0) {
		printf("    hotplug reported unsupported here (rc=%d) - skipping\n", rc);
		fflush(stdout);
		return 0;
	}
	*supported = 1;

	CHECK(handle > 0);
	step("deregister the probe callback");
	CHECK(hid_hotplug_deregister_callback(handle) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T1: invalid registration arguments -> -1, *callback_handle zeroed,
   and a retrievable (non-NULL) global error string. The exact error
   text is backend-specific, so only its existence is asserted. */

static int t1_check_invalid(unsigned short vid, unsigned short pid,
                            int events, int flags, hid_hotplug_callback_fn cb)
{
	hid_hotplug_callback_handle handle = 12345; /* poisoned: must be zeroed */
	int rc = hid_hotplug_register_callback(vid, pid, events, flags, cb, NULL, &handle);
	CHECK(rc == -1);
	CHECK(handle == 0);
	CHECK(hid_error(NULL) != NULL);
	return 0;
}

static int t1_arg_validation(void)
{
	step("NULL callback");
	if (t1_check_invalid(0, 0, ALL_EVENTS, 0, NULL) != 0)
		return -1;

	step("events == 0");
	if (t1_check_invalid(0, 0, 0, 0, cb_noop) != 0)
		return -1;

	step("unknown events bits");
	if (t1_check_invalid(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | (1 << 10), 0, cb_noop) != 0)
		return -1;

	step("unknown flags bits");
	if (t1_check_invalid(0, 0, ALL_EVENTS, (1 << 10), cb_noop) != 0)
		return -1;

	return 0;
}

/* ------------------------------------------------------------------ */
/* T2: handles are positive, never 0, and not reused while the library
   remains initialized (a later registration gets a different handle;
   hidapi.h promises uniqueness, not monotonicity). */
static int t2_handle_properties(hid_hotplug_callback_handle *out_stale)
{
	hid_hotplug_callback_handle h1 = 0, h2 = 0;

	step("register/deregister twice");
	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_noop, NULL, &h1) == 0);
	CHECK(h1 > 0);
	CHECK(hid_hotplug_deregister_callback(h1) == 0);

	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_noop, NULL, &h2) == 0);
	CHECK(h2 > 0);
	CHECK(h2 != h1); /* handles are not reused while initialized */
	CHECK(hid_hotplug_deregister_callback(h2) == 0);

	*out_stale = h2; /* a genuine but no-longer-registered handle for T3 */
	return 0;
}

/* ------------------------------------------------------------------ */
/* T3: deregistering 0, negative, never-issued and already-deregistered
   handles fails with -1, sets an error string and leaves a
   still-registered callback untouched. */
static int t3_stale_handles(hid_hotplug_callback_handle stale)
{
	hid_hotplug_callback_handle live = 0;

	step("register a live callback");
	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_noop, NULL, &live) == 0);
	CHECK(live > 0);

	step("deregister invalid handles");
	CHECK(hid_hotplug_deregister_callback(0) == -1);
	CHECK(hid_error(NULL) != NULL);
	CHECK(hid_hotplug_deregister_callback(-1) == -1);
	CHECK(hid_error(NULL) != NULL);
	CHECK(hid_hotplug_deregister_callback(live + 1000) == -1); /* never issued */
	CHECK(hid_error(NULL) != NULL);
	CHECK(hid_hotplug_deregister_callback(stale) == -1); /* already deregistered */
	CHECK(hid_error(NULL) != NULL);

	step("the live callback is unaffected");
	CHECK(hid_hotplug_deregister_callback(live) == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T5: hid_exit() with callbacks still registered returns (a hang is
   caught by the CTest timeout), invalidates the handles, and no
   callback fires after it returned. Then the register->immediate-exit
   teardown race is stressed in a loop. */
static int t5_hid_exit_teardown(void)
{
	hid_hotplug_callback_handle ha = 0, hb = 0;
	int i;

	step("register two callbacks");
	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_record, NULL, &ha) == 0);
	CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_record, NULL, &hb) == 0);

	step("hid_exit() with callbacks still registered");
	CHECK(hid_exit() == 0);
	test_mutex_lock(&g_lock);
	g_exit_returned = 1;
	test_mutex_unlock(&g_lock);

	step("old handles are invalid after re-init");
	CHECK(hid_init() == 0);
	CHECK(hid_hotplug_deregister_callback(ha) == -1);
	CHECK(hid_hotplug_deregister_callback(hb) == -1);

	step("no callback fires after hid_exit returned (settle window)");
	test_sleep_ms(SETTLE_MS);
	test_mutex_lock(&g_lock);
	i = g_fired_after_exit;
	test_mutex_unlock(&g_lock);
	CHECK(i == 0);

	step("register -> immediate hid_exit stress loop");
	for (i = 0; i < 50; i++) {
		hid_hotplug_callback_handle h = 0;
		CHECK(hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_record, NULL, &h) == 0);
		CHECK(h > 0);
		CHECK(hid_exit() == 0);
	}
	test_mutex_lock(&g_lock);
	g_exit_returned = 0;
	test_mutex_unlock(&g_lock);
	CHECK(hid_init() == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* T16: two threads register/deregister wildcard callbacks concurrently
   (the hotplug API is documented thread-safe). Pass = no crash, no
   hang (join timeout), no failed call. The threads never call
   hid_error(NULL): the global error string is the one part of the
   hotplug API the application must serialize itself. */

typedef struct churn_ctx {
	volatile int stop;
	long iterations;
	long failures;
} churn_ctx;

static void churn_thread_fn(void *arg)
{
	churn_ctx *ctx = (churn_ctx *)arg;

	while (!ctx->stop) {
		hid_hotplug_callback_handle h = 0;
		if (hid_hotplug_register_callback(0, 0, ALL_EVENTS, 0, cb_noop, NULL, &h) != 0) {
			ctx->failures++;
			continue;
		}
		if (h <= 0)
			ctx->failures++;
		if (hid_hotplug_deregister_callback(h) != 0)
			ctx->failures++;
		ctx->iterations++;
	}
}

static int t16_thread_churn(void)
{
	test_thread threads[2];
	churn_ctx ctxs[2];
	int i;

	memset(ctxs, 0, sizeof(ctxs));

	step("start two register/deregister churn threads");
	CHECK(test_thread_start(&threads[0], churn_thread_fn, &ctxs[0]) == 0);
	if (test_thread_start(&threads[1], churn_thread_fn, &ctxs[1]) != 0) {
		ctxs[0].stop = 1;
		(void)test_thread_join_timeout(&threads[0], 10000);
		CHECK(!"failed to start the second churn thread");
	}

	test_sleep_ms(CHURN_MS);
	ctxs[0].stop = 1;
	ctxs[1].stop = 1;

	step("join the churn threads");
	CHECK(test_thread_join_timeout(&threads[0], 30000) == 0);
	CHECK(test_thread_join_timeout(&threads[1], 30000) == 0);

	for (i = 0; i < 2; i++) {
		printf("    thread %d: %ld iterations, %ld failures\n",
		       i, ctxs[i].iterations, ctxs[i].failures);
		fflush(stdout);
		CHECK(ctxs[i].failures == 0);
		CHECK(ctxs[i].iterations > 0);
	}
	return 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
	hid_hotplug_callback_handle stale = 0;
	int supported = 0;
	int rc;

	test_mutex_init(&g_lock);

	/* NOTE: no hid_init() here on purpose: T4 requires that the hotplug
	   registration is the very first library call. */

	printf("running hotplug API tests...\n");
	fflush(stdout);

	printf("T4: implicit init (register as first library call)\n");
	fflush(stdout);
	rc = t4_implicit_init_probe(&supported);
	if (!supported) {
		test_mutex_destroy(&g_lock);
		return EXIT_SKIP;
	}
	report("T4 implicit_init", rc);

	printf("T1: registration argument validation\n");
	fflush(stdout);
	report("T1 arg_validation", t1_arg_validation());

	printf("T2: callback handle properties\n");
	fflush(stdout);
	report("T2 handle_properties", t2_handle_properties(&stale));

	printf("T3: stale/unknown handle deregistration\n");
	fflush(stdout);
	report("T3 stale_handles", t3_stale_handles(stale));

	printf("T5: hid_exit teardown with registered callbacks\n");
	fflush(stdout);
	report("T5 hid_exit_teardown", t5_hid_exit_teardown());

	printf("T16: register/deregister thread churn\n");
	fflush(stdout);
	report("T16 thread_churn", t16_thread_churn());

	hid_exit();
	test_mutex_destroy(&g_lock);

	printf("%s hotplug_api (%d failed checks)\n",
	       g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
