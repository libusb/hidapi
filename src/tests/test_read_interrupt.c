/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Unit test for the hid_read_interrupt() / hid_is_read_interrupted() /
 hid_read_clear_interrupt() API.

 The scenarios use only the public HIDAPI API and the backend-agnostic
 test_virtual_device interface, so the same test can be reused for any
 backend that provides a virtual-device implementation.

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

/* A live reader may still use its stack context and device. End the process
   without running subsequent scenarios or HIDAPI cleanup on a timeout. */
static void fatal_timeout(const char *what)
{
	fprintf(stderr, "FATAL: %s timed out\n", what);
	fflush(stdout);
	fflush(stderr);
	exit(EXIT_FAILURE);
}

static int drain(hid_device *h)
{
	unsigned char buf[64];
	long long deadline = test_now_ms() + 1000;
	int n;
	do {
		n = hid_read_timeout(h, buf, sizeof(buf), 0);
		if (n == 0)
			return 0;
	} while (n > 0 && test_now_ms() < deadline);
	return -1;
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

/* Sanity: a triggered input report is delivered by a normal read. */
static int test_normal_read(test_virtual_device *vdev, hid_device *h)
{
	const unsigned char payload[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
	unsigned char buf[64];
	int n;

	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_A) >= 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);
	CHECK(hid_is_read_interrupted(h) == 0);

	return 0;
}

/* hid_read_interrupt() before any read makes the next read return -1 at once
   without blocking, and the state is observable via hid_is_read_interrupted(). */
static int test_interrupt_before_read(test_virtual_device *vdev, hid_device *h)
{
	(void)vdev;

	CHECK(hid_is_read_interrupted(h) == 0);
	hid_read_interrupt(h);
	CHECK(hid_is_read_interrupted(h) == 1);

	long long t0 = test_now_ms();
	unsigned char buf[64];
	int n = hid_read_timeout(h, buf, sizeof(buf), -1);   /* blocking */
	long long elapsed = test_now_ms() - t0;

	CHECK(n == -1);
	CHECK(elapsed < 1000);   /* must not have blocked */

	return 0;
}

/* A blocking read on one thread is woken by an interrupt on another. */
static int test_blocking_read_interrupted_from_thread(test_virtual_device *vdev, hid_device *h)
{
	(void)vdev;

	struct reader_ctx ctx;
	ctx.handle = h;
	ctx.timeout_ms = -1;   /* blocking */
	ctx.result = -999;

	test_thread reader;
	CHECK(test_thread_start(&reader, reader_fn, &ctx) == 0);

	/* Let the reader settle into the blocking read (no input is triggered,
	   so it can only be released by the interrupt). */
	test_sleep_ms(200);

	hid_read_interrupt(h);

	if (test_thread_join_timeout(&reader, 3000) != 0)
		fatal_timeout("blocking reader");

	CHECK(ctx.result == -1);
	CHECK(hid_is_read_interrupted(h) == 1);

	return 0;
}

/* A read with a long finite timeout is also interrupted promptly. */
static int test_timed_read_interrupted_from_thread(test_virtual_device *vdev, hid_device *h)
{
	(void)vdev;

	struct reader_ctx ctx;
	ctx.handle = h;
	ctx.timeout_ms = 5000;
	ctx.result = -999;

	test_thread reader;
	CHECK(test_thread_start(&reader, reader_fn, &ctx) == 0);

	test_sleep_ms(200);

	long long t0 = test_now_ms();
	hid_read_interrupt(h);
	int joined = test_thread_join_timeout(&reader, 3000);   /* well under the 5s read timeout */
	long long elapsed = test_now_ms() - t0;

	if (joined != 0)
		fatal_timeout("timed reader");
	CHECK(ctx.result == -1);
	CHECK(elapsed < 3000);

	return 0;
}

/* Interrupted reads leave queued reports available after a clear, with
   blocking, nonblocking and timed reads. Feature reports still work while the
   read pipeline is interrupted. */
