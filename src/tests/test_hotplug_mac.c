/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 macOS hotplug lifecycle regression tests.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <hidapi.h>

#define WORKERS 4
#define ITERATIONS 8
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static struct timespec deadline_ms(int ms)
{
	struct timespec deadline;
	CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += ms / 1000;
	deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

/* All condition waits and API calls are bounded, including pthread_join:
   alarm() terminates the standalone test if a library call deadlocks. */
static void wait_flag(pthread_cond_t *cond, pthread_mutex_t *mutex, const int *flag)
{
	struct timespec deadline = deadline_ms(5000);
	while (!*flag)
		CHECK(pthread_cond_timedwait(cond, mutex, &deadline) == 0);
}

static int HID_API_CALL keep_callback(hid_hotplug_callback_handle handle,
	struct hid_device_info *device, hid_hotplug_event event, void *user_data)
{
	(void)handle;
	(void)device;
	(void)event;
	(void)user_data;
	return 0;
}

static hid_hotplug_callback_handle register_quiet(void)
{
	hid_hotplug_callback_handle handle = 0;
	CHECK(hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
		0, keep_callback, NULL, &handle) == 0);
	CHECK(handle > 0);
	return handle;
}

struct worker_gate {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int arrived;
	int generation;
};

static void wait_workers(struct worker_gate *gate)
{
	struct timespec deadline = deadline_ms(5000);
	int generation;
	CHECK(pthread_mutex_lock(&gate->mutex) == 0);
	generation = gate->generation;
	if (++gate->arrived == WORKERS) {
		gate->arrived = 0;
		gate->generation++;
		CHECK(pthread_cond_broadcast(&gate->cond) == 0);
	} else {
		while (generation == gate->generation)
			CHECK(pthread_cond_timedwait(&gate->cond, &gate->mutex, &deadline) == 0);
	}
	CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void *registration_worker(void *arg)
{
	struct worker_gate *gate = (struct worker_gate *)arg;
	int i;
	for (i = 0; i < ITERATIONS; i++) {
		hid_hotplug_callback_handle handle;
		wait_workers(gate);
		handle = register_quiet();
		wait_workers(gate);
		CHECK(hid_hotplug_deregister_callback(handle) == 0);
	}
	return NULL;
}

static void test_concurrent_registration(void)
{
	struct worker_gate gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };
	pthread_t workers[WORKERS];
	int i;
	puts("Concurrent register/deregister and competing collectors");
	for (i = 0; i < WORKERS; i++)
		CHECK(pthread_create(&workers[i], NULL, registration_worker, &gate) == 0);
	for (i = 0; i < WORKERS; i++)
		CHECK(pthread_join(workers[i], NULL) == 0);
	CHECK(pthread_cond_destroy(&gate.cond) == 0);
	CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
}

enum callback_action { REMOVE_BY_RETURN, REMOVE_EXPLICITLY, WAIT_FOR_RELEASE, REMOVE_THEN_EXIT };

static pthread_key_t callback_key;

struct callback_state {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	enum callback_action action;
	int entered;
	int release;
	int completed;
	int deregister_started;
	int deregister_done;
	int destructor_completed;
	hid_hotplug_callback_handle handle;
};

static void event_thread_destructor(void *arg)
{
	struct callback_state *state = (struct callback_state *)arg;
	struct timespec delay = { 0, 100000000L };
	hid_hotplug_callback_handle handle = -1;
	/* Resource release alone must not let a collector skip this destructor. */
	while (nanosleep(&delay, &delay) != 0)
		CHECK(errno == EINTR);
	CHECK(hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED,
		0, keep_callback, NULL, &handle) == -1);
	CHECK(handle == 0);
	CHECK(pthread_mutex_lock(&state->mutex) == 0);
	state->destructor_completed = 1;
	CHECK(pthread_mutex_unlock(&state->mutex) == 0);
}

static int HID_API_CALL lifecycle_callback(hid_hotplug_callback_handle handle,
	struct hid_device_info *device, hid_hotplug_event event, void *user_data)
{
	struct callback_state *state = (struct callback_state *)user_data;
	(void)device;
	CHECK(event == HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED);
	if (state->action != WAIT_FOR_RELEASE)
		CHECK(pthread_setspecific(callback_key, state) == 0);
	if (state->action == REMOVE_EXPLICITLY)
		CHECK(hid_hotplug_deregister_callback(handle) == 0);
	CHECK(pthread_mutex_lock(&state->mutex) == 0);
	if (state->action != WAIT_FOR_RELEASE)
		CHECK(!state->entered);
	state->entered = 1;
	CHECK(pthread_cond_broadcast(&state->cond) == 0);
	if (state->action == WAIT_FOR_RELEASE)
		wait_flag(&state->cond, &state->mutex, &state->release);
	state->completed = 1;
	CHECK(pthread_mutex_unlock(&state->mutex) == 0);
	return state->action == REMOVE_BY_RETURN || state->action == REMOVE_THEN_EXIT;
}

