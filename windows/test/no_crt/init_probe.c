/* Minimal executable entry point for the no-CRT link/runtime check. */
#include <windows.h>
#include <hidapi.h>

void __cdecl mainCRTStartup(void)
{
	if (hid_init() == 0)
		hid_exit();
	ExitProcess(0);
}
