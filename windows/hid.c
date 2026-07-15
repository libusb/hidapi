/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022, All Rights Reserved.

 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU General Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        https://github.com/libusb/hidapi .
********************************************************/

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32_LEAN_AND_MEAN
/* It may be set by IDE/project and apparently HIDAPI relies
 * on certain Windows headers being included by default. */
#undef WIN32_LEAN_AND_MEAN
#endif

#include "hidapi_winapi.h"

#include <windows.h>

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

#ifdef __CYGWIN__
#include <ntdef.h>
#include <wctype.h>
#define _wcsdup wcsdup
#define _strdup strdup
#endif

/*#define HIDAPI_USE_DDK*/

#include "hidapi_cfgmgr32.h"
#include "hidapi_hidclass.h"
#include "hidapi_hidsdi.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MSVC secure CRT (VS2005+) provides swprintf_s/wcsncpy_s.
   Older MSVC and GCC/MinGW/Cygwin use the classic variants. */
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#define HIDAPI_SWPRINTF swprintf_s
#define HIDAPI_WCSNCPY(dest, dest_count, src) wcsncpy_s((dest), (dest_count), (src), _TRUNCATE)
#else
#define HIDAPI_SWPRINTF swprintf
#define HIDAPI_WCSNCPY(dest, dest_count, src) wcsncpy((dest), (src), (dest_count))
#endif

#ifdef MIN
#undef MIN
#endif
#define MIN(x,y) ((x) < (y)? (x): (y))

/* MAXIMUM_USB_STRING_LENGTH from usbspec.h is 255 */
/* BLUETOOTH_DEVICE_NAME_SIZE from bluetoothapis.h is 256 */
#define MAX_STRING_WCHARS 256

/* For certain USB devices, using a buffer larger or equal to 127 wchars results
   in successful completion of HID API functions, but a broken string is stored
   in the output buffer. This behaviour persists even if HID API is bypassed and
   HID IOCTLs are passed to the HID driver directly. Therefore, for USB devices,
   the buffer MUST NOT exceed 126 WCHARs.
*/

#define MAX_STRING_WCHARS_USB 126

/* The value of the first callback handle to be given upon registration */
/* Can be any arbitrary positive integer */
#define FIRST_HOTPLUG_CALLBACK_HANDLE 1

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

#ifndef HIDAPI_USE_DDK
/* Since we're not building with the DDK, and the HID header
   files aren't part of the Windows SDK, we define what we need ourselves.
   In lookup_functions(), the function pointers
   defined below are set. */

static HidD_GetHidGuid_ HidD_GetHidGuid;
static HidD_GetAttributes_ HidD_GetAttributes;
static HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
static HidD_GetManufacturerString_ HidD_GetManufacturerString;
static HidD_GetProductString_ HidD_GetProductString;
static HidD_SetFeature_ HidD_SetFeature;
static HidD_GetFeature_ HidD_GetFeature;
static HidD_SetOutputReport_ HidD_SetOutputReport; 
static HidD_GetInputReport_ HidD_GetInputReport;
static HidD_GetIndexedString_ HidD_GetIndexedString;
static HidD_GetPreparsedData_ HidD_GetPreparsedData;
static HidD_FreePreparsedData_ HidD_FreePreparsedData;
static HidP_GetCaps_ HidP_GetCaps;
static HidD_SetNumInputBuffers_ HidD_SetNumInputBuffers;

static CM_Locate_DevNodeW_ CM_Locate_DevNodeW = NULL;
static CM_Get_Parent_ CM_Get_Parent = NULL;
static CM_Get_DevNode_PropertyW_ CM_Get_DevNode_PropertyW = NULL;
static CM_Get_Device_Interface_PropertyW_ CM_Get_Device_Interface_PropertyW = NULL;
static CM_Get_Device_Interface_List_SizeW_ CM_Get_Device_Interface_List_SizeW = NULL;
static CM_Get_Device_Interface_ListW_ CM_Get_Device_Interface_ListW = NULL;
static CM_Register_Notification_ CM_Register_Notification = NULL;
static CM_Unregister_Notification_ CM_Unregister_Notification = NULL;

static HMODULE hid_lib_handle = NULL;
static HMODULE cfgmgr32_lib_handle = NULL;
static BOOLEAN hidapi_initialized = FALSE;

static void free_library_handles()
{
	if (hid_lib_handle)
		FreeLibrary(hid_lib_handle);
	hid_lib_handle = NULL;
	if (cfgmgr32_lib_handle)
		FreeLibrary(cfgmgr32_lib_handle);
	cfgmgr32_lib_handle = NULL;
}

static int lookup_functions()
{
	hid_lib_handle = LoadLibraryW(L"hid.dll");
	if (hid_lib_handle == NULL) {
		goto err;
	}

	cfgmgr32_lib_handle = LoadLibraryW(L"cfgmgr32.dll");
	if (cfgmgr32_lib_handle == NULL) {
		goto err;
	}

/* Avoid direct function-pointer cast from FARPROC to typed callback pointer.
   Using memcpy keeps this warning-free regardless of the compiler and compiler settings. */
#define RESOLVE(lib_handle, x) do { \
	FARPROC proc_addr = GetProcAddress(lib_handle, #x); \
	if (!proc_addr) goto err; \
	memcpy(&x, &proc_addr, sizeof(x)); \
} while (0)

	RESOLVE(hid_lib_handle, HidD_GetHidGuid);
	RESOLVE(hid_lib_handle, HidD_GetAttributes);
	RESOLVE(hid_lib_handle, HidD_GetSerialNumberString);
	RESOLVE(hid_lib_handle, HidD_GetManufacturerString);
	RESOLVE(hid_lib_handle, HidD_GetProductString);
	RESOLVE(hid_lib_handle, HidD_SetFeature);
	RESOLVE(hid_lib_handle, HidD_GetFeature);
	RESOLVE(hid_lib_handle, HidD_SetOutputReport);
	RESOLVE(hid_lib_handle, HidD_GetInputReport);
	RESOLVE(hid_lib_handle, HidD_GetIndexedString);
	RESOLVE(hid_lib_handle, HidD_GetPreparsedData);
	RESOLVE(hid_lib_handle, HidD_FreePreparsedData);
	RESOLVE(hid_lib_handle, HidP_GetCaps);
	RESOLVE(hid_lib_handle, HidD_SetNumInputBuffers);

	RESOLVE(cfgmgr32_lib_handle, CM_Locate_DevNodeW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Parent);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_DevNode_PropertyW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_PropertyW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_List_SizeW);
	RESOLVE(cfgmgr32_lib_handle, CM_Get_Device_Interface_ListW);
	RESOLVE(cfgmgr32_lib_handle, CM_Register_Notification);
	RESOLVE(cfgmgr32_lib_handle, CM_Unregister_Notification);

#undef RESOLVE

	return 0;

err:
	free_library_handles();
	return -1;
}

#endif /* HIDAPI_USE_DDK */

struct hid_device_ {
		HANDLE device_handle;
		BOOL blocking;
		USHORT output_report_length;
		unsigned char *write_buf;
		size_t input_report_length;
		USHORT feature_report_length;
		unsigned char *feature_buf;
		wchar_t *last_error_str;
		wchar_t *last_read_error_str;
		BOOL read_pending;
		char *read_buf;
		OVERLAPPED ol;
		OVERLAPPED write_ol;
		struct hid_device_info* device_info;
		DWORD write_timeout_ms;
};

/* The threadpool work item that delivers the HID_API_HOTPLUG_ENUMERATE pass is
   Windows Vista and up, so - like every other API this file uses above its
   minimum target - it is resolved dynamically: a static import would raise the
   Windows version hidapi can be loaded on for every user, including those that
   never use hotplug. Hotplug registration fails with an error when it is not
   available; nothing else in the library depends on it.

   The threadpool handles are declared as opaque pointers (which is what they are
   in the SDK, too), so that none of this needs headers newer than the file's
   minimum target. */
typedef VOID (WINAPI *hid_internal_tp_work_callback)(PVOID instance, PVOID context, PVOID work);
typedef PVOID (WINAPI *CreateThreadpoolWork_)(hid_internal_tp_work_callback callback, PVOID context, PVOID callback_environ);
typedef VOID (WINAPI *SubmitThreadpoolWork_)(PVOID work);
typedef VOID (WINAPI *CloseThreadpoolWork_)(PVOID work);
typedef VOID (WINAPI *WaitForThreadpoolWorkCallbacks_)(PVOID work, BOOL cancel_pending);

static CreateThreadpoolWork_ hid_internal_CreateThreadpoolWork = NULL;
static SubmitThreadpoolWork_ hid_internal_SubmitThreadpoolWork = NULL;
static CloseThreadpoolWork_ hid_internal_CloseThreadpoolWork = NULL;
static WaitForThreadpoolWorkCallbacks_ hid_internal_WaitForThreadpoolWorkCallbacks = NULL;

static struct hid_hotplug_context {
	/* Win32 notification handle */
	HCMNOTIFICATION notify_handle;

	/* Threadpool work item (a PTP_WORK): delivers pending HID_API_HOTPLUG_ENUMERATE
	   passes and performs cleanup deferred from the notification callback.
	   Created with the first callback registration, closed by hid_exit(). */
	PVOID event_work;

	/* Number of notification handles detached from the context whose
	   CM_Unregister_Notification call has not completed yet, and a manual-reset
	   event that is signaled exactly while that count is zero. Both are guarded
	   by the critical section (the event is only ever waited on without it).
	   A new notification is only armed once the count is zero: the OS keeps a
	   detached handle live until the unregistration completes, and two live
	   registrations would deliver every event twice. */
	LONG pending_unregistrations;
	HANDLE quiescent_event;

	/* Set when CM_Unregister_Notification failed: the OS-side registration may
	   still be live and call into this module at any time. Sticky for the whole
	   process - the state such a notification can reach is never destroyed, the
	   libraries it calls into are never unloaded, the module is pinned, and no
	   second notification is ever armed next to it (every event would be
	   delivered twice). */
	unsigned char notification_leaked;

	/* Same failure, scoped to the teardown that has to report it (hid_exit) */
	unsigned char unregistration_failed;

	/* Critical section (faster mutex substitute), for both cached device list and callback list changes */
	CRITICAL_SECTION critical_section;

	/* Boolean flags */
	unsigned char mutex_ready;
	unsigned char mutex_in_use;
	unsigned char cb_list_dirty;

	/* HIDAPI unique callback handle counter */
	hid_hotplug_callback_handle next_handle;

	/* Linked list of the hotplug callbacks */
	struct hid_hotplug_callback *hotplug_cbs;

	/* Linked list of the device infos (mandatory when the device is disconnected).
	   Doubles as the arrival dedupe set: an arrival for a path that is already in
	   here has already been reported (see hid_internal_notify_callback). */
	struct hid_device_info *devs;
} hid_hotplug_context; /* zero-initialized (static storage); next_handle set on first init */

static hid_device *new_hid_device()
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));

	if (dev == NULL) {
		return NULL;
	}

	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->blocking = TRUE;
	dev->output_report_length = 0;
	dev->write_buf = NULL;
	dev->input_report_length = 0;
	dev->feature_report_length = 0;
	dev->feature_buf = NULL;
	dev->last_error_str = NULL;
	dev->last_read_error_str = NULL;
	dev->read_pending = FALSE;
	dev->read_buf = NULL;
	memset(&dev->ol, 0, sizeof(dev->ol));
	dev->ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*initial state f=nonsignaled*/, NULL);
	memset(&dev->write_ol, 0, sizeof(dev->write_ol));
	dev->write_ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*initial state f=nonsignaled*/, NULL);
	dev->device_info = NULL;
	dev->write_timeout_ms = 1000;

	return dev;
}

static void free_hid_device(hid_device *dev)
{
	CloseHandle(dev->ol.hEvent);
	CloseHandle(dev->write_ol.hEvent);
	CloseHandle(dev->device_handle);
	free(dev->last_error_str);
	free(dev->last_read_error_str);
	dev->last_error_str = NULL;
	dev->last_read_error_str = NULL;
	free(dev->write_buf);
	free(dev->feature_buf);
	free(dev->read_buf);
	hid_free_enumeration(dev->device_info);
	free(dev);
}