static void *deregister_worker(void *arg)
{
	struct callback_state *state = (struct callback_state *)arg;
	CHECK(pthread_mutex_lock(&state->mutex) == 0);
	state->deregister_started = 1;
	CHECK(pthread_cond_broadcast(&state->cond) == 0);
	CHECK(pthread_mutex_unlock(&state->mutex) == 0);
	CHECK(hid_hotplug_deregister_callback(state->handle) == 0);
	CHECK(pthread_mutex_lock(&state->mutex) == 0);
	CHECK(state->release && state->completed);
	state->deregister_done = 1;
	CHECK(pthread_cond_broadcast(&state->cond) == 0);
	CHECK(pthread_mutex_unlock(&state->mutex) == 0);
	return NULL;
}

static void test_device_callback(enum callback_action action, const char *name)
{
	struct callback_state state = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
		action, 0, 0, 0, 0, 0, 0, 0 };
	struct hid_device_info *devices = hid_enumerate(0, 0);
	unsigned short vendor_id, product_id;

	/* Enumeration and init/exit stay on the owner thread, outside worker calls. */
	if (!devices) {
		printf("SKIP: %s (no HID device available)\n", name);
	} else {
		vendor_id = devices->vendor_id;
		product_id = devices->product_id;
		hid_free_enumeration(devices);
		printf("%s (VID %04hx, PID %04hx)\n", name, vendor_id, product_id);
		CHECK(hid_hotplug_register_callback(vendor_id, product_id,
			HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, HID_API_HOTPLUG_ENUMERATE,
			lifecycle_callback, &state, &state.handle) == 0);
		CHECK(pthread_mutex_lock(&state.mutex) == 0);
		wait_flag(&state.cond, &state.mutex, &state.entered);
		CHECK(pthread_mutex_unlock(&state.mutex) == 0);

		if (action == WAIT_FOR_RELEASE) {
			pthread_t worker;
			struct timespec deadline;
			int result = 0;
			CHECK(pthread_create(&worker, NULL, deregister_worker, &state) == 0);
			CHECK(pthread_mutex_lock(&state.mutex) == 0);
			wait_flag(&state.cond, &state.mutex, &state.deregister_started);
			/* Give the external call time to block while the callback is held. */
			deadline = deadline_ms(100);
			while (!state.deregister_done && result == 0)
				result = pthread_cond_timedwait(&state.cond, &state.mutex, &deadline);
			CHECK(result == ETIMEDOUT && !state.deregister_done);
			state.release = 1;
			CHECK(pthread_cond_broadcast(&state.cond) == 0);
			wait_flag(&state.cond, &state.mutex, &state.deregister_done);
			CHECK(pthread_mutex_unlock(&state.mutex) == 0);
			CHECK(pthread_join(worker, NULL) == 0);
		} else {
			/* The API mutex waits for the self-removing callback to return;
			   collection must also wait for its thread-specific destructor. */
			hid_hotplug_callback_handle restarted = 0;
			if (action == REMOVE_THEN_EXIT)
				CHECK(hid_exit() == 0);
			else
				restarted = register_quiet();
			CHECK(pthread_mutex_lock(&state.mutex) == 0);
			CHECK(state.destructor_completed);
			CHECK(pthread_mutex_unlock(&state.mutex) == 0);
			if (action == REMOVE_THEN_EXIT) {
				CHECK(hid_init() == 0);
			} else {
				CHECK(hid_hotplug_deregister_callback(restarted) == 0);
				CHECK(hid_hotplug_deregister_callback(state.handle) == -1);
			}
		}
	}
	CHECK(pthread_cond_destroy(&state.cond) == 0);
	CHECK(pthread_mutex_destroy(&state.mutex) == 0);
}

int main(void)
{
	int i;
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(45);
	/* macOS requires the initializing thread to remain alive through hid_exit. */
	CHECK(hid_init() == 0);
	test_concurrent_registration();
	puts("Immediate restart after last deregistration");
	for (i = 0; i < ITERATIONS; i++) {
		hid_hotplug_callback_handle handle = register_quiet();
		CHECK(hid_hotplug_deregister_callback(handle) == 0);
	}
	CHECK(pthread_key_create(&callback_key, event_thread_destructor) == 0);
	test_device_callback(REMOVE_BY_RETURN, "Last callback removal by return value and restart");
	test_device_callback(REMOVE_EXPLICITLY, "Last callback explicit deregistration and restart");
	test_device_callback(REMOVE_THEN_EXIT, "Last callback removal followed by owner-thread hid_exit");
	test_device_callback(WAIT_FOR_RELEASE, "External deregistration waits for an in-flight callback");
	CHECK(pthread_key_delete(callback_key) == 0);
	CHECK(hid_exit() == 0);
	puts("Repeated owner-thread hid_init/register/hid_exit");
	for (i = 0; i < ITERATIONS; i++) {
		CHECK(hid_init() == 0);
		(void)register_quiet();
		CHECK(hid_exit() == 0);
	}
	alarm(0);
	puts("Hotplug lifecycle tests passed");
	return EXIT_SUCCESS;
}
