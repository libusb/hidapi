/*
 * Regression test for the libusb backend read-thread startup
 * (https://github.com/libusb/hidapi/issues/842).
 *
 * hid_open_path() starts a read thread and then waits on a two-party startup
 * barrier. If the thread cannot be created, nothing ever releases that
 * barrier, so the open must fail instead of waiting.
 *
 * The test compiles the real libusb/hid.c (and through it the real
 * hidapi_thread_pthread.h) into this translation unit, and interposes:
 *   - pthread_create / pthread_join / pthread_barrier_wait, to inject a thread
 *     creation failure and observe the startup synchronization;
 *   - the few libusb calls hid_open_path() makes against a device, to present
 *     a single fake HID interface without any hardware.
 * Everything else is the unmodified library code.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include <libusb.h>

/* ---- Interposed pthread functions ----------------------------------------- */

static pthread_mutex_t counters_mutex = PTHREAD_MUTEX_INITIALIZER;
static int inject_create_failure = 0;
static int create_failed = 0;
static int create_calls = 0;
static int join_calls = 0;
static int barrier_calls = 0;
/* The hid_device passed to the read thread, i.e. the device being opened. */
static void *opened_dev = NULL;

static void fatal(const char *message)
{
	fprintf(stderr, "FAIL: %s\n", message);
	fflush(stderr);
	/* Don't run any further library code (or atexit handlers) from here. */
	_exit(1);
}

static int count_call(int *counter)
{
	int value;
	pthread_mutex_lock(&counters_mutex);
	value = ++*counter;
	pthread_mutex_unlock(&counters_mutex);
	return value;
}

static int read_counter(const int *counter)
{
	int value;
	pthread_mutex_lock(&counters_mutex);
	value = *counter;
	pthread_mutex_unlock(&counters_mutex);
	return value;
}

static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*func)(void *), void *arg)
{
	count_call(&create_calls);
	opened_dev = arg;
	if (inject_create_failure) {
		create_failed = 1;
		return EAGAIN;
	}
	return pthread_create(thread, attr, func, arg);
}

static int test_pthread_join(pthread_t thread, void **retval)
{
	if (create_failed)
		fatal("joined a read thread that was never created");
	count_call(&join_calls);
	return pthread_join(thread, retval);
}

static int test_pthread_barrier_wait(pthread_barrier_t *barrier)
{
	/* With no read thread, the real barrier would never be released. */
	if (create_failed)
		fatal("waited on the startup barrier after the read thread failed to start");
	count_call(&barrier_calls);
	return pthread_barrier_wait(barrier);
}

/* ---- Fake libusb device: one HID interface at path "1-1:1.0" -------------- */

#define TEST_INTERFACE_NUMBER 0
#define TEST_DEVICE_PATH "1-1:1.0"

static char fake_context_storage;
static char fake_device_storage;
static char fake_handle_storage;
#define FAKE_CONTEXT ((libusb_context *)&fake_context_storage)
#define FAKE_DEVICE ((libusb_device *)&fake_device_storage)
#define FAKE_HANDLE ((libusb_device_handle *)&fake_handle_storage)

static libusb_device *fake_device_list[2];
static struct libusb_interface_descriptor fake_intf_desc;
static struct libusb_interface fake_interface;
static struct libusb_config_descriptor fake_config;

static int open_calls, close_calls;
static int claim_calls, release_calls;
static int detach_calls, attach_calls;
static int ring_destroyed_before_close = -1;

static void setup_fake_device(void)
{
	memset(&fake_intf_desc, 0, sizeof(fake_intf_desc));
	fake_intf_desc.bLength = LIBUSB_DT_INTERFACE_SIZE;
	fake_intf_desc.bDescriptorType = LIBUSB_DT_INTERFACE;
	fake_intf_desc.bInterfaceNumber = TEST_INTERFACE_NUMBER;
	fake_intf_desc.bInterfaceClass = LIBUSB_CLASS_HID;
	/* No interrupt IN endpoint: the read thread passes the startup barrier
	   and exits, without needing libusb event handling. */
	fake_intf_desc.bNumEndpoints = 0;

	memset(&fake_interface, 0, sizeof(fake_interface));
	fake_interface.altsetting = &fake_intf_desc;
	fake_interface.num_altsetting = 1;

	memset(&fake_config, 0, sizeof(fake_config));
	fake_config.bLength = LIBUSB_DT_CONFIG_SIZE;
	fake_config.bDescriptorType = LIBUSB_DT_CONFIG;
	fake_config.bNumInterfaces = 1;
	fake_config.bConfigurationValue = 1;
	fake_config.interface = &fake_interface;

	fake_device_list[0] = FAKE_DEVICE;
	fake_device_list[1] = NULL;
}