static void register_winapi_error_code_to_buffer(wchar_t **error_buffer, const WCHAR *op, DWORD error_code)
{
	free(*error_buffer);
	*error_buffer = NULL;

	/* Only clear out error messages if NULL is passed into op */
	if (!op) {
		return;
	}

	WCHAR system_err_buf[1024];

	DWORD system_err_len = FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		error_code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		system_err_buf, ARRAYSIZE(system_err_buf),
		NULL);

	DWORD op_len = (DWORD)wcslen(op);

	DWORD op_prefix_len =
		op_len
		+ 15 /*: (0x00000000) */
		;
	DWORD msg_len =
		+ op_prefix_len
		+ system_err_len
		;

	*error_buffer = (WCHAR *)calloc(msg_len + 1, sizeof (WCHAR));
	WCHAR *msg = *error_buffer;

	if (!msg)
		return;

	int printf_written = HIDAPI_SWPRINTF(msg, msg_len + 1, L"%.*ls: (0x%08X) %.*ls", (int)op_len, op, error_code, (int)system_err_len, system_err_buf);
	msg[msg_len] = L'\0';

	if (printf_written < 0)
	{
		/* Highly unlikely */
		msg[0] = L'\0';
		return;
	}

	/* Get rid of the CR and LF that FormatMessage() sticks at the
	   end of the message. Thanks Microsoft! */
	while (msg[msg_len-1] == L'\r' || msg[msg_len-1] == L'\n' || msg[msg_len-1] == L' ')
	{
		msg[msg_len-1] = L'\0';
		msg_len--;
	}
}

static void register_winapi_error_to_buffer(wchar_t **error_buffer, const WCHAR *op)
{
	/* Capture the error code first: free() (and, for the global error buffer,
	   acquiring its lock) is not required to preserve it */
	DWORD error_code = GetLastError();

	register_winapi_error_code_to_buffer(error_buffer, op, error_code);
}

#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
#endif
/* A bug in GCC/mingw gives:
 * error: array subscript 0 is outside array bounds of 'wchar_t *[0]' {aka 'short unsigned int *[]'} [-Werror=array-bounds]
 * |         free(*error_buffer);
 * Which doesn't make sense in this context. */

static void register_string_error_to_buffer(wchar_t **error_buffer, const WCHAR *string_error)
{
	free(*error_buffer);
	*error_buffer = NULL;

	if (string_error) {
		*error_buffer = _wcsdup(string_error);
	}
}

#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif

static void register_winapi_error(hid_device *dev, const WCHAR *op)
{
	register_winapi_error_to_buffer(&dev->last_error_str, op);
}

static void register_string_error(hid_device *dev, const WCHAR *string_error)
{
	register_string_error_to_buffer(&dev->last_error_str, string_error);
}

/* A minimal exclusive lock that needs no initialization.

   Deliberately not an SRWLOCK (nor a CONDITION_VARIABLE, nor the CRITICAL_SECTION
   that cannot be initialized statically): those APIs are Windows Vista and up, so
   using them would turn hidapi into a load-time importer of Vista-only kernel32
   symbols - for every user of the library, including those that never touch
   hotplug. Everything this file uses above its minimum target is resolved
   dynamically instead (see lookup_functions and hid_internal_hotplug_resolve_threadpool).
   The regions guarded by this lock are kept deliberately small - pointer swaps
   and flag/counter updates; the one-time bootstrap of the hotplug machinery is
   the largest. */
typedef volatile LONG hid_internal_lock;

static void hid_internal_lock_acquire(hid_internal_lock *lock)
{
	unsigned int attempts = 0;

	while (InterlockedCompareExchange(lock, 1, 0) != 0) {
		/* Sleep(0) yields the rest of the quantum, but only to threads of equal
		   or higher priority: if the holder is lower-priority (or starved on a
		   single-CPU system), spinning on it can burn quanta without any
		   progress. After a few attempts, Sleep(1) instead: it yields to any
		   ready thread. */
		if (++attempts < 16) {
			Sleep(0);
		} else {
			Sleep(1);
		}
	}
}

static void hid_internal_lock_release(hid_internal_lock *lock)
{
	InterlockedExchange(lock, 0);
}

static wchar_t *last_global_error_str = NULL;

/* Serializes mutations of last_global_error_str: the hotplug API is
   thread-safe and its failure paths may write the global error concurrently.
   Note that this only protects writers against each other: hid_error(NULL)
   hands the raw string pointer out to the application without any lock, which
   is why the header requires the application to serialize hid_error(NULL)
   against the hotplug API. HIDAPI's own code on the internal event context
   never writes the global error - not even a user callback that re-enters the
   public hotplug API from that context: register_global_error_message() drops
   those writes (see the mutex_in_use guard there). An application therefore
   never has to serialize hid_error(NULL) against a write it could not see
   coming, matching the other backends (libusb, linux, mac). */
static hid_internal_lock global_error_lock = 0;

/* Publishes a message (built by the caller, ownership taken) as the global
   error string. Only the pointer swap is under the lock: it is a spinlock, and
   building or freeing a message is far too heavy for a spin-guarded region. */
static void register_global_error_message(wchar_t *msg)
{
	wchar_t *old_msg;

	/* A user callback runs on HIDAPI's internal event context (a threadpool
	   work item or the CM notification callback), where mutex_in_use is set for
	   the whole dispatch. A nested public hotplug call made from such a callback
	   must not touch last_global_error_str - success clear included: the write
	   happens on the event context, and the application cannot serialize its
	   lock-free hid_error(NULL) read against it. Drop the write instead. This is
	   a cheap byte read of a flag the dispatching thread itself set (and the same
	   thread holds the critical section), so it adds no lock-order edge. */
	if (hid_hotplug_context.mutex_in_use) {
		free(msg);
		return;
	}

	hid_internal_lock_acquire(&global_error_lock);
	old_msg = last_global_error_str;
	last_global_error_str = msg;
	hid_internal_lock_release(&global_error_lock);

	free(old_msg);
}

static void register_global_winapi_error_code(DWORD error_code, const WCHAR *op)
{
	wchar_t *msg = NULL;

	register_winapi_error_code_to_buffer(&msg, op, error_code);
	register_global_error_message(msg);
}

static void register_global_winapi_error(const WCHAR *op)
{
	/* Capture the error code first: building the message may clobber it */
	register_global_winapi_error_code(GetLastError(), op);
}

static void register_global_error(const WCHAR *string_error)
{
	register_global_error_message(string_error ? _wcsdup(string_error) : NULL);
}

