/*******************************************************
 HIDAPI - FreeBSD read-interruption tests.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <sys/socket.h>

/* Use the production backend with a socket transport instead of a HID device.
   Including it exposes allocation and wake descriptors for lifecycle checks. */
#include "../hid.c"
#include "../../src/tests/test_platform.h"

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: %s (line %d)\n", #condition, __LINE__); \
		fflush(stdout); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

struct fixture {
	hid_device *dev;
	int peer;
};

static struct fixture open_fixture(void)
{
	struct fixture f;
	int transport[2];

	f.dev = new_hid_device();
	CHECK(f.dev != NULL);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, transport) == 0);
	f.dev->device_handle = transport[0];
	f.peer = transport[1];
	for (size_t i = 0; i < 2; i++) {
		int flags;
		CHECK(f.dev->interrupt_pipe[i] >= 0);
		flags = fcntl(f.dev->interrupt_pipe[i], F_GETFD);
		CHECK(flags >= 0 && (flags & FD_CLOEXEC) != 0);
		flags = fcntl(f.dev->interrupt_pipe[i], F_GETFL);
		CHECK(flags >= 0 && (flags & O_NONBLOCK) != 0);
	}
	CHECK(hid_is_read_interrupted(f.dev) == 0);
	return f;
}

static void check_closed(int fd)
{
	errno = 0;
	CHECK(fcntl(fd, F_GETFD) == -1);
	CHECK(errno == EBADF);
}

static void close_fixture(struct fixture *f)
{
	int device = f->dev->device_handle;
	int wake_read = f->dev->interrupt_pipe[0];
	int wake_write = f->dev->interrupt_pipe[1];

	hid_close(f->dev);
	f->dev = NULL;
	check_closed(device);
	check_closed(wake_read);
	check_closed(wake_write);
	CHECK(close(f->peer) == 0);
	f->peer = -1;
}

static void expect_readable(int fd)
{
	struct pollfd ready;
	ready.fd = fd;
	ready.events = POLLIN;
	ready.revents = 0;
	CHECK(poll(&ready, 1, 3000) == 1);
	CHECK((ready.revents & POLLIN) != 0);
}

static void check_interrupted(hid_device *dev, int timeout_ms)
{
	unsigned char data;
	long long start = test_now_ms();
	CHECK(hid_read_timeout(dev, &data, sizeof(data), timeout_ms) == -1);
	CHECK(test_now_ms() - start < 1000);
	CHECK(hid_is_read_interrupted(dev) == 1);
	CHECK(wcsstr(hid_read_error(dev), L"interrupted") != NULL);
}

static void test_preinterrupt_and_queued_input(void)
{
	struct fixture f = open_fixture();
	unsigned char sent = 0x5a;
	unsigned char received = 0;

	CHECK(write(f.peer, &sent, sizeof(sent)) == (ssize_t)sizeof(sent));
	expect_readable(f.dev->device_handle);
	hid_read_interrupt(f.dev);
	check_interrupted(f.dev, -1);
	check_interrupted(f.dev, 5000);
	check_interrupted(f.dev, 0);
	CHECK(hid_read_clear_interrupt(f.dev) == 0);
	CHECK(hid_is_read_interrupted(f.dev) == 0);
	CHECK(hid_read_timeout(f.dev, &received, sizeof(received), 1000) == (int)sizeof(received));
	CHECK(received == sent);
	CHECK(wcscmp(hid_read_error(f.dev), L"Success") == 0);
	CHECK(hid_read_timeout(f.dev, &received, sizeof(received), 0) == 0);
	close_fixture(&f);
}

struct reader_context {
	hid_device *dev;
	int timeout_ms;
	int ready;
	int result;
};

static void reader(void *arg)
{
	struct reader_context *ctx = (struct reader_context *)arg;
	unsigned char byte = 1;
	CHECK(write(ctx->ready, &byte, sizeof(byte)) == (ssize_t)sizeof(byte));
	ctx->result = hid_read_timeout(ctx->dev, &byte, sizeof(byte), ctx->timeout_ms);
}

static void test_wake_reader(int timeout_ms)
{
	struct fixture f = open_fixture();
	struct reader_context ctx;
	test_thread thread;
	int ready[2];
	unsigned char byte;
	long long start;

	CHECK(pipe2(ready, O_CLOEXEC | O_NONBLOCK) == 0);
	ctx.dev = f.dev;
	ctx.timeout_ms = timeout_ms;
	ctx.ready = ready[1];
	ctx.result = -999;
	CHECK(test_thread_start(&thread, reader, &ctx) == 0);
	expect_readable(ready[0]);
	CHECK(read(ready[0], &byte, sizeof(byte)) == (ssize_t)sizeof(byte));
	test_sleep_ms(50);
	CHECK(__atomic_load_n(&thread.done, __ATOMIC_ACQUIRE) == 0);

	start = test_now_ms();
	hid_read_interrupt(f.dev);
	/* Failure exits immediately; the active reader retains a valid context. */
	CHECK(test_thread_join_timeout(&thread, 3000) == 0);
	CHECK(test_now_ms() - start < 3000);
	CHECK(ctx.result == -1);
	CHECK(hid_is_read_interrupted(f.dev) == 1);
	CHECK(wcsstr(hid_read_error(f.dev), L"interrupted") != NULL);
	CHECK(close(ready[0]) == 0);
	CHECK(close(ready[1]) == 0);
	close_fixture(&f);
}