static int test_libusb_init(libusb_context **ctx)
{
	*ctx = FAKE_CONTEXT;
	return LIBUSB_SUCCESS;
}

static void test_libusb_exit(libusb_context *ctx)
{
	(void)ctx;
}

static ssize_t test_libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	(void)ctx;
	*list = fake_device_list;
	return 1;
}

static void test_libusb_free_device_list(libusb_device **list, int unref_devices)
{
	(void)list;
	(void)unref_devices;
}

static int test_libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
	(void)dev;
	memset(desc, 0, sizeof(*desc));
	desc->bLength = LIBUSB_DT_DEVICE_SIZE;
	desc->bDescriptorType = LIBUSB_DT_DEVICE;
	desc->idVendor = 0x1234;
	desc->idProduct = 0x5678;
	desc->bNumConfigurations = 1;
	return LIBUSB_SUCCESS;
}

static int test_libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config)
{
	(void)dev;
	*config = &fake_config;
	return LIBUSB_SUCCESS;
}

static int test_libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config)
{
	(void)config_index;
	return test_libusb_get_active_config_descriptor(dev, config);
}

static void test_libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
	(void)config;
}

static int test_libusb_get_port_numbers(libusb_device *dev, uint8_t *port_numbers, int port_numbers_len)
{
	(void)dev;
	if (port_numbers_len < 1)
		return LIBUSB_ERROR_OVERFLOW;
	port_numbers[0] = 1;
	return 1;
}

static uint8_t test_libusb_get_bus_number(libusb_device *dev)
{
	(void)dev;
	return 1;
}

static libusb_device *test_libusb_get_device(libusb_device_handle *dev_handle)
{
	(void)dev_handle;
	return FAKE_DEVICE;
}

static int test_libusb_open(libusb_device *dev, libusb_device_handle **dev_handle)
{
	(void)dev;
	count_call(&open_calls);
	*dev_handle = FAKE_HANDLE;
	return LIBUSB_SUCCESS;
}

static void test_libusb_close(libusb_device_handle *dev_handle);

static int test_libusb_kernel_driver_active(libusb_device_handle *dev_handle, int interface_number)
{
	(void)dev_handle;
	(void)interface_number;
	/* Pretend a kernel driver is bound, so the open detaches it. */
	return 1;
}

static int test_libusb_detach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
	(void)dev_handle;
	if (interface_number != TEST_INTERFACE_NUMBER)
		fatal("detached the kernel driver from an unexpected interface");
	count_call(&detach_calls);
	return LIBUSB_SUCCESS;
}

static int test_libusb_attach_kernel_driver(libusb_device_handle *dev_handle, int interface_number)
{
	(void)dev_handle;
	if (interface_number != TEST_INTERFACE_NUMBER)
		fatal("reattached the kernel driver to an unexpected interface");
	count_call(&attach_calls);
	return LIBUSB_SUCCESS;
}

static int test_libusb_claim_interface(libusb_device_handle *dev_handle, int interface_number)
{
	(void)dev_handle;
	if (interface_number != TEST_INTERFACE_NUMBER)
		fatal("claimed an unexpected interface");
	count_call(&claim_calls);
	return LIBUSB_SUCCESS;
}

static int test_libusb_release_interface(libusb_device_handle *dev_handle, int interface_number)
{
	(void)dev_handle;
	if (interface_number != TEST_INTERFACE_NUMBER)
		fatal("released an unexpected interface");
	count_call(&release_calls);
	return LIBUSB_SUCCESS;
}

static int test_libusb_control_transfer(libusb_device_handle *dev_handle, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	(void)dev_handle; (void)request_type; (void)bRequest; (void)wValue;
	(void)wIndex; (void)data; (void)wLength; (void)timeout;
	/* No report descriptor; the open doesn't depend on it. */
	return LIBUSB_ERROR_PIPE;
}

/* ---- The library under test ----------------------------------------------- */

/* <pthread.h> and <libusb.h> are already included (and include-guarded), so
   these only rename the calls made by the library code below. */
#define pthread_create test_pthread_create
#define pthread_join test_pthread_join
#define pthread_barrier_wait test_pthread_barrier_wait