static HANDLE open_device(const wchar_t *path, BOOL open_rw)
{
	HANDLE handle;
	DWORD desired_access = (open_rw)? (GENERIC_WRITE | GENERIC_READ): 0;
	DWORD share_mode = FILE_SHARE_READ|FILE_SHARE_WRITE;

	handle = CreateFileW(path,
		desired_access,
		share_mode,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,/*FILE_ATTRIBUTE_NORMAL,*/
		0);

	return handle;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

/* Serializes the bootstrap and the teardown of the hotplug machinery (and the
   flag and counter below): two racing first registrations must not both
   initialize the critical section, and every read of mutex_ready that is not
   already made under the critical section is made under this lock (it is what
   publishes the critical section to other threads). Statically initialized:
   guarding state with it costs no OS object. */
static hid_internal_lock hotplug_init_lock = 0;

/* Set while hid_exit() is tearing the hotplug machinery down (and unloading the
   resolved libraries): hotplug registration and deregistration fail instead of
   arming a context that is being destroyed, or calling into libraries that are
   being unloaded. Guarded by hotplug_init_lock, NOT by the critical section:
   hid_exit() must be able to raise it before it can know whether the machinery
   (and with it the critical section) even exists - and without creating it, as
   a program that never uses hotplug must not have hid_exit() create OS objects
   on its behalf. */
static LONG hotplug_exiting = 0;

/* Number of threads currently counted into the hotplug machinery by
   hid_internal_hotplug_enter, i.e. inside - or blocked on - the critical
   section from a public hotplug call. Guarded by hotplug_init_lock.
   hid_exit() destroys the critical section and the quiescence event only after
   `hotplug_exiting` has cut off new entries and this count has drained to zero:
   deleting a critical section another thread is blocked on (or about to enter)
   is undefined behavior. */
static LONG hotplug_machinery_users = 0;

/* Resolves the threadpool API used as the hotplug event context.
   Always called inside the critical section. Returns -1 when it is unavailable
   (the OS predates it), in which case hotplug is not available either. */
static int hid_internal_hotplug_resolve_threadpool(void)
{
	HMODULE kernel32;

	if (hid_internal_CreateThreadpoolWork != NULL) {
		/* Already resolved: resolved last, so it doubles as the "all set" flag */
		return 0;
	}

	/* kernel32.dll is mapped into every process and is never unloaded, so its
	   handle needs neither LoadLibrary nor FreeLibrary */
	kernel32 = GetModuleHandleW(L"kernel32.dll");
	if (kernel32 == NULL) {
		return -1;
	}

/* Avoid direct function-pointer cast from FARPROC to typed callback pointer.
   Using memcpy keeps this warning-free regardless of the compiler and compiler settings. */
#define RESOLVE_TP(x) do { \
	FARPROC proc_addr = GetProcAddress(kernel32, #x); \
	if (!proc_addr) return -1; \
	memcpy(&hid_internal_##x, &proc_addr, sizeof(hid_internal_##x)); \
} while (0)

	RESOLVE_TP(SubmitThreadpoolWork);
	RESOLVE_TP(CloseThreadpoolWork);
	RESOLVE_TP(WaitForThreadpoolWorkCallbacks);
	RESOLVE_TP(CreateThreadpoolWork);

#undef RESOLVE_TP

	return 0;
}

/* Bootstraps the hotplug machinery: the critical section that guards all
   hotplug state and the manual-reset quiescence event. Must be called with
   hotplug_init_lock held. On failure nothing is created and *create_error
   receives the CreateEvent error code (reporting it is left to the caller:
   this runs under a spinlock).
   Once created, the machinery stays valid for as long as anything can enter it:
   hid_exit() destroys it only after proving nothing can (see
   hid_internal_hotplug_enter and hid_internal_hotplug_exit), and when a
   notification could not be unregistered it is never destroyed at all, so that
   a live OS callback can never enter a deleted critical section. */
static int hid_internal_hotplug_init_under_lock(DWORD *create_error)
{
	if (hid_hotplug_context.mutex_ready) {
		return 0;
	}

	/* Manual reset, initially signaled: nothing is pending yet */
	hid_hotplug_context.quiescent_event = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (hid_hotplug_context.quiescent_event == NULL) {
		*create_error = GetLastError();
		return -1;
	}

	InitializeCriticalSection(&hid_hotplug_context.critical_section);

	hid_hotplug_context.mutex_in_use = 0;
	hid_hotplug_context.cb_list_dirty = 0;
	hid_hotplug_context.pending_unregistrations = 0;
	if (hid_hotplug_context.next_handle < FIRST_HOTPLUG_CALLBACK_HANDLE)
		hid_hotplug_context.next_handle = FIRST_HOTPLUG_CALLBACK_HANDLE;

	/* Set state to Ready. Published last: a thread that observes this
	   under hotplug_init_lock also observes everything above. */
	hid_hotplug_context.mutex_ready = 1;

	return 0;
}

/* Result codes of hid_internal_hotplug_enter (0 is success) */
#define HID_HOTPLUG_ENTER_EXITING   1 /* hid_exit() is in progress */
#define HID_HOTPLUG_ENTER_NOT_READY 2 /* no machinery and bootstrap not requested */
#define HID_HOTPLUG_ENTER_FAILED    3 /* bootstrap failed (global error registered) */

/* Counts the calling thread into the hotplug machinery, bootstrapping it first
   when `bootstrap` is set. While a thread is counted in, the critical section
   and the quiescence event exist and stay valid: hid_exit() destroys them only
   after `hotplug_exiting` has cut off new entries AND the count has drained to
   zero. A successful call (and only a successful call) MUST be balanced with
   hid_internal_hotplug_leave(). */
static int hid_internal_hotplug_enter(int bootstrap)
{
	int result = 0;
	DWORD create_error = 0;

	hid_internal_lock_acquire(&hotplug_init_lock);
	if (hotplug_exiting) {
		result = HID_HOTPLUG_ENTER_EXITING;
	} else if (!bootstrap && !hid_hotplug_context.mutex_ready) {
		result = HID_HOTPLUG_ENTER_NOT_READY;
	} else if (hid_internal_hotplug_init_under_lock(&create_error) < 0) {
		result = HID_HOTPLUG_ENTER_FAILED;
	} else {
		hotplug_machinery_users++;
	}
	hid_internal_lock_release(&hotplug_init_lock);

	if (result == HID_HOTPLUG_ENTER_FAILED) {
		register_global_winapi_error_code(create_error, L"hid_hotplug_register_callback/CreateEvent");
	}

	return result;
}

static void hid_internal_hotplug_leave(void)
{
	hid_internal_lock_acquire(&hotplug_init_lock);
	hotplug_machinery_users--;
	hid_internal_lock_release(&hotplug_init_lock);
}

/* Whether hid_exit() is currently tearing the machinery down. Re-checked under
   the critical section by callers that were already counted in when hid_exit()
   started: `hotplug_exiting` may be raised while they hold - or wait on - the
   critical section, and they must not arm anything behind the teardown. */
static int hid_internal_hotplug_exiting(void)
{
	int exiting;

	hid_internal_lock_acquire(&hotplug_init_lock);
	exiting = (hotplug_exiting != 0);
	hid_internal_lock_release(&hotplug_init_lock);

	return exiting;
}

/* Whether the critical section exists and may be entered. Only used by
   hid_exit() itself (via hid_internal_hotplug_notification_leaked), on the same
   thread that is the only one allowed to destroy the machinery, so the answer
   cannot go stale between the check and the EnterCriticalSection. */
static int hid_internal_hotplug_ready(void)
{
	int ready;

	hid_internal_lock_acquire(&hotplug_init_lock);
	ready = hid_hotplug_context.mutex_ready;
	hid_internal_lock_release(&hotplug_init_lock);

	return ready;
}

int HID_API_EXPORT hid_init(void)
{
	register_global_error(NULL);

#ifndef HIDAPI_USE_DDK
	if (!hidapi_initialized) {
		if (lookup_functions() < 0) {
			register_global_winapi_error(L"resolve DLL functions");
			return -1;
		}
		hidapi_initialized = TRUE;
	}
#endif

	return 0;
}

struct hid_hotplug_callback {
    hid_hotplug_callback_handle handle;
    unsigned short vendor_id;
    unsigned short product_id;
    int events; /* bitmask of hid_hotplug_event */
    void *user_data;
    hid_hotplug_callback_fn callback;

    /* HID_API_HOTPLUG_ENUMERATE snapshot taken at registration time,
       still to be replayed to this callback as synthetic
       HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED events on the event context */
    struct hid_device_info *replay;

    /* Pointer to the next notification */
    struct hid_hotplug_callback *next;
};

static struct hid_device_info *hid_internal_copy_device_info(const struct hid_device_info *src)
{
	struct hid_device_info *dst = (struct hid_device_info *)calloc(1, sizeof(struct hid_device_info));

	if (dst == NULL) {
		return NULL;
	}

	*dst = *src;
	dst->next = NULL;
	dst->path = NULL;
	dst->serial_number = NULL;
	dst->manufacturer_string = NULL;
	dst->product_string = NULL;

	if ((src->path && (dst->path = _strdup(src->path)) == NULL)
	    || (src->serial_number && (dst->serial_number = _wcsdup(src->serial_number)) == NULL)
	    || (src->manufacturer_string && (dst->manufacturer_string = _wcsdup(src->manufacturer_string)) == NULL)
	    || (src->product_string && (dst->product_string = _wcsdup(src->product_string)) == NULL)) {
		hid_free_enumeration(dst);
		return NULL;
	}

	return dst;
}

/* Pins the module this code lives in, so that it stays mapped even if the
   application unloads hidapi: a notification that could not be unregistered is
   still live at the OS level and will call hid_internal_notify_callback.
   Any address inside the module identifies it; hid_hotplug_context is in the very
   same translation unit as the callback. */
static void hid_internal_hotplug_pin_module(void)
{
	HMODULE module = NULL;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		(LPCWSTR)(void *)&hid_hotplug_context, &module);
}

/* ASCII case folding: locale-independent, and all an interface path needs
   (this is also all the _stricmp() below used to do, in the C locale) */
static unsigned char hid_internal_ascii_tolower(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

/* Compares a cached (UTF-8) interface path with the (UTF-16) one a notification
   carries, WITHOUT ALLOCATING: it encodes the UTF-16 path to UTF-8 on the fly, one
   code point at a time, and matches it against the bytes of the cached one.

   Both hotplug cache lookups need this comparison, and both used to convert the
   notification's symbolic link to UTF-8 first - which allocates. That allocation
   failing in the REMOVAL lookup would process the removal but leave its cache
   record behind, and a stale record is unrecoverable: it is the arrival dedupe, so
   the next connection on that interface path (paths are reused when a device is
   replugged into the same port) would be classified as a duplicate and suppressed
   forever, for every callback, including later HID_API_HOTPLUG_ENUMERATE passes.
   With no allocation on either lookup, an out-of-memory condition can no longer
   desynchronize the cache from the system: the only allocation left is the one that
   describes an arriving device, and failing it simply does not cache the device
   (nothing was reported for it either - see hid_internal_notify_callback).

   The comparison is case-independent for ASCII, exactly like the _stricmp() it
   replaces. A cached path is always a WC_ERR_INVALID_CHARS conversion of an
   interface path (see hid_internal_UTF16toUTF8 and hid_internal_get_device_info),
   so it always encodes well-formed UTF-16: an ill-formed symbolic link cannot be in
   the cache, and comparing unequal is the correct answer for it - which is what the
   failing conversion used to yield as well. */
static int hid_internal_path_equals(const char *cached_path, const wchar_t *interface_path)
{
	const unsigned char *cached = (const unsigned char *)cached_path;

	while (*interface_path != L'\0') {
		unsigned long code_point = (unsigned long)*interface_path++;
		unsigned char utf8[4];
		size_t len, i;

		if (code_point >= 0xD800UL && code_point <= 0xDBFFUL) {
			/* A high surrogate must be followed by a low one */
			if (*interface_path < 0xDC00 || *interface_path > 0xDFFF) {
				return 0;
			}
			code_point = 0x10000UL + ((code_point - 0xD800UL) << 10) + (unsigned long)(*interface_path++ - 0xDC00);
		} else if (code_point >= 0xDC00UL && code_point <= 0xDFFFUL) {
			/* An unpaired low surrogate */
			return 0;
		}

		if (code_point < 0x80UL) {
			utf8[0] = (unsigned char)code_point;
			len = 1;
		} else if (code_point < 0x800UL) {
			utf8[0] = (unsigned char)(0xC0UL | (code_point >> 6));
			utf8[1] = (unsigned char)(0x80UL | (code_point & 0x3FUL));
			len = 2;
		} else if (code_point < 0x10000UL) {
			utf8[0] = (unsigned char)(0xE0UL | (code_point >> 12));
			utf8[1] = (unsigned char)(0x80UL | ((code_point >> 6) & 0x3FUL));
			utf8[2] = (unsigned char)(0x80UL | (code_point & 0x3FUL));
			len = 3;
		} else {
			utf8[0] = (unsigned char)(0xF0UL | (code_point >> 18));
			utf8[1] = (unsigned char)(0x80UL | ((code_point >> 12) & 0x3FUL));
			utf8[2] = (unsigned char)(0x80UL | ((code_point >> 6) & 0x3FUL));
			utf8[3] = (unsigned char)(0x80UL | (code_point & 0x3FUL));
			len = 4;
		}

		/* No byte of an encoded code point is ever '\0', so a cached path that ends
		   early simply compares unequal here: the walk cannot run past its end */
		for (i = 0; i < len; ++i) {
			if (hid_internal_ascii_tolower(*cached) != hid_internal_ascii_tolower(utf8[i])) {
				return 0;
			}
			++cached;
		}
	}

	return *cached == '\0';
}

/* Tells whether an interface path is already in the device cache - i.e. whether
   its connection has already been reported. Always called inside a locked mutex.

   This is the whole arrival dedupe. The notification is armed BEFORE the
   registration-time enumeration runs (a device connecting in between must not be
   missed by both), so a device that arrives in that window is captured by the
   enumeration AND has an arrival notification in flight; that notification must
   not report - or cache - the same connection a second time.

   The cache decides it, with no timing assumption anywhere: the OS delivers
   exactly one arrival notification per interface instance, so that overlap is the
   only way an arrival can be a duplicate - and in that case the enumeration has,
   by construction, already put the device in the cache. Conversely the removal
   notification is authoritative and drops the cache entry, so a genuine re-plug
   (even onto the same, reused path) is NOT cached and IS reported. */
static int hid_internal_hotplug_is_cached(const wchar_t *interface_path)
{
	for (struct hid_device_info *device = hid_hotplug_context.devs; device != NULL; device = device->next) {
		/* Case-independent path comparison is mandatory */
		if (device->path != NULL && hid_internal_path_equals(device->path, interface_path)) {
			return 1;
		}
	}

	return 0;
}

/* Unlinks the cached device with this interface path, if there is one, and hands
   it to the caller (who owns it). Always called inside a locked mutex. Allocates
   nothing: see hid_internal_path_equals. */
static struct hid_device_info *hid_internal_hotplug_take_cached_device(const wchar_t *interface_path)
{
	for (struct hid_device_info **current = &hid_hotplug_context.devs; *current != NULL; current = &(*current)->next) {
		/* Case-independent path comparison is mandatory */
		if ((*current)->path != NULL && hid_internal_path_equals((*current)->path, interface_path)) {
			struct hid_device_info *device = *current;
			*current = device->next;
			device->next = NULL;
			return device;
		}
	}

	return NULL;
}

static void hid_internal_hotplug_remove_postponed()
{
	/* Unregister the callbacks whose removal was postponed */
	/* This function is always called inside a locked mutex */
	/* However, any actions are only allowed if the mutex is NOT in use and if the DIRTY flag is set */
	if (!hid_hotplug_context.mutex_ready || hid_hotplug_context.mutex_in_use || !hid_hotplug_context.cb_list_dirty) {
		return;
	}
	
	/* Traverse the list of callbacks and check if any were marked for removal */
	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback *callback = *current;
		if (!callback->events) {
			*current = (*current)->next;
			hid_free_enumeration(callback->replay);
			free(callback);
			continue;
		}
		current = &callback->next;
	}

	/* Clear the flag so we don't start the cycle unless necessary */
	hid_hotplug_context.cb_list_dirty = 0;
}

/* Completes the unregistration of a notification handle detached by
   hid_internal_hotplug_cleanup. Must be called OUTSIDE the critical section:
   CM_Unregister_Notification waits for in-progress notification callbacks,
   which may themselves be blocked on the critical section. */
static void hid_internal_hotplug_finish_unregistration(HCMNOTIFICATION notify_handle)
{
	CONFIGRET cr;

	if (notify_handle == NULL) {
		return;
	}

	cr = CM_Unregister_Notification(notify_handle);

	EnterCriticalSection(&hid_hotplug_context.critical_section);
	if (cr != CR_SUCCESS) {
		/* The OS-side registration may still be live and call into this module at
		   any time. Everything it can reach has to survive: the critical section and
		   the quiescence event are never destroyed anyway, the resolved libraries are
		   never unloaded (see hid_exit), the module is pinned, and no second
		   notification is ever armed next to this one - the two would deliver every
		   event twice. hid_exit() reports the failure; this function also runs on the
		   internal event context, which must never touch the global error string (an
		   application thread may be reading it through hid_error(NULL)). */
		hid_hotplug_context.notification_leaked = 1;
		hid_hotplug_context.unregistration_failed = 1;
		hid_internal_hotplug_pin_module();
	}
	hid_hotplug_context.pending_unregistrations--;
	if (hid_hotplug_context.pending_unregistrations == 0) {
		SetEvent(hid_hotplug_context.quiescent_event);
	}
	LeaveCriticalSection(&hid_hotplug_context.critical_section);
}

/* Waits until every detached notification handle has been unregistered.
   Must be called WITHOUT the critical section: the unregistration completes on
   another thread, which needs it. */
static void hid_internal_hotplug_wait_quiescent(void)
{
	for (;;) {
		int pending;

		EnterCriticalSection(&hid_hotplug_context.critical_section);
		pending = (hid_hotplug_context.pending_unregistrations > 0);
		LeaveCriticalSection(&hid_hotplug_context.critical_section);

		if (!pending) {
			return;
		}

		if (WaitForSingleObject(hid_hotplug_context.quiescent_event, INFINITE) == WAIT_FAILED) {
			/* Should never happen; do not spin forever on a broken event */
			return;
		}
	}
}

/* Always called inside a locked mutex.
   When the last callback is gone, tears the machinery down and returns the
   Win32 notification handle, which the caller MUST hand to
   hid_internal_hotplug_finish_unregistration after leaving the critical
   section (never under it, and never from the notification callback itself:
   the notification callback defers the teardown to the threadpool work item). */
static HCMNOTIFICATION hid_internal_hotplug_cleanup()
{
	HCMNOTIFICATION notify_handle;

	if (!hid_hotplug_context.mutex_ready || hid_hotplug_context.mutex_in_use) {
		return NULL;
	}

	/* Before checking if the list is empty, clear any entries whose removal was postponed first */
	hid_internal_hotplug_remove_postponed();

	/* Unregister the HID device connection notification when removing the last callback */
	if (hid_hotplug_context.hotplug_cbs != NULL) {
		return NULL;
	}

	if (hid_hotplug_context.devs) {
		/* Cleanup connected device list */
		hid_free_enumeration(hid_hotplug_context.devs);
		hid_hotplug_context.devs = NULL;
	}

	notify_handle = hid_hotplug_context.notify_handle;
	hid_hotplug_context.notify_handle = NULL;
	if (notify_handle != NULL) {
		/* Balanced by hid_internal_hotplug_finish_unregistration */
		hid_hotplug_context.pending_unregistrations++;
		ResetEvent(hid_hotplug_context.quiescent_event);
	}
	return notify_handle;
}

/* Deliver (and consume) a callback's pending HID_API_HOTPLUG_ENUMERATE
   snapshot. Always called inside a locked mutex, with mutex_in_use set. */
static void hid_internal_hotplug_replay_flush(struct hid_hotplug_callback *callback)
{
	while (callback->replay != NULL) {
		struct hid_device_info *device = callback->replay;
		callback->replay = device->next;
		device->next = NULL;

		if (!callback->events) {
			/* The callback was deregistered while the pass was pending */
			hid_free_enumeration(device);
			continue;
		}

		int result = (*callback->callback)(callback->handle, device, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED, callback->user_data);
		hid_free_enumeration(device);

		/* A non-zero result stops the remainder of the pass and deregisters the callback */
		if (result) {
			callback->events = 0;
			hid_hotplug_context.cb_list_dirty = 1;
			hid_free_enumeration(callback->replay);
			callback->replay = NULL;
		}
	}
}

/* Threadpool work item: delivers pending HID_API_HOTPLUG_ENUMERATE passes
   (unless a live event got to them first) and performs the cleanup the
   notification callback is not allowed to perform itself. */
static VOID WINAPI hid_internal_hotplug_event_work(PVOID instance, PVOID context, PVOID work)
{
	HCMNOTIFICATION notify_handle;

	(void)instance;
	(void)context;
	(void)work;

	EnterCriticalSection(&hid_hotplug_context.critical_section);

	hid_hotplug_context.mutex_in_use = 1;
	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback != NULL; callback = callback->next) {
		hid_internal_hotplug_replay_flush(callback);
	}
	hid_hotplug_context.mutex_in_use = 0;

	notify_handle = hid_internal_hotplug_cleanup();

	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	hid_internal_hotplug_finish_unregistration(notify_handle);
}