static void test_full_wake_pipe(void)
{
	struct fixture f = open_fixture();
	unsigned char fill[4096] = {0};
	unsigned char byte;
	struct pollfd wake;
	long long deadline = test_now_ms() + 3000;
	ssize_t count;

	hid_read_interrupt(f.dev);
	/* A full wake pipe represents repeated interrupts without a clear. */
	do {
		CHECK(test_now_ms() < deadline);
		count = write(f.dev->interrupt_pipe[1], fill, sizeof(fill));
	} while (count > 0 || (count == -1 && errno == EINTR));
	CHECK(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	/* Fill any space too small for the bulk write, down to the last byte. */
	do {
		CHECK(test_now_ms() < deadline);
		count = write(f.dev->interrupt_pipe[1], fill, 1);
	} while (count > 0 || (count == -1 && errno == EINTR));
	CHECK(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	deadline = test_now_ms() + 3000;
	for (int i = 0; i < 256; i++) {
		hid_read_interrupt(f.dev);
		CHECK(test_now_ms() < deadline);
	}
	check_interrupted(f.dev, -1);
	CHECK(hid_read_clear_interrupt(f.dev) == 0);
	CHECK(hid_read_clear_interrupt(f.dev) == 0);
	CHECK(hid_is_read_interrupted(f.dev) == 0);
	wake.fd = f.dev->interrupt_pipe[0];
	wake.events = POLLIN;
	wake.revents = 0;
	CHECK(poll(&wake, 1, 0) == 0);
	CHECK(hid_read_timeout(f.dev, &byte, sizeof(byte), 20) == 0);
	close_fixture(&f);
}

static void test_write_while_interrupted(void)
{
	struct fixture f = open_fixture();
	const unsigned char sent = 0xa5;
	unsigned char received = 0;

	hid_read_interrupt(f.dev);
	check_interrupted(f.dev, 0);
	CHECK(hid_write(f.dev, &sent, sizeof(sent)) == (int)sizeof(sent));
	expect_readable(f.peer);
	CHECK(read(f.peer, &received, sizeof(received)) == (ssize_t)sizeof(received));
	CHECK(received == sent);
	CHECK(wcscmp(hid_error(f.dev), L"Success") == 0);
	CHECK(wcsstr(hid_read_error(f.dev), L"interrupted") != NULL);
	CHECK(hid_is_read_interrupted(f.dev) == 1);
	close_fixture(&f);
}

/* Capture the next three free descriptor slots, covering two wake descriptors
   and the transport descriptor allocated by hid_open_path(). */
static void next_descriptors(int descriptors[3])
{
	for (size_t i = 0; i < 3; i++) {
		descriptors[i] = dup(STDERR_FILENO);
		CHECK(descriptors[i] >= 0);
	}
	for (size_t i = 0; i < 3; i++)
		CHECK(close(descriptors[i]) == 0);
}

static void test_open_failure_cleanup(void)
{
	int before[3], after[3];
	hid_device *dev;
	int wake_read, wake_write;

	/* A partially initialized handle owns only its wake descriptors. */
	dev = new_hid_device();
	CHECK(dev != NULL);
	CHECK(dev->device_handle == -1);
	wake_read = dev->interrupt_pipe[0];
	wake_write = dev->interrupt_pipe[1];
	hid_close(dev);
	check_closed(wake_read);
	check_closed(wake_write);

	next_descriptors(before);
	for (int i = 0; i < 16; i++) {
		CHECK(hid_open_path("") == NULL);
		next_descriptors(after);
		CHECK(memcmp(before, after, sizeof(before)) == 0);
		/* A valid descriptor rejected by the HID ioctl follows another path. */
		CHECK(hid_open_path("/dev/null") == NULL);
		next_descriptors(after);
		CHECK(memcmp(before, after, sizeof(before)) == 0);
	}
}

int main(void)
{
	CHECK(hid_init() == 0);
	test_preinterrupt_and_queued_input();
	puts("PASS preinterrupt_and_queued_input");
	test_wake_reader(-1);
	puts("PASS blocking_read_interrupted");
	test_wake_reader(5000);
	puts("PASS timed_read_interrupted");
	test_full_wake_pipe();
	puts("PASS full_wake_pipe_and_idempotency");
	test_write_while_interrupted();
	puts("PASS write_while_interrupted");
	test_open_failure_cleanup();
	puts("PASS descriptor_cleanup");
	CHECK(hid_exit() == 0);
	return EXIT_SUCCESS;
}