#define libusb_init test_libusb_init
#define libusb_exit test_libusb_exit
#define libusb_get_device_list test_libusb_get_device_list
#define libusb_free_device_list test_libusb_free_device_list
#define libusb_get_device_descriptor test_libusb_get_device_descriptor
#define libusb_get_active_config_descriptor test_libusb_get_active_config_descriptor
#define libusb_get_config_descriptor test_libusb_get_config_descriptor
#define libusb_free_config_descriptor test_libusb_free_config_descriptor
#define libusb_get_port_numbers test_libusb_get_port_numbers
#define libusb_get_bus_number test_libusb_get_bus_number
#define libusb_get_device test_libusb_get_device
#define libusb_open test_libusb_open
#define libusb_close test_libusb_close
#define libusb_kernel_driver_active test_libusb_kernel_driver_active
#define libusb_detach_kernel_driver test_libusb_detach_kernel_driver
#define libusb_attach_kernel_driver test_libusb_attach_kernel_driver
#define libusb_claim_interface test_libusb_claim_interface
#define libusb_release_interface test_libusb_release_interface
#define libusb_control_transfer test_libusb_control_transfer

#include "../hid.c"

#undef pthread_create
#undef pthread_join
#undef pthread_barrier_wait

static void test_libusb_close(libusb_device_handle *dev_handle)
{
	(void)dev_handle;
	count_call(&close_calls);
	/* Both hid_open_path() (on failure) and hid_close() close the handle
	   before freeing the device: its input ring must already be gone in the
	   former case, and still be allocated in the latter. */
	if (opened_dev)
		ring_destroyed_before_close = ((hid_device *)opened_dev)->input_ring.storage == NULL;
}

/* ---- Test driver ------------------------------------------------------------ */

#ifdef DETACH_KERNEL_DRIVER
#define EXPECTED_DRIVER_CALLS 1
#else
#define EXPECTED_DRIVER_CALLS 0
#endif

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void reset_counters(void)
{
	pthread_mutex_lock(&counters_mutex);
	create_failed = 0;
	create_calls = join_calls = barrier_calls = 0;
	open_calls = close_calls = 0;
	claim_calls = release_calls = 0;
	detach_calls = attach_calls = 0;
	ring_destroyed_before_close = -1;
	opened_dev = NULL;
	pthread_mutex_unlock(&counters_mutex);
}

static void test_thread_create_failure(void)
{
	hid_device *dev;
	const wchar_t *err;

	reset_counters();
	inject_create_failure = 1;

	/* Must return instead of blocking in the startup barrier. */
	dev = hid_open_path(TEST_DEVICE_PATH);
	inject_create_failure = 0;

	CHECK(dev == NULL);
	CHECK(read_counter(&create_calls) == 1);
	CHECK(read_counter(&barrier_calls) == 0);
	CHECK(read_counter(&join_calls) == 0);

	/* Everything acquired for the open is given back. */
	CHECK(read_counter(&open_calls) == 1);
	CHECK(read_counter(&close_calls) == 1);
	CHECK(read_counter(&claim_calls) == 1);
	CHECK(read_counter(&release_calls) == 1);
	CHECK(read_counter(&detach_calls) == EXPECTED_DRIVER_CALLS);
	CHECK(read_counter(&attach_calls) == EXPECTED_DRIVER_CALLS);
	CHECK(ring_destroyed_before_close == 1);

	/* The failure is reported. */
	err = hid_error(NULL);
	CHECK(err != NULL && wcsstr(err, L"failed to initialize device") != NULL);
	if (dev)
		hid_close(dev);
}

static void test_normal_startup(void)
{
	hid_device *dev;

	reset_counters();

	dev = hid_open_path(TEST_DEVICE_PATH);
	CHECK(dev != NULL);
	if (!dev)
		return;

	CHECK(read_counter(&create_calls) == 1);
	/* Both the opening thread and the read thread passed the barrier. */
	CHECK(read_counter(&barrier_calls) == 2);
	CHECK(read_counter(&join_calls) == 0);
	CHECK(read_counter(&claim_calls) == 1);
	CHECK(read_counter(&release_calls) == 0);
	CHECK(read_counter(&detach_calls) == EXPECTED_DRIVER_CALLS);
	CHECK(read_counter(&attach_calls) == 0);
	CHECK(read_counter(&close_calls) == 0);
	CHECK(dev->input_ring.storage != NULL);
	CHECK(wcscmp(hid_error(NULL), L"Success") == 0);

	hid_close(dev);

	CHECK(read_counter(&join_calls) == 1);
	CHECK(read_counter(&release_calls) == 1);
	CHECK(read_counter(&attach_calls) == EXPECTED_DRIVER_CALLS);
	CHECK(read_counter(&close_calls) == 1);
	CHECK(ring_destroyed_before_close == 0);
}

int main(void)
{
	/* Watchdog: a regression must fail the test rather than hang it. */
	alarm(10);

	setup_fake_device();

	CHECK(hid_init() == 0);

	test_thread_create_failure();
	/* The library stays usable after a failed open. */
	test_normal_startup();

	CHECK(hid_exit() == 0);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
