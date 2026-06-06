/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Unit test for the hid_read_interrupt() / hid_is_read_interrupted() /
 hid_read_clear_interrupt() API.

 The scenarios use only the public HIDAPI API and the backend-agnostic
 test_virtual_device interface, so the same test can be reused for any
 backend that provides a virtual-device implementation. Today the only
 provider is Linux uhid (see src/tests/CMakeLists.txt).

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#define _GNU_SOURCE /* for pthread_timedjoin_np */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <hidapi.h>

#include "test_virtual_device.h"

/* Exit codes: 0 = pass, 1 = fail, 77 = skipped (CTest SKIP_RETURN_CODE). */
#define EXIT_SKIP 77

#define TEST_VID 0xF1D0
#define TEST_PID 0x9001
#define TEST_SERIAL "HIDAPI-READ-INTERRUPT-TEST"

static int g_failures = 0;

#define CHECK(expr) do { \
	if (!(expr)) { \
		printf("    CHECK FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
		return -1; \
	} \
} while (0)

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void drain(hid_device *h)
{
	unsigned char buf[64];
	while (hid_read_timeout(h, buf, sizeof(buf), 0) > 0) { /* discard */ }
}

struct reader_ctx {
	hid_device *handle;
	int timeout_ms;
	int result;
};

static void *reader_fn(void *arg)
{
	struct reader_ctx *c = (struct reader_ctx *)arg;
	unsigned char buf[64];
	c->result = hid_read_timeout(c->handle, buf, sizeof(buf), c->timeout_ms);
	return NULL;
}

/* Join 'thread' but give up after timeout_ms. Returns 0 on join, -1 on timeout. */
static int join_with_timeout(pthread_t thread, int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_timedjoin_np(thread, NULL, &ts) == 0 ? 0 : -1;
}

/* --- Individual scenarios ------------------------------------------------ */