/* Whether a notification could not be unregistered at some point in this process
   and may still be live at the OS level. Sticky: see hid_hotplug_context. */
static int hid_internal_hotplug_notification_leaked(void)
{
	int leaked;

	if (!hid_internal_hotplug_ready()) {
		return 0;
	}

	EnterCriticalSection(&hid_hotplug_context.critical_section);
	leaked = (hid_hotplug_context.notification_leaked != 0);
	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	return leaked;
}

/* Ends the teardown started by hid_internal_hotplug_exit: see `hotplug_exiting`.
   Called unconditionally by hid_exit() once the resolved libraries are gone. */
static void hid_internal_hotplug_exit_done(void)
{
	hid_internal_lock_acquire(&hotplug_init_lock);
	hotplug_exiting = 0;
	hid_internal_lock_release(&hotplug_init_lock);
}

static int hid_internal_hotplug_exit(void)
{
	HCMNOTIFICATION notify_handle;
	PVOID event_work;
	int failed;
	int keep_machinery;
	int machinery_ready;

	/* Nothing may enter or arm the machinery from here on. Without this, a
	   registration waiting for a pending unregistration to complete could wake up
	   behind this teardown, arm a fresh notification and append a callback to a
	   context that is being dismantled - leaving a live notification and a
	   "registered" callback behind hid_exit(). It is also what keeps a concurrent
	   hid_hotplug_register_callback() - which is thread-safe, and initializes the
	   library implicitly - from calling into hid.dll/cfgmgr32.dll while hid_exit()
	   is unloading them, and what makes the destruction at the end of this
	   function safe. Lowered again by hid_internal_hotplug_exit_done(). */
	hid_internal_lock_acquire(&hotplug_init_lock);
	hotplug_exiting = 1;
	machinery_ready = (hid_hotplug_context.mutex_ready != 0);
	hid_internal_lock_release(&hotplug_init_lock);

	if (!machinery_ready) {
		/* Hotplug was never used (or a previous hid_exit() already destroyed the
		   machinery): nothing can be armed, and nothing was created that would
		   have to be freed. A registration bootstrapping the machinery right now
		   fails on `hotplug_exiting` before it arms anything or calls into the
		   libraries hid_exit() is about to unload. */
		return 0;
	}

	EnterCriticalSection(&hid_hotplug_context.critical_section);

	/* Remove all callbacks from the list, including their undelivered HID_API_HOTPLUG_ENUMERATE snapshots */
	{
		struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
		while (*current) {
			struct hid_hotplug_callback *next = (*current)->next;
			hid_free_enumeration((*current)->replay);
			free(*current);
			*current = next;
		}
	}
	notify_handle = hid_internal_hotplug_cleanup();
	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	hid_internal_hotplug_finish_unregistration(notify_handle);

	/* Quiescence: unregistrations started by concurrent (contract-legal)
	   hid_hotplug_deregister_callback calls must complete before anything a
	   notification can still reach is released */
	hid_internal_hotplug_wait_quiescent();

	EnterCriticalSection(&hid_hotplug_context.critical_section);
	if (hid_hotplug_context.pending_unregistrations > 0) {
		/* Only reachable if waiting itself failed: treat it as a live notification */
		hid_hotplug_context.notification_leaked = 1;
		hid_hotplug_context.unregistration_failed = 1;
		hid_internal_hotplug_pin_module();
	}
	/* Scoped to this teardown: a later hid_exit() has nothing of its own to report
	   (a hotplug callback can no longer be registered once a notification leaked) */
	failed = (hid_hotplug_context.unregistration_failed != 0);
	hid_hotplug_context.unregistration_failed = 0;

	event_work = hid_hotplug_context.event_work;
	/* `failed` implies notification_leaked: they are only ever set together.
	   Stable at this point: no unregistration is pending anymore, and nothing
	   that could start one can enter behind `hotplug_exiting`. */
	keep_machinery = (hid_hotplug_context.notification_leaked != 0);
	if (!keep_machinery) {
		hid_hotplug_context.event_work = NULL;
	} else {
		/* A notification may still fire and submit to the work item: keep it
		   (deliberately leaked), along with the critical section, the quiescence
		   event and the device cache it works on */
		event_work = NULL;
	}
	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	if (event_work != NULL) {
		/* No notification is registered anymore (CM_Unregister_Notification only
		   returns once its callbacks have finished) and nothing can submit new work
		   while `exiting` is set, so the work item can be drained and closed.
		   Not under the critical section: the work item takes it.
		   hid_exit() must not be called from a hotplug callback, so the wait cannot
		   deadlock on the calling thread itself. */
		hid_internal_WaitForThreadpoolWorkCallbacks(event_work, FALSE);
		hid_internal_CloseThreadpoolWork(event_work);
	}

	EnterCriticalSection(&hid_hotplug_context.critical_section);
	/* A last-gasp notification callback may have re-added a device between the
	   cleanup and the completion of the unregistration */
	hid_free_enumeration(hid_hotplug_context.devs);
	hid_hotplug_context.devs = NULL;
	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	if (!keep_machinery) {
		/* hid_exit() frees what the machinery created - one kernel event and one
		   critical section: a plugin-style host that repeatedly loads, uses and
		   unloads the library must not accumulate OS objects. Destroying them is
		   safe only once nothing can be inside the critical section: every
		   notification is unregistered and the work item is drained and closed
		   (established above), `hotplug_exiting` (still raised) keeps new callers
		   out, so only callers already counted in remain - wait those out. They
		   fail out promptly on their `exiting` checks. */
		for (;;) {
			int busy;

			hid_internal_lock_acquire(&hotplug_init_lock);
			busy = (hotplug_machinery_users != 0);
			if (!busy) {
				/* Unpublished under the same acquisition that proved the machinery
				   idle: with `hotplug_exiting` raised nothing can be counted back
				   in, so the destruction below cannot race anything. */
				hid_hotplug_context.mutex_ready = 0;
			}
			hid_internal_lock_release(&hotplug_init_lock);

			if (!busy) {
				break;
			}
			Sleep(1);
		}

		DeleteCriticalSection(&hid_hotplug_context.critical_section);
		CloseHandle(hid_hotplug_context.quiescent_event);
		hid_hotplug_context.quiescent_event = NULL;
	}
	/* else: the machinery outlives hid_exit() on purpose - a leaked notification
	   may still enter the critical section at any time
	   (see hid_internal_hotplug_init_under_lock) */

	/* `hotplug_exiting` stays raised until hid_exit() is done unloading the
	   libraries: see hid_internal_hotplug_exit_done */

	if (failed) {
		register_global_error(L"hid_exit: a hotplug notification could not be unregistered");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
	int result = hid_internal_hotplug_exit();

#ifndef HIDAPI_USE_DDK
	if (!hid_internal_hotplug_notification_leaked()) {
		/* Still under `exiting`: a concurrent (thread-safe) hotplug registration
		   fails instead of calling into libraries that are being unloaded here.
		   Every other call that goes through them either holds the critical section
		   or is accounted for by pending_unregistrations, which the teardown above
		   has already waited out. */
		free_library_handles();
		hidapi_initialized = FALSE;
	}
	/* else: a hotplug notification is still registered at the OS level and its
	   callback calls into hid.dll/cfgmgr32.dll through these handles: they stay
	   loaded (deliberately leaked, and the module is pinned) so that a late
	   notification never calls into unloaded code */
#endif

	hid_internal_hotplug_exit_done();

	if (result < 0) {
		/* register_global_error: set by hid_internal_hotplug_exit */
		return -1;
	}

	register_global_error(NULL);

	return 0;
}

static void* hid_internal_get_devnode_property(DEVINST dev_node, const DEVPROPKEY* property_key, DEVPROPTYPE expected_property_type)
{
	ULONG len = 0;
	CONFIGRET cr;
	DEVPROPTYPE property_type;
	PBYTE property_value = NULL;

	cr = CM_Get_DevNode_PropertyW(dev_node, property_key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_property_type)
		return NULL;

	property_value = (PBYTE)calloc(len, sizeof(BYTE));
	cr = CM_Get_DevNode_PropertyW(dev_node, property_key, &property_type, property_value, &len, 0);
	if (cr != CR_SUCCESS) {
		free(property_value);
		return NULL;
	}

	return property_value;
}

static void* hid_internal_get_device_interface_property(const wchar_t* interface_path, const DEVPROPKEY* property_key, DEVPROPTYPE expected_property_type)
{
	ULONG len = 0;
	CONFIGRET cr;
	DEVPROPTYPE property_type;
	PBYTE property_value = NULL;

	cr = CM_Get_Device_Interface_PropertyW(interface_path, property_key, &property_type, NULL, &len, 0);
	if (cr != CR_BUFFER_SMALL || property_type != expected_property_type)
		return NULL;

	property_value = (PBYTE)calloc(len, sizeof(BYTE));
	cr = CM_Get_Device_Interface_PropertyW(interface_path, property_key, &property_type, property_value, &len, 0);
	if (cr != CR_SUCCESS) {
		free(property_value);
		return NULL;
	}

	return property_value;
}

static void hid_internal_towupper(wchar_t* string)
{
	for (wchar_t* p = string; *p; ++p) *p = towupper(*p);
}

static int hid_internal_extract_int_token_value(wchar_t* string, const wchar_t* token)
{
	int token_value;
	wchar_t* startptr, * endptr;

	startptr = wcsstr(string, token);
	if (!startptr)
		return -1;

	startptr += wcslen(token);
	token_value = wcstol(startptr, &endptr, 16);
	if (endptr == startptr)
		return -1;

	return token_value;
}

static void hid_internal_get_usb_info(struct hid_device_info* dev, DEVINST dev_node)
{
	wchar_t *device_id = NULL, *hardware_ids = NULL;

	device_id = (wchar_t *)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!device_id)
		goto end;

	/* Normalize to upper case */
	hid_internal_towupper(device_id);

	/* Check for Xbox Common Controller class (XUSB) device.
	   https://docs.microsoft.com/windows/win32/xinput/directinput-and-xusb-devices
	   https://docs.microsoft.com/windows/win32/xinput/xinput-and-directinput
	*/
	if (hid_internal_extract_int_token_value(device_id, L"IG_") != -1) {
		/* Get devnode parent to reach out USB device. */
		if (CM_Get_Parent(&dev_node, dev_node, 0) != CR_SUCCESS)
			goto end;
	}

	/* Get the hardware ids from devnode */
	hardware_ids = (wchar_t *)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_HardwareIds, DEVPROP_TYPE_STRING_LIST);
	if (!hardware_ids)
		goto end;

	/* Get additional information from USB device's Hardware ID
	   https://docs.microsoft.com/windows-hardware/drivers/install/standard-usb-identifiers
	   https://docs.microsoft.com/windows-hardware/drivers/usbcon/enumeration-of-interfaces-not-grouped-in-collections
	*/
	for (wchar_t* hardware_id = hardware_ids; *hardware_id; hardware_id += wcslen(hardware_id) + 1) {
		/* Normalize to upper case */
		hid_internal_towupper(hardware_id);

		if (dev->release_number == 0) {
			/* USB_DEVICE_DESCRIPTOR.bcdDevice value. */
			int release_number = hid_internal_extract_int_token_value(hardware_id, L"REV_");
			if (release_number != -1) {
				dev->release_number = (unsigned short)release_number;
			}
		}

		if (dev->interface_number == -1) {
			/* USB_INTERFACE_DESCRIPTOR.bInterfaceNumber value. */
			int interface_number = hid_internal_extract_int_token_value(hardware_id, L"MI_");
			if (interface_number != -1) {
				dev->interface_number = interface_number;
			}
		}
	}

	/* Try to get USB device manufacturer string if not provided by HidD_GetManufacturerString. */
	if (wcslen(dev->manufacturer_string) == 0) {
		wchar_t* manufacturer_string = (wchar_t *)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_Manufacturer, DEVPROP_TYPE_STRING);
		if (manufacturer_string) {
			free(dev->manufacturer_string);
			dev->manufacturer_string = manufacturer_string;
		}
	}

	/* Try to get USB device serial number if not provided by HidD_GetSerialNumberString. */
	if (wcslen(dev->serial_number) == 0) {
		DEVINST usb_dev_node = dev_node;
		if (dev->interface_number != -1) {
			/* Get devnode parent to reach out composite parent USB device.
			   https://docs.microsoft.com/windows-hardware/drivers/usbcon/enumeration-of-the-composite-parent-device
			*/
			if (CM_Get_Parent(&usb_dev_node, dev_node, 0) != CR_SUCCESS)
				goto end;
		}

		/* Get the device id of the USB device. */
		free(device_id);
		device_id = (wchar_t *)hid_internal_get_devnode_property(usb_dev_node, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
		if (!device_id)
			goto end;

		/* Extract substring after last '\\' of Instance ID.
		   For USB devices it may contain device's serial number.
		   https://docs.microsoft.com/windows-hardware/drivers/install/instance-ids
		*/
		for (wchar_t *ptr = device_id + wcslen(device_id); ptr > device_id; --ptr) {
			/* Instance ID is unique only within the scope of the bus.
			   For USB devices it means that serial number is not available. Skip. */
			if (*ptr == L'&')
				break;

			if (*ptr == L'\\') {
				free(dev->serial_number);
				dev->serial_number = _wcsdup(ptr + 1);
				break;
			}
		}
	}

	/* If we can't get the interface number, it means that there is only one interface. */
	if (dev->interface_number == -1)
		dev->interface_number = 0;

end:
	free(device_id);
	free(hardware_ids);
}

