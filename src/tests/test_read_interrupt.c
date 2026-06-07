/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Unit test for the hid_read_interrupt() / hid_is_read_interrupted() /
 hid_read_clear_interrupt() API.

 The scenarios use only the public HIDAPI API plus the backend-agnostic
 test_virtual_device interface and the small cross-platform helpers in
 test_platform.h, so the same test runs against any backend that provides
 a virtual-device implementation (Linux uhid, Windows UMDF, ...).

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

/* Must precede any system header so pthread_timedjoin_np is declared (Linux). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#include <hidapi.h>

#include "test_platform.h"
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

static void reader_fn(void *arg)
{
	struct reader_ctx *c = (struct reader_ctx *)arg;
	unsigned char buf[64];
	c->result = hid_read_timeout(c->handle, buf, sizeof(buf), c->timeout_ms);
}

/* --- Individual scenarios ------------------------------------------------ */

/* Sanity: triggering a scenario makes the device replay a report that a
   normal read returns. */
static int test_normal_read(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	const unsigned char expected[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
	unsigned char buf[64];
	int n;

	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_A) >= 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(expected));
	CHECK(memcmp(buf, expected, sizeof(expected)) == 0);
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

	long long t0 = test_now_ms();
	unsigned char buf[64];
	int n = hid_read_timeout(h, buf, sizeof(buf), -1);   /* blocking */
	long long elapsed = test_now_ms() - t0;

	CHECK(n == -1);
	CHECK(elapsed < 1000);   /* must not have blocked */

	hid_close(h);
	return 0;
}

/* THE regression test: a blocking hid_read_timeout(-1) on one thread must be
   woken by hid_read_interrupt() from another thread. */
static int test_blocking_read_interrupted_from_thread(test_virtual_device *vdev)
{
	hid_device *h = test_virtual_device_open_hidapi(vdev, 5000);
	CHECK(h != NULL);
	drain(h);

	struct reader_ctx ctx;
	ctx.handle = h;
	ctx.timeout_ms = -1;   /* blocking */
	ctx.result = -999;

	test_thread reader;
	CHECK(test_thread_start(&reader, reader_fn, &ctx) == 0);

	/* Let the reader settle into the blocking read (no scenario triggered, so
	   it can only be released by the interrupt). */
	test_sleep_ms(200);

	CHECK(hid_read_interrupt(h) == 0);

	if (test_thread_join_timeout(&reader, 3000) != 0) {
		printf("    CHECK FAILED: blocking read was not interrupted (hang)\n");
		return -1;   /* reader still holds h; leak handle to force failure */
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

	test_thread reader;
	CHECK(test_thread_start(&reader, reader_fn, &ctx) == 0);

	test_sleep_ms(200);

	long long t0 = test_now_ms();
	CHECK(hid_read_interrupt(h) == 0);
	int joined = test_thread_join_timeout(&reader, 3000);   /* well under the 5s read timeout */
	long long elapsed = test_now_ms() - t0;

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
		long long t0 = test_now_ms();
		int n = hid_read_timeout(h, buf, sizeof(buf), 200);
		long long elapsed = test_now_ms() - t0;
		CHECK(n == -1);
		CHECK(elapsed < 200);   /* returns immediately, doesn't wait out the timeout */
	}

	hid_close(h);
	return 0;
}

/* After clearing the interrupt, reads resume working: a timed read with no
   data returns 0 (not -1), and a triggered report is then delivered. */
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

	const unsigned char expected[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_B;
	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_B) >= 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(expected));
	CHECK(memcmp(buf, expected, sizeof(expected)) == 0);

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
		printf("SKIP: virtual HID device unavailable. Skipping read-interrupt tests.\n");
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

	/* Confirm the device is reachable through HIDAPI before running scenarios;
	   if not, treat as skip (driver not installed / environment issue). */
	{
		hid_device *probe = test_virtual_device_open_hidapi(vdev, 5000);
		if (!probe) {
			printf("SKIP: virtual device did not appear via HIDAPI enumeration. Skipping.\n");
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
