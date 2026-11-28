/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2026, All Rights Reserved.

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

#ifndef HIDAPI_WINAPI_CRT_COMPAT_H__
#define HIDAPI_WINAPI_CRT_COMPAT_H__

#ifndef HIDAPI_NO_CRT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef __CYGWIN__
#include <wctype.h>
#endif

/* MSVC secure CRT (VS2005+) provides swprintf_s/wcsncpy_s.
   Older MSVC and GCC/MinGW/Cygwin use the classic variants. */
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#define HIDAPI_SWPRINTF swprintf_s
#define HIDAPI_WCSNCPY(dest, dest_count, src) wcsncpy_s((dest), (dest_count), (src), _TRUNCATE)
#else
#define HIDAPI_SWPRINTF swprintf
#define HIDAPI_WCSNCPY(dest, dest_count, src) wcsncpy((dest), (src), (dest_count))
#endif

static inline void hid_internal_towupper(wchar_t *string)
{
	for (wchar_t *p = string; *p; ++p)
		*p = (wchar_t)towupper(*p);
}

#define HIDAPI_MALLOC(n) malloc(n)
#define HIDAPI_CALLOC(n, size) calloc((n), (size))
#define HIDAPI_FREE(p) free(p)
#define HIDAPI_MEMCPY(dest, src, bytes) memcpy((dest), (src), (bytes))
#define HIDAPI_MEMSET(dest, value, bytes) memset((dest), (value), (bytes))
#define HIDAPI_MEMCMP(lhs, rhs, bytes) memcmp((lhs), (rhs), (bytes))
#define HIDAPI_WCSLEN(s) wcslen(s)
#define HIDAPI_WCSCMP(lhs, rhs) wcscmp((lhs), (rhs))
#define HIDAPI_WCSSTR(haystack, needle) wcsstr((haystack), (needle))
#define HIDAPI_WCSTOL_HEX(s, end) wcstol((s), (end), 16)
#ifdef __CYGWIN__
#define HIDAPI_WCSDUP(s) wcsdup(s)
#else
#define HIDAPI_WCSDUP(s) _wcsdup(s)
#endif
#define HIDAPI_STRDUP(s) _strdup(s)

#else /* HIDAPI_NO_CRT */

#include <stdint.h>
#include <windows.h>
#include <shlwapi.h>

static inline void hid_internal_towupper(wchar_t *string)
{
	CharUpperW(string);
}

static inline void *hid_internal_malloc(size_t bytes)
{
	return HeapAlloc(GetProcessHeap(), 0, bytes ? bytes : 1);
}

static inline void *hid_internal_calloc(size_t count, size_t size)
{
	size_t bytes;

	if (count != 0 && size > SIZE_MAX / count)
		return NULL;

	bytes = count * size;
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1);
}

static inline void hid_internal_free(void *ptr)
{
	if (ptr)
		HeapFree(GetProcessHeap(), 0, ptr);
}

#if (defined(_M_IX86) || defined(_M_X64)) && defined(_MSC_VER)
#include <intrin.h>
#define HIDAPI_WINAPI_HAVE_MSVC_MEM_INTRIN
#endif

static inline void *hid_internal_memcpy(void *dest, const void *src, size_t bytes)
{
#ifdef HIDAPI_WINAPI_HAVE_MSVC_MEM_INTRIN
	__movsb((unsigned char *)dest, (const unsigned char *)src, bytes);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	while (bytes--)
		*d++ = *s++;
#endif
	return dest;
}

static inline void *hid_internal_memset(void *dest, int value, size_t bytes)
{
/* clang-cl lowers __stosb to external memset, which no-CRT builds cannot use. */
#if defined(HIDAPI_WINAPI_HAVE_MSVC_MEM_INTRIN) && !defined(__clang__)
	__stosb((unsigned char *)dest, (unsigned char)value, bytes);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	while (bytes--)
		*d++ = (unsigned char)value;
#endif
	return dest;
}

#undef HIDAPI_WINAPI_HAVE_MSVC_MEM_INTRIN

static inline int hid_internal_memcmp(const void *lhs, const void *rhs, size_t bytes)
{
	const volatile unsigned char *a = (const volatile unsigned char *)lhs;
	const volatile unsigned char *b = (const volatile unsigned char *)rhs;
	while (bytes--) {
		if (*a != *b)
			return *a < *b ? -1 : 1;
		++a;
		++b;
	}
	return 0;
}

static inline void *hid_internal_dup(const void *src, size_t bytes)
{
	void *dest = hid_internal_malloc(bytes);
	if (dest)
		hid_internal_memcpy(dest, src, bytes);
	return dest;
}

static inline char *hid_internal_strdup(const char *src)
{
	return (char *)hid_internal_dup(src, (size_t)lstrlenA(src) + 1);
}

static inline wchar_t *hid_internal_wcsdup(const wchar_t *src)
{
	return (wchar_t *)hid_internal_dup(src, ((size_t)lstrlenW(src) + 1) * sizeof(wchar_t));
}

static inline int hid_internal_wcscmp(const wchar_t *lhs, const wchar_t *rhs)
{
	int result = CompareStringOrdinal(lhs, -1, rhs, -1, FALSE);
	if (result == CSTR_EQUAL)
		return 0;
	return result == CSTR_LESS_THAN ? -1 : 1;
}

static inline long hid_internal_wcstol_hex(const wchar_t *string, wchar_t **endptr)
{
	long value = 0;
	const wchar_t *p = string;

	for (;; ++p) {
		wchar_t c = *p;
		int digit;
		if (c >= L'0' && c <= L'9') digit = c - L'0';
		else if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
		else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
		else break;
		value = (value << 4) | digit;
	}
	*endptr = (wchar_t *)p;
	return value;
}

#define HIDAPI_MALLOC(n) hid_internal_malloc(n)
#define HIDAPI_CALLOC(n, size) hid_internal_calloc((n), (size))
#define HIDAPI_FREE(p) hid_internal_free(p)
#define HIDAPI_MEMCPY(dest, src, bytes) hid_internal_memcpy((dest), (src), (bytes))
#define HIDAPI_MEMSET(dest, value, bytes) hid_internal_memset((dest), (value), (bytes))
#define HIDAPI_MEMCMP(lhs, rhs, bytes) hid_internal_memcmp((lhs), (rhs), (bytes))
#define HIDAPI_WCSLEN(s) ((size_t)lstrlenW(s))
#define HIDAPI_WCSCMP(lhs, rhs) hid_internal_wcscmp((lhs), (rhs))
#define HIDAPI_WCSSTR(haystack, needle) StrStrW((haystack), (needle))
#define HIDAPI_WCSTOL_HEX(s, end) hid_internal_wcstol_hex((s), (end))
#define HIDAPI_WCSDUP(s) hid_internal_wcsdup(s)
#define HIDAPI_STRDUP(s) hid_internal_strdup(s)
#define HIDAPI_SWPRINTF(dest, count, ...) wnsprintfW((dest), (int)(count), __VA_ARGS__)
#define HIDAPI_WCSNCPY(dest, dest_count, src) lstrcpynW((dest), (src), (int)(dest_count))


#endif /* HIDAPI_NO_CRT */

#endif /* HIDAPI_WINAPI_CRT_COMPAT_H__ */