/* HidD_GetProductString/HidD_GetManufacturerString/HidD_GetSerialNumberString is not working for BLE HID devices
   Request this info via dev node properties instead.
   https://docs.microsoft.com/answers/questions/401236/hidd-getproductstring-with-ble-hid-device.html
*/
static void hid_internal_get_ble_info(struct hid_device_info* dev, DEVINST dev_node)
{
	if (wcslen(dev->manufacturer_string) == 0) {
		/* Manufacturer Name String (UUID: 0x2A29) */
		wchar_t* manufacturer_string = (wchar_t *)hid_internal_get_devnode_property(dev_node, (const DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_Manufacturer, DEVPROP_TYPE_STRING);
		if (manufacturer_string) {
			free(dev->manufacturer_string);
			dev->manufacturer_string = manufacturer_string;
		}
	}

	if (wcslen(dev->serial_number) == 0) {
		/* Serial Number String (UUID: 0x2A25) */
		wchar_t* serial_number = (wchar_t *)hid_internal_get_devnode_property(dev_node, (const DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_DeviceAddress, DEVPROP_TYPE_STRING);
		if (serial_number) {
			free(dev->serial_number);
			dev->serial_number = serial_number;
		}
	}

	if (wcslen(dev->product_string) == 0) {
		/* Model Number String (UUID: 0x2A24) */
		wchar_t* product_string = (wchar_t *)hid_internal_get_devnode_property(dev_node, (const DEVPROPKEY*)&PKEY_DeviceInterface_Bluetooth_ModelNumber, DEVPROP_TYPE_STRING);
		if (!product_string) {
			DEVINST parent_dev_node = 0;
			/* Fallback: Get devnode grandparent to reach out Bluetooth LE device node */
			if (CM_Get_Parent(&parent_dev_node, dev_node, 0) == CR_SUCCESS) {
				/* Device Name (UUID: 0x2A00) */
				product_string = (wchar_t *)hid_internal_get_devnode_property(parent_dev_node, &DEVPKEY_NAME, DEVPROP_TYPE_STRING);
			}
		}

		if (product_string) {
			free(dev->product_string);
			dev->product_string = product_string;
		}
	}
}

static int hid_internal_match_device_id(unsigned short vendor_id, unsigned short product_id, unsigned short expected_vendor_id, unsigned short expected_product_id)
{
	return (expected_vendor_id == 0x0 || vendor_id == expected_vendor_id) && (expected_product_id == 0x0 || product_id == expected_product_id);
}

/* Unfortunately, HID_API_BUS_xxx constants alone aren't enough to distinguish between BLUETOOTH and BLE */
#define HID_API_BUS_FLAG_BLE 0x01

typedef struct hid_internal_detect_bus_type_result_ {
	DEVINST dev_node;
	hid_bus_type bus_type;
	unsigned int bus_flags;
} hid_internal_detect_bus_type_result;

static hid_internal_detect_bus_type_result hid_internal_detect_bus_type(const wchar_t* interface_path)
{
	wchar_t *device_id = NULL, *compatible_ids = NULL;
	CONFIGRET cr;
	DEVINST dev_node;
	hid_internal_detect_bus_type_result result;

	memset(&result, 0, sizeof(result));

	/* Get the device id from interface path */
	device_id = (wchar_t *)hid_internal_get_device_interface_property(interface_path, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!device_id)
		goto end;

	/* Open devnode from device id */
	cr = CM_Locate_DevNodeW(&dev_node, (DEVINSTID_W)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get devnode parent */
	cr = CM_Get_Parent(&dev_node, dev_node, 0);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get the compatible ids from parent devnode */
	compatible_ids = (wchar_t *)hid_internal_get_devnode_property(dev_node, &DEVPKEY_Device_CompatibleIds, DEVPROP_TYPE_STRING_LIST);
	if (!compatible_ids)
		goto end;

	/* Now we can parse parent's compatible IDs to find out the device bus type */
	for (wchar_t* compatible_id = compatible_ids; *compatible_id; compatible_id += wcslen(compatible_id) + 1) {
		/* Normalize to upper case */
		hid_internal_towupper(compatible_id);

		/* USB devices
		   https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-support
		   https://docs.microsoft.com/windows-hardware/drivers/install/standard-usb-identifiers */
		if (wcsstr(compatible_id, L"USB") != NULL) {
			result.bus_type = HID_API_BUS_USB;
			break;
		}

		/* Bluetooth devices
		   https://docs.microsoft.com/windows-hardware/drivers/bluetooth/installing-a-bluetooth-device */
		if (wcsstr(compatible_id, L"BTHENUM") != NULL) {
			result.bus_type = HID_API_BUS_BLUETOOTH;
			break;
		}

		/* Bluetooth LE devices */
		if (wcsstr(compatible_id, L"BTHLEDEVICE") != NULL) {
			result.bus_type = HID_API_BUS_BLUETOOTH;
			result.bus_flags |= HID_API_BUS_FLAG_BLE;
			break;
		}

		/* I2C devices
		   https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-support-and-power-management */
		if (wcsstr(compatible_id, L"PNP0C50") != NULL) {
			result.bus_type = HID_API_BUS_I2C;
			break;
		}

		/* SPI devices
		   https://docs.microsoft.com/windows-hardware/drivers/hid/plug-and-play-for-spi */
		if (wcsstr(compatible_id, L"PNP0C51") != NULL) {
			result.bus_type = HID_API_BUS_SPI;
			break;
		}
	}

	result.dev_node = dev_node;

end:
	free(device_id);
	free(compatible_ids);
	return result;
}

/* Returns NULL both when `src` is not valid UTF-16 and on allocation failure.
   Callers that need to tell the two apart (an invalid string is the string's
   problem; running out of memory is a library failure) pass `oom`, which is set
   to 1 on allocation failure and left untouched otherwise. */
static char *hid_internal_UTF16toUTF8(const wchar_t *src, int *oom)
{
	char *dst = NULL;
	int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
	if (len) {
		dst = (char*)calloc(len, sizeof(char));
		if (dst == NULL) {
			if (oom) {
				*oom = 1;
			}
			return NULL;
		}
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, len, NULL, NULL);
	}

	return dst;
}

static wchar_t *hid_internal_UTF8toUTF16(const char *src)
{
	wchar_t *dst = NULL;
	int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
	if (len) {
		dst = (wchar_t*)calloc(len, sizeof(wchar_t));
		if (dst == NULL) {
			return NULL;
		}
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, len);
	}

	return dst;
}

/* Returns NULL when the device cannot be described: because the process is out
   of memory (`oom` - when passed - is set to 1; a library failure) or because
   `path` is not valid UTF-16 (`oom` is left untouched; the device is simply not
   representable and callers skip it). */
static struct hid_device_info *hid_internal_get_device_info(const wchar_t *path, HANDLE handle, int *oom)
{
	struct hid_device_info *dev = NULL; /* return object */
	HIDD_ATTRIBUTES attrib;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	HIDP_CAPS caps;
	wchar_t string[MAX_STRING_WCHARS + 1];
	ULONG len;
	ULONG size;
	hid_internal_detect_bus_type_result detect_bus_type_result;

	/* Create the record. */
	dev = (struct hid_device_info*)calloc(1, sizeof(struct hid_device_info));

	if (dev == NULL) {
		if (oom) {
			*oom = 1;
		}
		return NULL;
	}

	/* Fill out the record */
	dev->next = NULL;
	dev->path = hid_internal_UTF16toUTF8(path, oom);
	if (dev->path == NULL) {
		/* A record without a path is useless to the caller and unusable as the key
		   of the hotplug device cache (where it would crash the removal lookup) */
		free(dev);
		return NULL;
	}
	dev->interface_number = -1;

	attrib.Size = sizeof(HIDD_ATTRIBUTES);
	if (HidD_GetAttributes(handle, &attrib)) {
		/* VID/PID */
		dev->vendor_id = attrib.VendorID;
		dev->product_id = attrib.ProductID;

		/* Release Number */
		dev->release_number = attrib.VersionNumber;
	}

	/* Get the Usage Page and Usage for this device. */
	if (HidD_GetPreparsedData(handle, &pp_data)) {
		if (HidP_GetCaps(pp_data, &caps) == HIDP_STATUS_SUCCESS) {
			dev->usage_page = caps.UsagePage;
			dev->usage = caps.Usage;
		}

		HidD_FreePreparsedData(pp_data);
	}

	/* detect bus type before reading string descriptors */
	detect_bus_type_result = hid_internal_detect_bus_type(path);
	dev->bus_type = detect_bus_type_result.bus_type;

	len = dev->bus_type == HID_API_BUS_USB ? MAX_STRING_WCHARS_USB : MAX_STRING_WCHARS;
	string[len] = L'\0';
	size = len * sizeof(wchar_t);

	/* Serial Number */
	string[0] = L'\0';
	HidD_GetSerialNumberString(handle, string, size);
	dev->serial_number = _wcsdup(string);

	/* Manufacturer String */
	string[0] = L'\0';
	HidD_GetManufacturerString(handle, string, size);
	dev->manufacturer_string = _wcsdup(string);

	/* Product String */
	string[0] = L'\0';
	HidD_GetProductString(handle, string, size);
	dev->product_string = _wcsdup(string);

	if (dev->serial_number == NULL || dev->manufacturer_string == NULL || dev->product_string == NULL) {
		/* Out of memory. A half-built record is not a device (and the bus-specific
		   fixups right below dereference these strings) */
		if (oom) {
			*oom = 1;
		}
		hid_free_enumeration(dev);
		return NULL;
	}

	/* now, the portion that depends on string descriptors */
	switch (dev->bus_type) {
	case HID_API_BUS_USB:
		hid_internal_get_usb_info(dev, detect_bus_type_result.dev_node);
		break;

	case HID_API_BUS_BLUETOOTH:
		if (detect_bus_type_result.bus_flags & HID_API_BUS_FLAG_BLE)
			hid_internal_get_ble_info(dev, detect_bus_type_result.dev_node);
		break;

	case HID_API_BUS_UNKNOWN:
	case HID_API_BUS_SPI:
	case HID_API_BUS_I2C:
	case HID_API_BUS_VIRTUAL:
		/* shut down -Wswitch */
		break;
	}

	return dev;
}

/* Same as hid_enumerate, but distinguishes a genuine failure (*failure set
   to 1, error registered) from an empty result (NULL with *failure left 0) */
static struct hid_device_info *hid_internal_enumerate(unsigned short vendor_id, unsigned short product_id, int *failure)
{
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	GUID interface_class_guid;
	CONFIGRET cr;
	wchar_t* device_interface_list = NULL;
	DWORD len;

	*failure = 1;

	if (hid_init() < 0) {
		/* register_global_error: global error is reset by hid_init */
		return NULL;
	}

	/* Retrieve HID Interface Class GUID
	   https://docs.microsoft.com/windows-hardware/drivers/install/guid-devinterface-hid */
	HidD_GetHidGuid(&interface_class_guid);

	/* Get the list of all device interfaces belonging to the HID class. */
	/* Retry in case of list was changed between calls to
	  CM_Get_Device_Interface_List_SizeW and CM_Get_Device_Interface_ListW */
	do {
		cr = CM_Get_Device_Interface_List_SizeW(&len, &interface_class_guid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
		if (cr != CR_SUCCESS) {
			register_global_error(L"Failed to get size of HID device interface list");
			break;
		}

		if (device_interface_list != NULL) {
			free(device_interface_list);
		}

		device_interface_list = (wchar_t*)calloc(len, sizeof(wchar_t));
		if (device_interface_list == NULL) {
			register_global_error(L"Failed to allocate memory for HID device interface list");
			return NULL;
		}
		cr = CM_Get_Device_Interface_ListW(&interface_class_guid, NULL, device_interface_list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
		if (cr != CR_SUCCESS && cr != CR_BUFFER_SMALL) {
			register_global_error(L"Failed to get HID device interface list");
		}
	} while (cr == CR_BUFFER_SMALL);

	if (cr != CR_SUCCESS) {
		goto end_of_function;
	}

	*failure = 0;

	/* Iterate over each device interface in the HID class, looking for the right one. */
	for (wchar_t* device_interface = device_interface_list; *device_interface; device_interface += wcslen(device_interface) + 1) {
		HANDLE device_handle = INVALID_HANDLE_VALUE;
		HIDD_ATTRIBUTES attrib;

		/* Open read-only handle to the device */
		device_handle = open_device(device_interface, FALSE);

		/* Check validity of device_handle. */
		if (device_handle == INVALID_HANDLE_VALUE) {
			/* Unable to open the device. */
			continue;
		}

		/* Get the Vendor ID and Product ID for this device. */
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		if (!HidD_GetAttributes(device_handle, &attrib)) {
			goto cont_close;
		}

		/* Check the VID/PID to see if we should add this
		   device to the enumeration list. */
		if (hid_internal_match_device_id(attrib.VendorID, attrib.ProductID, vendor_id, product_id)) {
			/* VID/PID match. Create the record. */
			int oom = 0;
			struct hid_device_info *tmp = hid_internal_get_device_info(device_interface, device_handle, &oom);

			if (tmp == NULL && !oom) {
				/* The interface path is not valid UTF-16: the device cannot be
				   represented to the caller. Skip it - consistently with the
				   hotplug notifications, which cannot report (or cache) such a
				   device either, so the device cache stays in step with this list. */
				goto cont_close;
			}

			if (tmp == NULL) {
				/* Out of memory. Report a failure rather than a partial list: a
				   list that silently misses devices is indistinguishable from the
				   system not having them, and the hotplug device cache and the
				   HID_API_HOTPLUG_ENUMERATE snapshot are built from it. */
				register_global_error(L"Failed to allocate memory for a device info");
				*failure = 1;
				CloseHandle(device_handle);
				hid_free_enumeration(root);
				root = NULL;
				goto end_of_function;
			}

			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;
		}

cont_close:
		CloseHandle(device_handle);
	}

	if (root == NULL) {
		if (vendor_id == 0 && product_id == 0) {
			register_global_error(L"No HID devices found in the system.");
		} else {
			register_global_error(L"No HID devices with requested VID/PID found in the system.");
		}
	}

end_of_function:
	free(device_interface_list);

	return root;
}

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	int failure = 0;
	return hid_internal_enumerate(vendor_id, product_id, &failure);
}

void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
	/* TODO: Merge this with the Linux version. This function is platform-independent. */
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}

/* Delivers one live event to every matching callback registered at this moment.
   Always called inside a locked mutex. Does not consume `device`. */
static void hid_internal_hotplug_dispatch(struct hid_device_info *device, hid_hotplug_event hotplug_event)
{
	/* Mark the critical section as IN USE, to prevent callback removal from inside a callback */
	hid_hotplug_context.mutex_in_use = 1;

	/* Callbacks registered from inside a callback are appended to the list
	   and see this device in their registration-time HID_API_HOTPLUG_ENUMERATE
	   snapshot (or don't, for a removal): the live dispatch is bound to the
	   callbacks present at event time, so the connection is reported exactly once */
	struct hid_hotplug_callback *last_at_event = hid_hotplug_context.hotplug_cbs;
	while (last_at_event != NULL && last_at_event->next != NULL) {
		last_at_event = last_at_event->next;
	}

	/* Call the notifications for the device */
	for (struct hid_hotplug_callback *callback = hid_hotplug_context.hotplug_cbs; callback != NULL; callback = callback->next) {
		/* The registration-time enumeration pass is always delivered
		   before any live events for the callback */
		hid_internal_hotplug_replay_flush(callback);

		if ((callback->events & hotplug_event) && hid_internal_match_device_id(device->vendor_id, device->product_id, callback->vendor_id, callback->product_id)) {
			int result = (callback->callback)(callback->handle, device, hotplug_event, callback->user_data);

			/* If the result is non-zero, we MARK the callback for future removal and proceed */
			/* We avoid changing the list until we are done calling the callbacks to simplify the process */
			if (result) {
				callback->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			}
		}

		if (callback == last_at_event) {
			break;
		}
	}

	hid_hotplug_context.mutex_in_use = 0;
}

DWORD WINAPI hid_internal_notify_callback(HCMNOTIFICATION notify, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA event_data, DWORD event_data_size)
{
	struct hid_device_info *device = NULL;
	hid_hotplug_event hotplug_event = (hid_hotplug_event)0;

	(void)notify;
	(void)context;
	(void)event_data_size;

	if (event_data == NULL || event_data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) {
		return ERROR_SUCCESS;
	}

	/* Lock the mutex to avoid race conditions */
	EnterCriticalSection(&hid_hotplug_context.critical_section);

	if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
		hotplug_event = HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED;

		if (hid_internal_hotplug_is_cached(event_data->u.DeviceInterface.SymbolicLink)) {
			/* This connection is already known - and therefore already reported.
			   The only way that happens is the arm-before-enumerate window at
			   registration, where the enumeration cached the device and the OS had
			   already queued this arrival for it (see
			   hid_internal_hotplug_is_cached). Drop it whole: no second cache
			   entry, no second dispatch. */
		} else {
			/* Open read-only handle to the device */
			HANDLE read_handle = open_device(event_data->u.DeviceInterface.SymbolicLink, FALSE);

			/* Check validity of read_handle. */
			if (read_handle != INVALID_HANDLE_VALUE) {
				device = hid_internal_get_device_info(event_data->u.DeviceInterface.SymbolicLink, read_handle, NULL);
				CloseHandle(read_handle);
			}

			if (device != NULL) {
				/* Append to the end of the device list */
				if (hid_hotplug_context.devs != NULL) {
					struct hid_device_info *last = hid_hotplug_context.devs;
					while (last->next != NULL) {
						last = last->next;
					}
					last->next = device;
				} else {
					hid_hotplug_context.devs = device;
				}
			}
			/* else: the interface could not be opened or described. The usual cause
			   is that it disconnected again before we got to it - no connection is
			   owed then: nothing is cached, so the removal notification that follows
			   finds nothing and reports nothing either, which is consistent. An
			   out-of-memory failure does lose the connection, but there is nothing to
			   report it with; the cache is left consistent either way, as only fully
			   described devices are ever cached. */
		}
	} else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
		hotplug_event = HID_API_HOTPLUG_EVENT_DEVICE_LEFT;

		/* Get and remove this device from the device list. Dropping the entry is
		   what makes a later arrival on the same path (an interface path is reused
		   when a device is replugged into the same port) a new connection rather
		   than a duplicate - see hid_internal_hotplug_is_cached. The lookup cannot
		   fail for lack of memory (it allocates nothing), so a removal can never
		   leave its record behind. */
		device = hid_internal_hotplug_take_cached_device(event_data->u.DeviceInterface.SymbolicLink);
	}

	if (device) {
		hid_internal_hotplug_dispatch(device, hotplug_event);

		/* Free removed device */
		if (hotplug_event == HID_API_HOTPLUG_EVENT_DEVICE_LEFT) {
			hid_free_enumeration(device);
		}

		/* Remove any callbacks that were marked for removal; if none are left,
		   defer the teardown to the threadpool work item: unregistering the
		   notification from its own callback is not allowed (deadlock) */
		hid_internal_hotplug_remove_postponed();
		if (hid_hotplug_context.hotplug_cbs == NULL && hid_hotplug_context.event_work != NULL) {
			hid_internal_SubmitThreadpoolWork(hid_hotplug_context.event_work);
		}
	}

	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	return ERROR_SUCCESS;
}

