#include <hidapi.h>

int main(void)
{
	return hid_version() ? 0 : 1;
}
