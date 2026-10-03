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

#include <windows.h>
#include <stddef.h>

#if defined(HIDAPI_NO_CRT)

#if (defined(_M_IX86) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__clang__))
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(memset, memcpy, memcmp)
#endif

void *__cdecl memset(void *dest, int value, size_t count)
{
#if (defined(_M_IX86) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__clang__))
	__stosb((unsigned char *)dest, (unsigned char)value, count);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	while (count--)
		*d++ = (unsigned char)value;
#endif
	return dest;
}

void *__cdecl memcpy(void *dest, const void *src, size_t count)
{
#if (defined(_M_IX86) || defined(_M_X64)) && (defined(_MSC_VER) || defined(__clang__))
	__movsb((unsigned char *)dest, (const unsigned char *)src, count);
#else
	volatile unsigned char *d = (volatile unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;
	while (count--)
		*d++ = *s++;
#endif
	return dest;
}

int __cdecl memcmp(const void *lhs, const void *rhs, size_t count)
{
	const volatile unsigned char *a = (const volatile unsigned char *)lhs;
	const volatile unsigned char *b = (const volatile unsigned char *)rhs;
	while (count--) {
		if (*a != *b)
			return *a < *b ? -1 : 1;
		++a;
		++b;
	}
	return 0;
}

#endif /* HIDAPI_NO_CRT */
