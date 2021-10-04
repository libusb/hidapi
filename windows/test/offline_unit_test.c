#include "../windows/hid.c"
#include <hidapi.h>

read_preparsed_data_from_file(char* filename, struct hid_device_info* dev_info, PHIDP_PREPARSED_DATA pp_data) {


}


main(int argc, char* argv[])
{

	int res;
	unsigned char buf[256];
#define MAX_STR 255
	wchar_t wstr[MAX_STR];
	hid_device* handle;
	int i;


	struct hid_device_info* devs, * cur_dev;

	printf("hidapi test/example tool. Compiled with hidapi version %s, runtime version %s.\n", HID_API_VERSION_STR, hid_version_str());
	if (hid_version()->major == HID_API_VERSION_MAJOR && hid_version()->minor == HID_API_VERSION_MINOR && hid_version()->patch == HID_API_VERSION_PATCH) {
		printf("Compile-time version matches runtime version of hidapi.\n\n");
	}
	else {
		printf("Compile-time version is different than runtime version of hidapi.\n]n");
	}

	if (hid_init())
		return -1;

	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);
		printf("\n");
		printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
		printf("  Product:      %ls\n", cur_dev->product_string);
		printf("  Release:      %hx\n", cur_dev->release_number);
		printf("  Interface:    %d\n", cur_dev->interface_number);
		printf("  Usage (page): 0x%hx (0x%hx)\n", cur_dev->usage, cur_dev->usage_page);

		printf("\n");
		cur_dev = cur_dev->next;
	}
		//hid_get_report_descriptor(hid_device * dev, unsigned char* buf, size_t buf_size)
	printf("%s\n", argv[1]);


	HIDP_PREPARSED_DATA pp_data;
	struct hid_device_info dev_info;
	read_preparsed_data_from_file(argv[1], &dev_info, &pp_data);

	return 1;
}