/* The registration steps that run inside the machinery. The caller has already
   counted itself in with hid_internal_hotplug_enter - which is what keeps the
   critical section alive for the whole call - and balances that with
   hid_internal_hotplug_leave afterwards. Takes ownership of hotplug_cb: it is
   freed on failure. */
static int hid_internal_hotplug_register_counted(struct hid_hotplug_callback *hotplug_cb, int flags, hid_hotplug_callback_handle *callback_handle)
{
	/* Lock the mutex to avoid race conditions */
	EnterCriticalSection(&hid_hotplug_context.critical_section);

	for (;;) {
		if (hid_internal_hotplug_exiting()) {
			/* hid_exit() started tearing the machinery down after this call was
			   counted in: arming it again behind its back would leave a live
			   notification and a registered callback with no context to run in */
			register_global_error(L"hid_exit() is in progress");
			LeaveCriticalSection(&hid_hotplug_context.critical_section);
			free(hotplug_cb);
			return -1;
		}

		/* A notification detached by a concurrent deregistration may still be live
		   at the OS until its unregistration completes; arming a replacement in
		   that window would deliver every event twice. */
		if (hid_hotplug_context.hotplug_cbs != NULL || hid_hotplug_context.notify_handle != NULL
		    || hid_hotplug_context.pending_unregistrations == 0) {
			break;
		}

		/* The unregistration completes on another thread (or on the event context),
		   which needs the critical section, so the wait must not hold it. On wake the
		   state is re-evaluated from scratch.
		   This LeaveCriticalSection releases the critical section completely only
		   because it cannot be held recursively here: on the event context (the one
		   place this function runs with the critical section already held, via a
		   user callback registering a callback) hotplug_cbs is never NULL, so the
		   loop has already exited above. */
		LeaveCriticalSection(&hid_hotplug_context.critical_section);
		if (WaitForSingleObject(hid_hotplug_context.quiescent_event, INFINITE) == WAIT_FAILED) {
			register_global_winapi_error(L"hid_hotplug_register_callback/WaitForSingleObject");
			free(hotplug_cb);
			return -1;
		}
		EnterCriticalSection(&hid_hotplug_context.critical_section);
	}

	if (hid_hotplug_context.notification_leaked) {
		/* A notification could not be unregistered and may still be live at the OS
		   level: a second one would deliver every event twice, to every callback.
		   The condition is sticky (and unreachable in practice), so hotplug stays
		   unavailable for the rest of the process. */
		register_global_error(L"A hotplug notification could not be unregistered: hotplug is no longer available");
		LeaveCriticalSection(&hid_hotplug_context.critical_section);
		free(hotplug_cb);
		return -1;
	}

	/* Handle values are never reused while the library remains initialized */
	if (hid_hotplug_context.next_handle >= INT_MAX) {
		register_global_error(L"Hotplug callback handles exhausted");
		LeaveCriticalSection(&hid_hotplug_context.critical_section);
		free(hotplug_cb);
		return -1;
	}
	hotplug_cb->handle = hid_hotplug_context.next_handle++;

	/* Start the machinery with the first callback */
	if (hid_hotplug_context.hotplug_cbs == NULL) {
		if (hid_init() < 0) {
			/* register_global_error: global error is already set by hid_init */
			LeaveCriticalSection(&hid_hotplug_context.critical_section);
			free(hotplug_cb);
			return -1;
		}

		if (hid_internal_hotplug_resolve_threadpool() < 0) {
			register_global_error(L"Hotplug is not supported: the threadpool API is unavailable");
			LeaveCriticalSection(&hid_hotplug_context.critical_section);
			free(hotplug_cb);
			return -1;
		}

		if (hid_hotplug_context.event_work == NULL) {
			hid_hotplug_context.event_work = hid_internal_CreateThreadpoolWork(hid_internal_hotplug_event_work, NULL, NULL);
			if (hid_hotplug_context.event_work == NULL) {
				register_global_winapi_error(L"hid_hotplug_register_callback/CreateThreadpoolWork");
				LeaveCriticalSection(&hid_hotplug_context.critical_section);
				free(hotplug_cb);
				return -1;
			}
		}

		if (hid_hotplug_context.notify_handle == NULL) {
			GUID interface_class_guid;
			CM_NOTIFY_FILTER notify_filter;
			int enumerate_failure = 0;

			memset(&notify_filter, 0, sizeof(notify_filter));

			/* Retrieve HID Interface Class GUID
				https://docs.microsoft.com/windows-hardware/drivers/install/guid-devinterface-hid */
			HidD_GetHidGuid(&interface_class_guid);

			notify_filter.cbSize = sizeof(notify_filter);
			notify_filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
			notify_filter.u.DeviceInterface.ClassGuid = interface_class_guid;

			/* Register for a HID device notification when adding the first callback.
			   Armed BEFORE the device cache is filled: a device connecting in between
			   is then caught by the notification instead of being missed by both, and
			   the duplicate that this creates is suppressed by the cache itself
			   (see hid_internal_hotplug_is_cached). */
			if (CM_Register_Notification(&notify_filter, NULL, hid_internal_notify_callback, &hid_hotplug_context.notify_handle) != CR_SUCCESS) {
				register_global_error(L"hid_hotplug_register_callback/CM_Register_Notification");
				hid_hotplug_context.notify_handle = NULL;
				LeaveCriticalSection(&hid_hotplug_context.critical_section);
				free(hotplug_cb);
				return -1;
			}

			/* Normally empty already; an old notification may have appended entries
			   between its detachment and the completion of its unregistration */
			hid_free_enumeration(hid_hotplug_context.devs);
			hid_hotplug_context.devs = NULL;

			/* Fill already connected devices so we can use this info in disconnection
			   notifications and HID_API_HOTPLUG_ENUMERATE passes */
			hid_hotplug_context.devs = hid_internal_enumerate(0, 0, &enumerate_failure);
			if (enumerate_failure) {
				/* An empty system is fine; a failed enumeration is not: the device
				   cache and the ENUMERATE snapshot would misrepresent the system.
				   register_global_error: set above or by hid_internal_enumerate */
				HCMNOTIFICATION notify_handle = hid_internal_hotplug_cleanup();
				LeaveCriticalSection(&hid_hotplug_context.critical_section);
				hid_internal_hotplug_finish_unregistration(notify_handle);
				free(hotplug_cb);
				return -1;
			}
		}
		/* else: reuse the still-attached notification (its deferred teardown has
		   not run yet); the device cache is kept current by that notification */
	}

	/* Take the registration-time snapshot to be replayed asynchronously
	   on the event context, one exact copy per matching connected device */
	if ((flags & HID_API_HOTPLUG_ENUMERATE) && (hotplug_cb->events & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)) {
		struct hid_device_info **replay_tail = &hotplug_cb->replay;
		for (struct hid_device_info *device = hid_hotplug_context.devs; device != NULL; device = device->next) {
			if (!hid_internal_match_device_id(device->vendor_id, device->product_id, hotplug_cb->vendor_id, hotplug_cb->product_id)) {
				continue;
			}
			*replay_tail = hid_internal_copy_device_info(device);
			if (*replay_tail == NULL) {
				HCMNOTIFICATION notify_handle;
				register_global_error(L"Failed to allocate memory for a device info snapshot");
				hid_free_enumeration(hotplug_cb->replay);
				/* Tear the machinery down if this would-be-first callback was starting it */
				notify_handle = hid_internal_hotplug_cleanup();
				LeaveCriticalSection(&hid_hotplug_context.critical_section);
				hid_internal_hotplug_finish_unregistration(notify_handle);
				free(hotplug_cb);
				return -1;
			}
			replay_tail = &(*replay_tail)->next;
		}
	}

	/* Append the new callback to the end of the list */
	if (hid_hotplug_context.hotplug_cbs != NULL) {
		struct hid_hotplug_callback *last = hid_hotplug_context.hotplug_cbs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = hotplug_cb;
	}
	else {
		hid_hotplug_context.hotplug_cbs = hotplug_cb;
	}

	/* Return allocated handle */
	if (callback_handle != NULL) {
		*callback_handle = hotplug_cb->handle;
	}

	/* Have the snapshot delivered on the event context; never from within this call */
	if (hotplug_cb->replay != NULL) {
		hid_internal_SubmitThreadpoolWork(hid_hotplug_context.event_work);
	}

	/* A successful registration leaves no stale error behind (the internal
	   enumeration of an empty system registers "No HID devices found").
	   Under the critical section: outside of it this would race with - and wipe -
	   the error string of a concurrently failing registration. */
	register_global_error(NULL);

	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	return 0;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_register_callback(unsigned short vendor_id, unsigned short product_id, int events, int flags, hid_hotplug_callback_fn callback, void* user_data, hid_hotplug_callback_handle* callback_handle)
{
	struct hid_hotplug_callback* hotplug_cb;
	int result;

	/* No events can be delivered before the handle is written */
	if (callback_handle != NULL) {
		*callback_handle = 0;
	}

	/* Check params */
	if (callback == NULL) {
		register_global_error(L"Callback function is NULL");
		return -1;
	}
	if (events == 0
		|| (events & ~(HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT))) {
		register_global_error(L"Invalid events mask");
		return -1;
	}
	if (flags & ~(HID_API_HOTPLUG_ENUMERATE)) {
		register_global_error(L"Invalid flags");
		return -1;
	}

	hotplug_cb = (struct hid_hotplug_callback*)calloc(1, sizeof(struct hid_hotplug_callback));

	if (hotplug_cb == NULL) {
		register_global_error(L"Failed to allocate memory for a hotplug callback");
		return -1;
	}

	/* Fill out the record */
	hotplug_cb->next = NULL;
	hotplug_cb->vendor_id = vendor_id;
	hotplug_cb->product_id = product_id;
	hotplug_cb->events = events;
	hotplug_cb->user_data = user_data;
	hotplug_cb->callback = callback;
	hotplug_cb->replay = NULL;

	/* Ensure the machinery is ready to be used, and keep hid_exit() from
	   destroying it while this call is inside */
	switch (hid_internal_hotplug_enter(1 /* bootstrap on first use */)) {
	case 0:
		break;
	case HID_HOTPLUG_ENTER_EXITING:
		/* hid_exit() is tearing the machinery down: arming it again behind its
		   back would leave a live notification and a registered callback with no
		   context to run in */
		register_global_error(L"hid_exit() is in progress");
		free(hotplug_cb);
		return -1;
	default:
		/* register_global_error: set by hid_internal_hotplug_enter */
		free(hotplug_cb);
		return -1;
	}

	result = hid_internal_hotplug_register_counted(hotplug_cb, flags, callback_handle);

	hid_internal_hotplug_leave();

	return result;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_deregister_callback(hid_hotplug_callback_handle callback_handle)
{
	int result = -1;
	HCMNOTIFICATION notify_handle;

	/* Never bootstraps the machinery: with no machinery there is nothing this
	   handle could belong to. And when hid_exit() is in progress, it deregisters
	   every callback and invalidates every handle - this one is (or is about to
	   be) one of them. */
	if (callback_handle <= 0 || hid_internal_hotplug_enter(0) != 0) {
		register_global_error(L"Invalid or unknown hotplug callback handle");
		return -1;
	}

	/* Lock the mutex to avoid race conditions */
	EnterCriticalSection(&hid_hotplug_context.critical_section);

	if (hid_internal_hotplug_exiting()) {
		/* hid_exit() started tearing the machinery down after this call was
		   counted in: same as above */
		register_global_error(L"Invalid or unknown hotplug callback handle");
		LeaveCriticalSection(&hid_hotplug_context.critical_section);
		hid_internal_hotplug_leave();
		return -1;
	}

	/* Remove this notification */
	for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
		if ((*current)->handle == callback_handle) {
			/* A callback already marked for removal counts as deregistered */
			if (!(*current)->events) {
				break;
			}

			/* Undelivered HID_API_HOTPLUG_ENUMERATE events must never fire after deregistration */
			hid_free_enumeration((*current)->replay);
			(*current)->replay = NULL;

			/* Check if we were already in the critical section, as we are NOT allowed to remove any callbacks if we are */
			if (hid_hotplug_context.mutex_in_use) {
				/* If we are not allowed to remove the callback, we mark it as pending removal */
				(*current)->events = 0;
				hid_hotplug_context.cb_list_dirty = 1;
			} else {
				struct hid_hotplug_callback *next = (*current)->next;
				free(*current);
				*current = next;
			}
			result = 0;
			break;
		}
	}

	if (result != 0) {
		/* Under the critical section: outside of it this would race with - and wipe -
		   the error string of a concurrently failing hotplug call */
		register_global_error(L"Invalid or unknown hotplug callback handle");
	}

	notify_handle = hid_internal_hotplug_cleanup();

	LeaveCriticalSection(&hid_hotplug_context.critical_section);

	hid_internal_hotplug_finish_unregistration(notify_handle);

	hid_internal_hotplug_leave();

	return result;
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* TODO: Merge this functions with the Linux version. This function should be platform independent. */
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	/* register_global_error: global error is reset by hid_enumerate/hid_init */
	devs = hid_enumerate(vendor_id, product_id);
	if (!devs) {
		/* register_global_error: global error is already set by hid_enumerate */
		return NULL;
	}

	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (cur_dev->serial_number && wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	} else {
		register_global_error(L"Device with requested VID/PID/(SerialNumber) not found");
	}

	hid_free_enumeration(devs);

	return handle;
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
	hid_device *dev = NULL;
	wchar_t* interface_path = NULL;
	HANDLE device_handle = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	HIDP_CAPS caps;

	if (hid_init() < 0) {
		/* register_global_error: global error is reset by hid_init */
		goto end_of_function;
	}

	interface_path = hid_internal_UTF8toUTF16(path);
	if (!interface_path) {
		register_global_error(L"Path conversion failure");
		goto end_of_function;
	}

	/* Open a handle to the device */
	device_handle = open_device(interface_path, TRUE);

	/* Check validity of write_handle. */
	if (device_handle == INVALID_HANDLE_VALUE) {
		/* System devices, such as keyboards and mice, cannot be opened in
		   read-write mode, because the system takes exclusive control over
		   them.  This is to prevent keyloggers.  However, feature reports
		   can still be sent and received.  Retry opening the device, but
		   without read/write access. */
		device_handle = open_device(interface_path, FALSE);

		/* Check the validity of the limited device_handle. */
		if (device_handle == INVALID_HANDLE_VALUE) {
			register_global_winapi_error(L"open_device");
			goto end_of_function;
		}
	}

	/* Set the Input Report buffer size to 64 reports. */
	if (!HidD_SetNumInputBuffers(device_handle, 64)) {
		register_global_winapi_error(L"set input buffers");
		goto end_of_function;
	}

	/* Get the Input Report length for the device. */
	if (!HidD_GetPreparsedData(device_handle, &pp_data)) {
		register_global_winapi_error(L"get preparsed data");
		goto end_of_function;
	}

	if (HidP_GetCaps(pp_data, &caps) != HIDP_STATUS_SUCCESS) {
		register_global_error(L"HidP_GetCaps");
		goto end_of_function;
	}

	dev = new_hid_device();

	if (dev == NULL) {
		register_global_error(L"hid_device allocation error");
		goto end_of_function;
	}

	dev->device_handle = device_handle;
	device_handle = INVALID_HANDLE_VALUE;

	dev->output_report_length = caps.OutputReportByteLength;
	dev->input_report_length = caps.InputReportByteLength;
	dev->feature_report_length = caps.FeatureReportByteLength;
	dev->read_buf = (char*) malloc(dev->input_report_length);
	dev->device_info = hid_internal_get_device_info(interface_path, dev->device_handle, NULL);

end_of_function:
	free(interface_path);

	if (device_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(device_handle);
	}

	if (pp_data) {
		HidD_FreePreparsedData(pp_data);
	}

	return dev;
}

void HID_API_EXPORT_CALL hid_winapi_set_write_timeout(hid_device *dev, unsigned long timeout)
{
	dev->write_timeout_ms = timeout;
}

int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	DWORD bytes_written = 0;
	int function_result = -1;
	BOOL res;
	BOOL overlapped = FALSE;

	unsigned char *buf;

	if (!data || !length) {
		register_string_error(dev, L"Zero buffer/length");
		return function_result;
	}

	register_string_error(dev, NULL);

	/* Make sure the right number of bytes are passed to WriteFile. Windows
	   expects the number of bytes which are in the _longest_ report (plus
	   one for the report number) bytes even if the data is a report
	   which is shorter than that. Windows gives us this value in
	   caps.OutputReportByteLength. If a user passes in fewer bytes than this,
	   use cached temporary buffer which is the proper size. */
	if (length >= dev->output_report_length) {
		/* The user passed the right number of bytes. Use the buffer as-is. */
		buf = (unsigned char *) data;
	} else {
		if (dev->write_buf == NULL) {
			dev->write_buf = (unsigned char *) malloc(dev->output_report_length);

			if (dev->write_buf == NULL) {
				register_string_error(dev, L"hid_write/malloc");
				goto end_of_function;
			}
		}

		buf = dev->write_buf;
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->output_report_length - length);
		length = dev->output_report_length;
	}

	res = WriteFile(dev->device_handle, buf, (DWORD) length, &bytes_written, &dev->write_ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* WriteFile() failed. Return error. */
			register_winapi_error(dev, L"WriteFile");
			goto end_of_function;
		}
		overlapped = TRUE;
	} else {
		/* WriteFile() succeeded synchronously. */
		function_result = bytes_written;
	}

	if (overlapped) {
		/* Wait for the transaction to complete. This makes
		   hid_write() synchronous. */
		res = WaitForSingleObject(dev->write_ol.hEvent, dev->write_timeout_ms);
		if (res != WAIT_OBJECT_0) {
			/* There was a Timeout. */
			register_winapi_error(dev, L"hid_write/WaitForSingleObject");
			goto end_of_function;
		}

		/* Get the result. */
		res = GetOverlappedResult(dev->device_handle, &dev->write_ol, &bytes_written, FALSE/*wait*/);
		if (res) {
			function_result = bytes_written;
		}
		else {
			/* The Write operation failed. */
			register_winapi_error(dev, L"hid_write/GetOverlappedResult");
			goto end_of_function;
		}
	}