/* Sanity: an injected input report is delivered by a normal read. */
static int test_normal_read(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	const unsigned char payload[TEST_VDEV_REPORT_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
	unsigned char buf[64];
	int n;

	CHECK(test_virtual_device_send_input(vdev, payload, sizeof(payload)) == 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);
	CHECK(hid_is_read_interrupted(h) == 0);

	hid_close(h);
	return 0;
}

/* hid_read_interrupt() before any read makes the next read return -1 at once
   without blocking, and the state is observable via hid_is_read_interrupted(). */
static int test_interrupt_before_read(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	CHECK(hid_is_read_interrupted(h) == 0);
	CHECK(hid_read_interrupt(h) == 0);
	CHECK(hid_is_read_interrupted(h) == 1);

	long long t0 = now_ms();
	unsigned char buf[64];
	int n = hid_read_timeout(h, buf, sizeof(buf), -1);   /* blocking */
	long long elapsed = now_ms() - t0;

	CHECK(n == -1);
	CHECK(elapsed < 1000);   /* must not have blocked */

	hid_close(h);
	return 0;
}

/* THE regression test: a blocking hid_read_timeout(-1) on one thread must be
   woken by hid_read_interrupt() from another thread. Before the fix the
   hidraw blocking path never polled the interrupt eventfd and this hung. */
static int test_blocking_read_interrupted_from_thread(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	struct reader_ctx ctx;
	ctx.handle = h;
	ctx.timeout_ms = -1;   /* blocking */
	ctx.result = -999;

	pthread_t reader;
	CHECK(pthread_create(&reader, NULL, reader_fn, &ctx) == 0);

	/* Let the reader settle into the blocking read (no input is injected,
	   so it can only be released by the interrupt). */
	struct timespec ts = {0, 200 * 1000000L};
	nanosleep(&ts, NULL);

	CHECK(hid_read_interrupt(h) == 0);

	if (join_with_timeout(reader, 3000) != 0) {
		/* Reader is stuck => hid_read_interrupt() failed to wake it. */
		printf("    CHECK FAILED: blocking read was not interrupted (hang)\n");
		/* The reader still holds 'h'; cannot safely close. Leak handle and
		   force the test failure. */
		return -1;
	}

	CHECK(ctx.result == -1);
	CHECK(hid_is_read_interrupted(h) == 1);

	hid_close(h);
	return 0;
}

/* A read with a long finite timeout is also interrupted promptly. */
static int test_timed_read_interrupted_from_thread(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	struct reader_ctx ctx;
	ctx.handle = h;
	ctx.timeout_ms = 5000;
	ctx.result = -999;

	pthread_t reader;
	CHECK(pthread_create(&reader, NULL, reader_fn, &ctx) == 0);

	struct timespec ts = {0, 200 * 1000000L};
	nanosleep(&ts, NULL);

	long long t0 = now_ms();
	CHECK(hid_read_interrupt(h) == 0);
	int joined = join_with_timeout(reader, 3000);   /* well under the 5s read timeout */
	long long elapsed = now_ms() - t0;

	if (joined != 0) {
		printf("    CHECK FAILED: timed read was not interrupted (hang)\n");
		return -1;
	}
	CHECK(ctx.result == -1);
	CHECK(elapsed < 3000);

	hid_close(h);
	return 0;
}

/* The interrupt is sticky: once set, repeated reads keep returning -1. */
static int test_interrupt_is_sticky(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	CHECK(hid_read_interrupt(h) == 0);

	unsigned char buf[64];
	for (int i = 0; i < 3; i++) {
		long long t0 = now_ms();
		int n = hid_read_timeout(h, buf, sizeof(buf), 200);
		long long elapsed = now_ms() - t0;
		CHECK(n == -1);
		CHECK(elapsed < 200);   /* returns immediately, doesn't wait out the timeout */
	}

	hid_close(h);
	return 0;
}

/* After clearing the interrupt, reads resume working: a timed read with no
   data returns 0 (not -1), and an injected report is then delivered. */
static int test_clear_resumes_reads(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	CHECK(hid_read_interrupt(h) == 0);
	CHECK(hid_is_read_interrupted(h) == 1);
	CHECK(hid_read_clear_interrupt(h) == 0);
	CHECK(hid_is_read_interrupted(h) == 0);

	unsigned char buf[64];
	int n = hid_read_timeout(h, buf, sizeof(buf), 0);   /* no data queued */
	CHECK(n == 0);

	const unsigned char payload[TEST_VDEV_REPORT_SIZE] = {9, 8, 7, 6, 5, 4, 3, 2};
	CHECK(test_virtual_device_send_input(vdev, payload, sizeof(payload)) == 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);

	hid_close(h);
	return 0;
}

/* hid_read_interrupt() is idempotent and a single clear undoes it. */
static int test_idempotent(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	CHECK(hid_read_interrupt(h) == 0);
	CHECK(hid_read_interrupt(h) == 0);   /* twice */
	CHECK(hid_is_read_interrupted(h) == 1);
	CHECK(hid_read_clear_interrupt(h) == 0);
	CHECK(hid_is_read_interrupted(h) == 0);

	/* Reads work again afterwards. */
	unsigned char buf[64];
	CHECK(hid_read_timeout(h, buf, sizeof(buf), 0) == 0);

	hid_close(h);
	return 0;
}

/* --- Harness ------------------------------------------------------------- */

struct test_case {
	const char *name;
	int (*fn)(test_virtual_device *);
};

int main(void)
{
	test_virtual_device *vdev = NULL;
	int rc = test_virtual_device_create(&vdev, TEST_VID, TEST_PID, TEST_SERIAL);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("SKIP: virtual HID device unavailable (on Linux this needs the "
		       "'uhid' module and root). Skipping read-interrupt tests.\n");
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK) {
		printf("FATAL: could not create virtual HID device\n");
		return 1;
	}

	if (hid_init() != 0) {
		printf("FATAL: hid_init() failed\n");
		test_virtual_device_destroy(vdev);
		return 1;
	}

	/* Make sure the device is reachable through HIDAPI before running the
	   scenarios; if not, treat as skip (environment issue). */
	{
		hid_device *probe = test_virtual_device_open_hidapi(vdev, 5000);
		if (!probe) {
			printf("SKIP: virtual device did not appear via HIDAPI enumeration "
			       "(udev/permission issue). Skipping.\n");
			hid_exit();
			test_virtual_device_destroy(vdev);
			return EXIT_SKIP;
		}
		hid_close(probe);
	}

	struct test_case tests[] = {
		{"normal_read",                           test_normal_read},
		{"interrupt_before_read",                 test_interrupt_before_read},
		{"blocking_read_interrupted_from_thread", test_blocking_read_interrupted_from_thread},
		{"timed_read_interrupted_from_thread",    test_timed_read_interrupted_from_thread},
		{"interrupt_is_sticky",                   test_interrupt_is_sticky},
		{"clear_resumes_reads",                   test_clear_resumes_reads},
		{"idempotent",                            test_idempotent},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int r = tests[i].fn(vdev);
		printf("%s %s\n", r == 0 ? "PASS" : "FAIL", tests[i].name);
		if (r != 0)
			g_failures++;
	}

	printf("\n%d/%zu failed\n", g_failures, n);

	hid_exit();
	test_virtual_device_destroy(vdev);

	return g_failures ? 1 : 0;
}
