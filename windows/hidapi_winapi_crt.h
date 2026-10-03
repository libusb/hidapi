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

#ifndef HIDAPI_WINAPI_CRT_H__
#define HIDAPI_WINAPI_CRT_H__

#if defined(HIDAPI_NO_CRT)

#include <windows.h>
#include <stdint.h>

#if (defined(_M_IX86) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__clang__))
#include <intrin.h>
#define HIDAPI_WINAPI_HAVE_MOVSB
#endif

static void *hid_internal_alloc(size_t bytes)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1);
}

static void hid_internal_free(void *ptr)
{
	if (ptr)
		HeapFree(GetProcessHeap(), 0, ptr);
}

static void *hid_internal_calloc(size_t count, size_t size)
{
	if (count != 0 && size > SIZE_MAX / count)
		return NULL;
	return hid_internal_alloc(count * size);
}

static void *hid_internal_copy(void *dest, const void *src, size_t bytes)
{
#ifdef HIDAPI_WINAPI_HAVE_MOVSB
	__movsb((unsigned char *)dest, (const unsigned char *)src, bytes);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	while (bytes--)
		*d++ = *s++;
#endif
	return dest;
}

static void *hid_internal_fill(void *dest, int value, size_t bytes)
{
#ifdef HIDAPI_WINAPI_HAVE_MOVSB
	__stosb((unsigned char *)dest, (unsigned char)value, bytes);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	while (bytes--)
		*d++ = (unsigned char)value;
#endif
	return dest;
}

static int hid_internal_compare(const void *lhs, const void *rhs, size_t bytes)
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

static void *hid_internal_dup(const void *src, size_t bytes)
{
	void *dest = hid_internal_alloc(bytes);
	if (dest)
		hid_internal_copy(dest, src, bytes);
	return dest;
}

static char *hid_internal_strdup(const char *src)
{
	return (char *)hid_internal_dup(src, (size_t)lstrlenA(src) + 1);
}

static wchar_t *hid_internal_wcsdup(const wchar_t *src)
{
	return (wchar_t *)hid_internal_dup(src, ((size_t)lstrlenW(src) + 1) * sizeof(wchar_t));
}

static int hid_internal_wcscmp(const wchar_t *lhs, const wchar_t *rhs)
{
	while (*lhs && *lhs == *rhs) {
		++lhs;
		++rhs;
	}
	return (int)(unsigned int)*lhs - (int)(unsigned int)*rhs;
}

static wchar_t *hid_internal_wcsstr(const wchar_t *haystack, const wchar_t *needle)
{
	if (!*needle)
		return (wchar_t *)haystack;
	for (; *haystack; ++haystack) {
		const wchar_t *h = haystack;
		const wchar_t *n = needle;
		while (*h && *h == *n) {
			++h;
			++n;
		}
		if (!*n)
			return (wchar_t *)haystack;
	}
	return NULL;
}

static inline int hid_internal_format_error(wchar_t *buffer, size_t count, const wchar_t *op,
	size_t op_len, DWORD error_code, const wchar_t *system_error, size_t system_error_len)
{
	static const wchar_t prefix[] = L": (0x";
	static const wchar_t suffix[] = L") ";
	static const wchar_t hex[] = L"0123456789ABCDEF";
	size_t pos = 0;
	size_t i;
	int shift;

	if (count == 0)
		return -1;

	for (i = 0; i < op_len && pos + 1 < count; ++i)
		buffer[pos++] = op[i];
	for (i = 0; i < sizeof(prefix) / sizeof(prefix[0]) - 1 && pos + 1 < count; ++i)
		buffer[pos++] = prefix[i];
	for (shift = 28; shift >= 0; shift -= 4) {
		if (pos + 1 >= count)
			break;
		buffer[pos++] = hex[(error_code >> shift) & 0x0F];
	}
	for (i = 0; i < sizeof(suffix) / sizeof(suffix[0]) - 1 && pos + 1 < count; ++i)
		buffer[pos++] = suffix[i];
	for (i = 0; i < system_error_len && pos + 1 < count; ++i)
		buffer[pos++] = system_error[i];

	buffer[pos] = L'\0';
	return (int)pos;
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

#undef malloc
#undef calloc
#undef free
#undef memcpy
#undef memset
#undef memcmp
#undef wcslen
#undef wcscmp
#undef wcsstr
#undef _wcsdup
#undef _strdup

#define malloc(n) hid_internal_alloc(n)
#define calloc(n, size) hid_internal_calloc((n), (size))
#define free(p) hid_internal_free(p)
#define memcpy(dest, src, bytes) hid_internal_copy((dest), (src), (bytes))
#define memset(dest, value, bytes) hid_internal_fill((dest), (value), (bytes))
#define memcmp(lhs, rhs, bytes) hid_internal_compare((lhs), (rhs), (bytes))
#define wcslen(s) ((size_t)lstrlenW(s))
#define wcscmp(lhs, rhs) hid_internal_wcscmp((lhs), (rhs))
#define wcsstr(haystack, needle) hid_internal_wcsstr((haystack), (needle))
#define _wcsdup(s) hid_internal_wcsdup(s)
#define _strdup(s) hid_internal_strdup(s)

#endif /* HIDAPI_NO_CRT */

#endif /* HIDAPI_WINAPI_CRT_H__ */