end_of_function:
	return function_result;
}


int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	DWORD bytes_read = 0;
	size_t copy_len = 0;
	BOOL res = FALSE;
	BOOL overlapped = FALSE;

	if (!data || !length) {
		register_string_error_to_buffer(&dev->last_read_error_str, L"Zero buffer/length");
		return -1;
	}

	register_string_error_to_buffer(&dev->last_read_error_str, NULL);

	/* Copy the handle for convenience. */
	HANDLE ev = dev->ol.hEvent;

	if (!dev->read_pending) {
		/* Start an Overlapped I/O read. */
		dev->read_pending = TRUE;
		memset(dev->read_buf, 0, dev->input_report_length);
		ResetEvent(ev);
		res = ReadFile(dev->device_handle, dev->read_buf, (DWORD) dev->input_report_length, &bytes_read, &dev->ol);

		if (!res) {
			if (GetLastError() != ERROR_IO_PENDING) {
				/* ReadFile() has failed.
				   Clean up and return error. */
				register_winapi_error_to_buffer(&dev->last_read_error_str, L"ReadFile");
				CancelIo(dev->device_handle);
				dev->read_pending = FALSE;
				goto end_of_function;
			}
			overlapped = TRUE;
		}
	}
	else {
		overlapped = TRUE;
	}

	if (overlapped) {
		/* See if there is any data yet. */
		res = WaitForSingleObject(ev, milliseconds >= 0 ? (DWORD)milliseconds : INFINITE);
		if (res != WAIT_OBJECT_0) {
			/* There was no data this time. Return zero bytes available,
			   but leave the Overlapped I/O running. */
			return 0;
		}

		/* Get the number of bytes read. The actual data has been copied to the data[]
		   array which was passed to ReadFile(). We must not wait here because we've
		   already waited on our event above, and since it's auto-reset, it will have
		   been reset back to unsignalled by now. */
		res = GetOverlappedResult(dev->device_handle, &dev->ol, &bytes_read, FALSE/*don't wait now - already did on the prev step*/);
	}
	/* Set pending back to false, even if GetOverlappedResult() returned error. */
	dev->read_pending = FALSE;

	if (res && bytes_read > 0) {
		if (dev->read_buf[0] == 0x0) {
			/* If report numbers aren't being used, but Windows sticks a report
			   number (0x0) on the beginning of the report anyway. To make this
			   work like the other platforms, and to make it work more like the
			   HID spec, we'll skip over this byte. */
			bytes_read--;
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf+1, copy_len);
		}
		else {
			/* Copy the whole buffer, report number and all. */
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf, copy_len);
		}
	}
	if (!res) {
		register_winapi_error_to_buffer(&dev->last_read_error_str, L"hid_read_timeout/GetOverlappedResult");
	}