static int test_interrupt_preserves_queued_input(test_virtual_device *vdev, hid_device *h)
{
	const unsigned char payload[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_A;
	/* Blocking first: if the queued report were consumed, the read returns
	   it instead of blocking. */
	const int timeouts[] = { -1, 0, 1000, -1 };
	unsigned char buf[64];
	int n;

	hid_read_interrupt(h);
	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_A) >= 0);
	/* Allow the backend's input queue to receive the report. */
	test_sleep_ms(200);

	for (size_t i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); i++) {
		long long t0 = test_now_ms();
		n = hid_read_timeout(h, buf, sizeof(buf), timeouts[i]);
		long long elapsed = test_now_ms() - t0;
		CHECK(n == -1);
		CHECK(elapsed < 500);
	}
	CHECK(hid_is_read_interrupted(h) == 1);
	hid_read_clear_interrupt(h);
	CHECK(hid_is_read_interrupted(h) == 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);
	return 0;
}

/* After clearing the interrupt, reads resume working: a timed read with no
   data returns 0 (not -1), and a triggered report is then delivered. */
static int test_clear_resumes_reads(test_virtual_device *vdev, hid_device *h)
{
	hid_read_interrupt(h);
	CHECK(hid_is_read_interrupted(h) == 1);
	hid_read_clear_interrupt(h);
	CHECK(hid_is_read_interrupted(h) == 0);

	unsigned char buf[64];
	int n = hid_read_timeout(h, buf, sizeof(buf), 20);   /* no data queued */
	CHECK(n == 0);

	const unsigned char payload[TEST_VDEV_REPORT_SIZE] = TEST_VDEV_INPUT_B;
	CHECK(test_virtual_device_trigger(vdev, h, TEST_VDEV_CMD_EMIT_B) >= 0);
	n = hid_read_timeout(h, buf, sizeof(buf), 2000);
	CHECK(n == (int)sizeof(payload));
	CHECK(memcmp(buf, payload, sizeof(payload)) == 0);

	return 0;
}

/* hid_read_interrupt() is idempotent and a single clear undoes it. */
static int test_idempotent(test_virtual_device *vdev, hid_device *h)
{
	(void)vdev;

	hid_read_interrupt(h);
	hid_read_interrupt(h);   /* twice */
	CHECK(hid_is_read_interrupted(h) == 1);
	hid_read_clear_interrupt(h);
	hid_read_clear_interrupt(h);   /* clearing a cleared state is harmless */
	CHECK(hid_is_read_interrupted(h) == 0);

	/* Reads work again afterwards. */
	unsigned char buf[64];
	CHECK(hid_read_timeout(h, buf, sizeof(buf), 0) == 0);

	return 0;
}

#ifdef _WIN32
struct pooled_reader_ctx {
	struct reader_ctx read;
	HANDLE returned;
	HANDLE release;
};

static void pooled_reader_fn(void *arg)
{
	struct pooled_reader_ctx *ctx = (struct pooled_reader_ctx *)arg;
	reader_fn(&ctx->read);
	SetEvent(ctx->returned);
	WaitForSingleObject(ctx->release, INFINITE);
}

/* Close on the controller while the issuing thread stays alive, so thread
   rundown cannot cancel its pending overlapped read on the library's behalf. */
