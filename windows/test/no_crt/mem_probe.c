/* Exercise the HIDAPI memory helpers in an amalgamated no-CRT build. */
#include <windows.h>
#include <hidapi.h>

#include "../../hid.c"

void __cdecl mainCRTStartup(void)
{
	volatile unsigned char src[64];
	volatile unsigned char dst[64];
	unsigned int salt = GetTickCount();

	for (unsigned int i = 0; i < sizeof(src); i++)
		src[i] = (unsigned char)(salt + i * 31u);

	hid_internal_memset((void *)dst, 0xA5, sizeof(dst));
	if (dst[0] != 0xA5 || dst[sizeof(dst) - 1] != 0xA5)
		ExitProcess(2);

	hid_internal_memcpy((void *)dst, (const void *)src, sizeof(dst));
	if (hid_internal_memcmp((const void *)dst, (const void *)src, sizeof(dst)) != 0)
		ExitProcess(3);

	src[0] ^= 0xFF;
	if (hid_internal_memcmp((const void *)dst, (const void *)src, sizeof(dst)) == 0)
		ExitProcess(4);

	if (hid_init() == 0)
		hid_exit();
	ExitProcess(0);
}