end_of_function:
	if (!res) {
		return -1;
	}

	return (int) copy_len;
}

int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

HID_API_EXPORT const wchar_t * HID_API_CALL hid_read_error(hid_device *dev)
{
	if (dev->last_read_error_str == NULL)
		return L"Success";
	return dev->last_read_error_str;
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = !nonblock;
	return 0; /* Success */
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	BOOL res = FALSE;
	unsigned char *buf;
	size_t length_to_send;

	if (!data || !length) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	register_string_error(dev, NULL);

	/* Windows expects at least caps.FeatureReportByteLength bytes passed
	   to HidD_SetFeature(), even if the report is shorter. Any less sent and
	   the function fails with error ERROR_INVALID_PARAMETER set. Any more
	   and HidD_SetFeature() silently truncates the data sent in the report
	   to caps.FeatureReportByteLength. */
	if (length >= dev->feature_report_length) {
		buf = (unsigned char *) data;
		length_to_send = length;
	} else {
		if (dev->feature_buf == NULL) {
			dev->feature_buf = (unsigned char *) malloc(dev->feature_report_length);

			if (dev->feature_buf == NULL) {
				register_string_error(dev, L"hid_send_feature_report/malloc");
				return -1;
			}
		}

		buf = dev->feature_buf;
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->feature_report_length - length);
		length_to_send = dev->feature_report_length;
	}

	res = HidD_SetFeature(dev->device_handle, (PVOID)buf, (DWORD) length_to_send);

	if (!res) {
		register_winapi_error(dev, L"HidD_SetFeature");
		return -1;
	}

	return (int) length;
}

static int hid_get_report(hid_device *dev, DWORD report_type, unsigned char *data, size_t length)
{
	BOOL res;
	DWORD bytes_returned = 0;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	if (!data || !length) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	register_string_error(dev, NULL);

	res = DeviceIoControl(dev->device_handle,
		report_type,
		data, (DWORD) length,
		data, (DWORD) length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl() failed. Return error. */
			register_winapi_error(dev, L"Get Input/Feature Report DeviceIoControl");
			return -1;
		}
	}

	/* Wait here until the write is done. This makes
	   hid_get_feature_report() synchronous. */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* The operation failed. */
		register_winapi_error(dev, L"Get Input/Feature Report GetOverLappedResult");
		return -1;
	}

	/* When numbered reports aren't used,
	   bytes_returned seem to include only what is actually received from the device
	   (not including the first byte with 0, as an indication "no numbered reports"). */
	if (data[0] == 0x0) {
		bytes_returned++;
	}

	return bytes_returned;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetFeature() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_FEATURE, data, length);
}

int HID_API_EXPORT HID_API_CALL hid_send_output_report(hid_device* dev, const unsigned char* data, size_t length)
{
	BOOL res = FALSE;
	unsigned char *buf;
	size_t length_to_send;

	if (!data || !length) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	register_string_error(dev, NULL);

	/* Windows expects at least caps.OutputeportByteLength bytes passed
	   to HidD_SetOutputReport(), even if the report is shorter. Any less sent and
	   the function fails with error ERROR_INVALID_PARAMETER set. Any more 
	   and HidD_SetOutputReport() silently truncates the data sent in the report
	   to caps.OutputReportByteLength. */
	if (length >= dev->output_report_length) {
		buf = (unsigned char *) data;
		length_to_send = length;
	} else {
		if (dev->write_buf == NULL) {
			dev->write_buf = (unsigned char *) malloc(dev->output_report_length);

			if (dev->write_buf == NULL) {
				register_string_error(dev, L"hid_send_output_report/malloc");
				return -1;
			}
		}

		buf = dev->write_buf;
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->output_report_length - length);
		length_to_send = dev->output_report_length;
	}

	res = HidD_SetOutputReport(dev->device_handle, (PVOID)buf, (DWORD) length_to_send);
	if (!res) {
		register_string_error(dev, L"HidD_SetOutputReport");
		return -1;
	}

	return (int) length;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetInputReport() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_INPUT_REPORT, data, length);
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
	if (!dev)
		return;

	CancelIo(dev->device_handle);
	free_hid_device(dev);
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	if (!dev->device_info) {
		register_string_error(dev, L"NULL device info");
		return -1;
	}

	HIDAPI_WCSNCPY(string, maxlen, dev->device_info->manufacturer_string);
	string[maxlen - 1] = L'\0';

	register_string_error(dev, NULL);

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	if (!dev->device_info) {
		register_string_error(dev, L"NULL device info");
		return -1;
	}

	HIDAPI_WCSNCPY(string, maxlen, dev->device_info->product_string);
	string[maxlen - 1] = L'\0';

	register_string_error(dev, NULL);

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_string_error(dev, L"Zero buffer/length");
		return -1;
	}

	if (!dev->device_info) {
		register_string_error(dev, L"NULL device info");
		return -1;
	}

	HIDAPI_WCSNCPY(string, maxlen, dev->device_info->serial_number);
	string[maxlen - 1] = L'\0';

	register_string_error(dev, NULL);

	return 0;
}

HID_API_EXPORT struct hid_device_info * HID_API_CALL hid_get_device_info(hid_device *dev)
{
	if (!dev->device_info)
	{
		register_string_error(dev, L"NULL device info");
		return NULL;
	}

	register_string_error(dev, NULL);

	return dev->device_info;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	BOOL res;

	if (dev->device_info && dev->device_info->bus_type == HID_API_BUS_USB && maxlen > MAX_STRING_WCHARS_USB) {
		string[MAX_STRING_WCHARS_USB] = L'\0';
		maxlen = MAX_STRING_WCHARS_USB;
	}

	res = HidD_GetIndexedString(dev->device_handle, string_index, string, (ULONG)maxlen * sizeof(wchar_t));
	if (!res) {
		register_winapi_error(dev, L"HidD_GetIndexedString");
		return -1;
	}

	register_string_error(dev, NULL);

	return 0;
}

int HID_API_EXPORT_CALL hid_winapi_get_container_id(hid_device *dev, GUID *container_id)
{
	wchar_t *interface_path = NULL, *device_id = NULL;
	CONFIGRET cr = CR_FAILURE;
	DEVINST dev_node;
	DEVPROPTYPE property_type;
	ULONG len;

	if (!container_id) {
		register_string_error(dev, L"Invalid Container ID");
		return -1;
	}

	if (!dev->device_info) {
		register_string_error(dev, L"NULL device info");
		return -1;
	}

	register_string_error(dev, NULL);

	interface_path = hid_internal_UTF8toUTF16(dev->device_info->path);
	if (!interface_path) {
		register_string_error(dev, L"Path conversion failure");
		goto end;
	}

	/* Get the device id from interface path */
	device_id = (wchar_t *)hid_internal_get_device_interface_property(interface_path, &DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING);
	if (!device_id) {
		register_string_error(dev, L"Failed to get device interface property InstanceId");
		goto end;
	}

	/* Open devnode from device id */
	cr = CM_Locate_DevNodeW(&dev_node, (DEVINSTID_W)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS) {
		register_string_error(dev, L"Failed to locate device node");
		goto end;
	}

	/* Get the container id from devnode */
	len = sizeof(*container_id);
	cr = CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_Device_ContainerId, &property_type, (PBYTE)container_id, &len, 0);
	if (cr == CR_SUCCESS && property_type != DEVPROP_TYPE_GUID)
		cr = CR_FAILURE;

	if (cr != CR_SUCCESS)
		register_string_error(dev, L"Failed to read ContainerId property from device node");

end:
	free(interface_path);
	free(device_id);

	return cr == CR_SUCCESS ? 0 : -1;
}


int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device *dev, unsigned char *buf, size_t buf_size)
{
	PHIDP_PREPARSED_DATA pp_data = NULL;

	if (!HidD_GetPreparsedData(dev->device_handle, &pp_data) || pp_data == NULL) {
		register_string_error(dev, L"HidD_GetPreparsedData");
		return -1;
	}


	int res = hid_winapi_descriptor_reconstruct_pp_data(pp_data, buf, buf_size);

	HidD_FreePreparsedData(pp_data);

	if (res == 0) {
		register_string_error(dev, NULL);
	}
	else {
		register_string_error(dev, L"Failed to reconstruct descriptor from PREPARSED_DATA");
	}

	return res;
}

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->last_error_str == NULL)
			return L"Success";
		return (wchar_t*)dev->last_error_str;
	}

	if (last_global_error_str == NULL)
		return L"Success";
	return last_global_error_str;
}

#ifndef HIDAPI_CMAKE_V0_BUILD
/* Include the descriptor reconstruction code when NOT building with CMake.
   CMake builds compile hidapi_descriptor_reconstruct.c as a separate translation unit. */
#include "hidapi_descriptor_reconstruct.c"
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