static int test_close_with_pooled_reader(test_virtual_device *vdev)
{
	struct pooled_reader_ctx ctx;
	test_thread reader;
	hid_device *h = NULL;
	hid_device *control = NULL;
	int result = -1;

	memset(&ctx, 0, sizeof(ctx));
	ctx.returned = CreateEvent(NULL, TRUE, FALSE, NULL);
	ctx.release = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ctx.returned || !ctx.release)
		goto cleanup;
	h = test_virtual_device_open_hidapi(vdev, 5000);
	control = test_virtual_device_open_hidapi(vdev, 5000);
	if (!h || !control)
		goto cleanup;
	/* The worker must issue the first read, including any nonblocking read. */
	ctx.read.handle = h;
	ctx.read.timeout_ms = -1;
	ctx.read.result = -999;
	if (test_thread_start(&reader, pooled_reader_fn, &ctx) != 0)
		goto cleanup;
	test_sleep_ms(200);
	hid_read_interrupt(h);
	if (WaitForSingleObject(ctx.returned, 3000) != WAIT_OBJECT_0)
		fatal_timeout("pooled reader return");

	hid_close(h);
	h = NULL;
	/* Exercise report delivery while the read's issuing thread is alive. */
	if (test_virtual_device_trigger(vdev, control, TEST_VDEV_CMD_EMIT_A) >= 0 &&
	    ctx.read.result == -1)
		result = 0;
	test_sleep_ms(100);
	SetEvent(ctx.release);
	if (test_thread_join_timeout(&reader, 3000) != 0)
		fatal_timeout("pooled reader release");

cleanup:
	if (h)
		hid_close(h);
	if (control)
		hid_close(control);
	if (ctx.returned)
		CloseHandle(ctx.returned);
	if (ctx.release)
		CloseHandle(ctx.release);
	return result;
}
#endif

/* --- Harness ------------------------------------------------------------- */

struct test_case {
	const char *name;
	int (*fn)(test_virtual_device *, hid_device *);
};

int main(void)
{
	test_virtual_device *vdev = NULL;
	int rc;
	if (hid_init() != 0) {
		printf("FATAL: hid_init() failed\n");
		return EXIT_FAILURE;
	}
	rc = test_virtual_device_create(&vdev, TEST_VID, TEST_PID, TEST_SERIAL);
	if (rc == TEST_VDEV_UNAVAILABLE) {
		printf("SKIP: virtual HID device unavailable\n");
		hid_exit();
		return EXIT_SKIP;
	}
	if (rc != TEST_VDEV_OK || !vdev) {
		printf("FATAL: could not create virtual HID device\n");
		hid_exit();
		return EXIT_FAILURE;
	}

	/* Make sure the device is reachable through HIDAPI before running the
	   scenarios; if not, treat as skip (environment issue). */
	{
		hid_device *probe = test_virtual_device_open_hidapi(vdev, 5000);
		if (!probe) {
			printf("SKIP: virtual device did not enumerate via HIDAPI\n");
			test_virtual_device_destroy(vdev);
			hid_exit();
			return EXIT_SKIP;
		}
		hid_close(probe);
	}

	struct test_case tests[] = {
		{"normal_read",                           test_normal_read},
		{"interrupt_before_read",                 test_interrupt_before_read},
		{"blocking_read_interrupted_from_thread", test_blocking_read_interrupted_from_thread},
		{"timed_read_interrupted_from_thread",    test_timed_read_interrupted_from_thread},
		{"interrupt_preserves_queued_input",      test_interrupt_preserves_queued_input},
		{"clear_resumes_reads",                   test_clear_resumes_reads},
		{"idempotent",                            test_idempotent},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		hid_device *h;
		int r;
		printf("RUN %s\n", tests[i].name);
		fflush(stdout);
		h = test_virtual_device_open_hidapi(vdev, 5000);
		r = h && drain(h) == 0 ? tests[i].fn(vdev, h) : -1;
		if (h)
			hid_close(h);
		printf("%s %s\n", r == 0 ? "PASS" : "FAIL", tests[i].name);
		if (r != 0)
			g_failures++;
	}
#ifdef _WIN32
	printf("RUN close_with_pooled_reader\n");
	fflush(stdout);
	rc = test_close_with_pooled_reader(vdev);
	printf("%s close_with_pooled_reader\n", rc == 0 ? "PASS" : "FAIL");
	if (rc != 0)
		g_failures++;
	n++;
#endif

	printf("\n%d/%zu failed\n", g_failures, n);

	test_virtual_device_destroy(vdev);
	hid_exit();

	return g_failures ? 1 : 0;
}
