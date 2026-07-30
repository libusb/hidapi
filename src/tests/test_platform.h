/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026.

 Test support: tiny cross-platform helpers (threads, timing)
 so the HIDAPI unit tests stay platform-neutral.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#ifndef HIDAPI_TEST_PLATFORM_H__
#define HIDAPI_TEST_PLATFORM_H__

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
#endif

/* Monotonic milliseconds for measuring elapsed time. */
static long long test_now_ms(void)
{
#ifdef _WIN32
	return (long long)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void test_sleep_ms(int ms)
{
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
#endif
}

/* A joinable thread running void fn(void*). Results are communicated through
 * the user's arg (this matches how the tests use a context struct). */
typedef struct test_thread {
	void (*fn)(void *);
	void *arg;
#ifdef _WIN32
	HANDLE handle;
#else
	pthread_t thread;
	int done; /* set by the wrapper when fn returns; polled by the timed join */
#endif
} test_thread;

#ifdef _WIN32
static DWORD WINAPI test__thread_entry(LPVOID p)
{
	test_thread *t = (test_thread *)p;
	t->fn(t->arg);
	return 0;
}
#else
static void *test__thread_entry(void *p)
{
	test_thread *t = (test_thread *)p;
	t->fn(t->arg);
	__atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
	return NULL;
}
#endif

/* Returns 0 on success, -1 on failure. */
static int test_thread_start(test_thread *t, void (*fn)(void *), void *arg)
{
	t->fn = fn;
	t->arg = arg;
#ifdef _WIN32
	t->handle = CreateThread(NULL, 0, test__thread_entry, t, 0, NULL);
	return t->handle ? 0 : -1;
#else
	t->done = 0;
	return pthread_create(&t->thread, NULL, test__thread_entry, t) == 0 ? 0 : -1;
#endif
}

/* Join with a timeout. Returns 0 if the thread finished, -1 on timeout. */
static int test_thread_join_timeout(test_thread *t, int timeout_ms)
{
#ifdef _WIN32
	DWORD r = WaitForSingleObject(t->handle, (DWORD)timeout_ms);
	if (r == WAIT_OBJECT_0) {
		CloseHandle(t->handle);
		t->handle = NULL;
		return 0;
	}
	return -1;
#else
	/* pthread_timedjoin_np() is a glibc/BSD extension that does not exist on
	   macOS, so poll a completion flag set by the thread wrapper instead, and
	   only pthread_join() (briefly) once the thread is known to be finishing. */
	long long deadline = test_now_ms() + timeout_ms;
	while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
		if (test_now_ms() >= deadline)
			return -1;
		test_sleep_ms(10);
	}
	return pthread_join(t->thread, NULL) == 0 ? 0 : -1;
#endif
}

#endif /* HIDAPI_TEST_PLATFORM_H__ */
