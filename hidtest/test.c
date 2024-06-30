/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022.

 This contents of this file may be used by anyone
 for any reason without any conditions and may be
 used as a starting point for your own applications
 which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h> // for "tolower()"

#include <hidapi.h>

// Headers needed for sleeping and console management (wait for a keypress)
#ifdef _WIN32
	#include <windows.h>
	#include <conio.h>
#else
	#include <fcntl.h>
	#include <termios.h>
	#include <unistd.h>
#endif

// Fallback/example
#ifndef HID_API_MAKE_VERSION
#define HID_API_MAKE_VERSION(mj, mn, p) (((mj) << 24) | ((mn) << 8) | (p))
#endif
#ifndef HID_API_VERSION
#define HID_API_VERSION HID_API_MAKE_VERSION(HID_API_VERSION_MAJOR, HID_API_VERSION_MINOR, HID_API_VERSION_PATCH)
#endif

//
// Sample using platform-specific headers
#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_darwin.h>
#endif

#if defined(_WIN32) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_winapi.h>
#endif

#if defined(USING_HIDAPI_LIBUSB) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_libusb.h>
#endif

//
// A function that waits for a key to be pressed and reports it's code
// Used for immediate response in interactive subroutines
// Taken from:
// https://cboard.cprogramming.com/c-programming/63166-kbhit-linux.html
int waitkey()
{
#ifdef _WIN32
	return _getch();
#else
	struct termios oldt, newt;
	int ch;
	int oldf;

	tcgetattr(STDIN_FILENO, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

	do
	{
		usleep(1);
		ch = getchar();
	}
	while (EOF == ch);

	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	fcntl(STDIN_FILENO, F_SETFL, oldf);

	return ch;
#endif
}

//

//
// Report Device info
const char *hid_bus_name(hid_bus_type bus_type) {
	static const char *const HidBusTypeName[] = {
		"Unknown",
		"USB",
		"Bluetooth",
		"I2C",
		"SPI",
	};

	if ((int)bus_type < 0)
		bus_type = HID_API_BUS_UNKNOWN;
	if ((int)bus_type >= (int)(sizeof(HidBusTypeName) / sizeof(HidBusTypeName[0])))
		bus_type = HID_API_BUS_UNKNOWN;

	return HidBusTypeName[bus_type];
}

void print_device(struct hid_device_info *cur_dev) {
	printf("Device Found\n  type: %04hx %04hx\n  path: %s\n  serial_number: %ls", cur_dev->vendor_id, cur_dev->product_id, cur_dev->path, cur_dev->serial_number);
	printf("\n");
	printf("  Manufacturer: %ls\n", cur_dev->manufacturer_string);
	printf("  Product:      %ls\n", cur_dev->product_string);
	printf("  Release:      %hx\n", cur_dev->release_number);
	printf("  Interface:    %d\n",  cur_dev->interface_number);
	printf("  Usage (page): 0x%hx (0x%hx)\n", cur_dev->usage, cur_dev->usage_page);
	printf("  Bus type: %u (%s)\n", (unsigned)cur_dev->bus_type, hid_bus_name(cur_dev->bus_type));
	printf("\n");
}

void print_hid_report_descriptor_from_device(hid_device *device) {
	unsigned char descriptor[HID_API_MAX_REPORT_DESCRIPTOR_SIZE];
	int res = 0;

	printf("  Report Descriptor: ");
	res = hid_get_report_descriptor(device, descriptor, sizeof(descriptor));
	if (res < 0) {
		printf("error getting: %ls", hid_error(device));
	}
	else {
		printf("(%d bytes)", res);
	}
	for (int i = 0; i < res; i++) {
		if (i % 10 == 0) {
			printf("\n");
		}
		printf("0x%02x, ", descriptor[i]);
	}
	printf("\n");
}

void print_hid_report_descriptor_from_path(const char *path) {
	hid_device *device = hid_open_path(path);
	if (device) {
		print_hid_report_descriptor_from_device(device);
		hid_close(device);
	}
	else {
		printf("  Report Descriptor: Unable to open device by path\n");
	}
}

void print_devices(struct hid_device_info *cur_dev) {
	for (; cur_dev; cur_dev = cur_dev->next) {
		print_device(cur_dev);
	}
}

void print_devices_with_descriptor(struct hid_device_info *cur_dev) {
	for (; cur_dev; cur_dev = cur_dev->next) {
		print_device(cur_dev);
		print_hid_report_descriptor_from_path(cur_dev->path);
	}
}

//
// Default static testing
void test_static()
{
	struct hid_device_info *devs;

	devs = hid_enumerate(0x0, 0x0);
	print_devices_with_descriptor(devs);
	hid_free_enumeration(devs);
}


//
// Fixed device testing
void test_device()
{
	int res;
	unsigned char buf[256];
#define MAX_STR 255
	wchar_t wstr[MAX_STR];
	hid_device *handle;
	int i;

	// Set up the command buffer.
	memset(buf,0x00,sizeof(buf));
	buf[0] = 0x01;
	buf[1] = 0x81;


	// Open the device using the VID, PID,
	// and optionally the Serial number.
	////handle = hid_open(0x4d8, 0x3f, L"12345");
	handle = hid_open(0x4d8, 0x3f, NULL);
	if (!handle) {
		printf("unable to open device\n");
		return;
	}

	// Read the Manufacturer String
	wstr[0] = 0x0000;
	res = hid_get_manufacturer_string(handle, wstr, MAX_STR);
	if (res < 0)
		printf("Unable to read manufacturer string\n");
	printf("Manufacturer String: %ls\n", wstr);

	// Read the Product String
	wstr[0] = 0x0000;
	res = hid_get_product_string(handle, wstr, MAX_STR);
	if (res < 0)
		printf("Unable to read product string\n");
	printf("Product String: %ls\n", wstr);

	// Read the Serial Number String
	wstr[0] = 0x0000;
	res = hid_get_serial_number_string(handle, wstr, MAX_STR);
	if (res < 0)
		printf("Unable to read serial number string\n");
	printf("Serial Number String: (%d) %ls\n", wstr[0], wstr);

	print_hid_report_descriptor_from_device(handle);

	struct hid_device_info* info = hid_get_device_info(handle);
	if (info == NULL) {
		printf("Unable to get device info\n");
	} else {
		print_devices(info);
	}

	// Read Indexed String 1
	wstr[0] = 0x0000;
	res = hid_get_indexed_string(handle, 1, wstr, MAX_STR);
	if (res < 0)
		printf("Unable to read indexed string 1\n");
	printf("Indexed String 1: %ls\n", wstr);

	// Set the hid_read() function to be non-blocking.
	hid_set_nonblocking(handle, 1);

	// Try to read from the device. There should be no
	// data here, but execution should not block.
	res = hid_read(handle, buf, 17);

	// Send a Feature Report to the device
	buf[0] = 0x2;
	buf[1] = 0xa0;
	buf[2] = 0x0a;
	buf[3] = 0x00;
	buf[4] = 0x00;
	res = hid_send_feature_report(handle, buf, 17);
	if (res < 0) {
		printf("Unable to send a feature report.\n");
	}

	memset(buf,0,sizeof(buf));

	// Read a Feature Report from the device
	buf[0] = 0x2;
	res = hid_get_feature_report(handle, buf, sizeof(buf));
	if (res < 0) {
		printf("Unable to get a feature report: %ls\n", hid_error(handle));
	}
	else {
		// Print out the returned buffer.
		printf("Feature Report\n   ");
		for (i = 0; i < res; i++)
			printf("%02x ", (unsigned int) buf[i]);
		printf("\n");
	}

	memset(buf,0,sizeof(buf));

	// Toggle LED (cmd 0x80). The first byte is the report number (0x1).
	buf[0] = 0x1;
	buf[1] = 0x80;
	res = hid_write(handle, buf, 17);
	if (res < 0) {
		printf("Unable to write(): %ls\n", hid_error(handle));
	}


	// Request state (cmd 0x81). The first byte is the report number (0x1).
	buf[0] = 0x1;
	buf[1] = 0x81;
	hid_write(handle, buf, 17);
	if (res < 0) {
		printf("Unable to write()/2: %ls\n", hid_error(handle));
	}

	// Read requested state. hid_read() has been set to be
	// non-blocking by the call to hid_set_nonblocking() above.
	// This loop demonstrates the non-blocking nature of hid_read().
	res = 0;
	i = 0;
	while (res == 0) {
		res = hid_read(handle, buf, sizeof(buf));
		if (res == 0) {
			printf("waiting...\n");
		}
		if (res < 0) {
			printf("Unable to read(): %ls\n", hid_error(handle));
			break;
		}

		i++;
		if (i >= 10) { /* 10 tries by 500 ms - 5 seconds of waiting*/
			printf("read() timeout\n");
			break;
		}

#ifdef _WIN32
		Sleep(500);
#else
		usleep(500*1000);
#endif
	}

	if (res > 0) {
		printf("Data read:\n   ");
		// Print out the returned buffer.
		for (i = 0; i < res; i++)
			printf("%02x ", (unsigned int) buf[i]);
		printf("\n");
	}

	hid_close(handle);
}

//
// Normal hotplug testing
int device_callback(
	hid_hotplug_callback_handle callback_handle,
	struct hid_device_info* device,
	hid_hotplug_event event,
	void* user_data)
{
	(void)user_data;

	if (event & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)
		printf("Handle %d: New device is connected: %s.\n", callback_handle, device->path);
	else
		printf("Handle %d: Device was disconnected: %s.\n", callback_handle, device->path);

	printf("type: %04hx %04hx\n  serial_number: %ls", device->vendor_id, device->product_id, device->serial_number);
	printf("\n");
	printf("  Manufacturer: %ls\n", device->manufacturer_string);
	printf("  Product:      %ls\n", device->product_string);
	printf("  Release:      %hx\n", device->release_number);
	printf("  Interface:    %d\n", device->interface_number);
	printf("  Usage (page): 0x%hx (0x%hx)\n", device->usage, device->usage_page);
	printf("(Press Q to exit the test)\n");
	printf("\n");

	/* Printed data might not show on the screen - force it out */
	fflush(stdout);

	return 0;
}


void test_hotplug()
{
	printf("Starting the Hotplug test\n");
	printf("(Press Q to exit the test)\n");

	hid_hotplug_callback_handle token1, token2;

	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, device_callback, NULL, &token1);
	hid_hotplug_register_callback(0x054c, 0x0ce6, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, device_callback, NULL, &token2);

	while (1)
	{
		int command = tolower(waitkey());
		if ('q' == command)
		{
			break;
		}
	}

	hid_hotplug_deregister_callback(token2);
	hid_hotplug_deregister_callback(token1);

	printf("\n\nHotplug test stopped\n");
}

//
// Stress-testing weird edge cases in hotplugs
int cb1_handle;
int cb2_handle;
int cb_test1_triggered;

int cb2_func(hid_hotplug_callback_handle callback_handle,
             struct hid_device_info *device,
             hid_hotplug_event event,
             void *user_data)
{
	(void) callback_handle;
	(void) device;
	(void) event;
	(void) user_data;
	// TIP: only perform the test once
	if(cb_test1_triggered)
	{
		return 1;
	}

	printf("Callback 2 fired\n");

	// Deregister the first callback
	// It should be placed in the list at an index prior to the current one, which will make the pointer to the current one invalid on some implementations
	hid_hotplug_deregister_callback(cb1_handle);

	cb_test1_triggered = 1;

	// As long as we are inside this callback, nothing goes wrong; however, returning from here will cause a use-after-free error on flawed implementations
	// as to retrieve the next element (or to check for it's presence) it will look those dereference a pointer located in an already freed area
	// Undefined behavior
	return 1;
}

int cb1_func(hid_hotplug_callback_handle callback_handle,
             struct hid_device_info *device,
             hid_hotplug_event event,
             void *user_data)
{
	(void) callback_handle;
	(void) device;
	(void) event;
	(void) user_data;

	// TIP: only perform the test once
	if(cb_test1_triggered)
	{
		return 1;
	}

	printf("Callback 1 fired\n");

	// Register the second callback and make it be called immediately by enumeration attempt
	// Will cause a deadlock on Linux immediately
	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, cb2_func, NULL, &cb2_handle);
	return 1;
}

void test_hotplug_deadlocks()
{
	cb_test1_triggered = 0;
	printf("Starting the Hotplug callbacks deadlocks test\n");
	printf("TIP: if you don't see a message that it succeeded, it means the test failed and the system is now deadlocked\n");
	// Register the first callback and make it be called immediately by enumeration attempt (if at least 1 device is present)
	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, cb1_func, NULL, &cb1_handle);

	printf("Test finished successfully (at least no deadlocks were found)\n");
}


//
// CLI

void print_version_check()
{
	printf("hidapi test/example tool. Compiled with hidapi version %s, runtime version %s.\n", HID_API_VERSION_STR, hid_version_str());
	if (HID_API_VERSION == HID_API_MAKE_VERSION(hid_version()->major, hid_version()->minor, hid_version()->patch)) {
		printf("Compile-time version matches runtime version of hidapi.\n\n");
	}
	else {
		printf("Compile-time version is different than runtime version of hidapi.\n]n");
	}
}

void interactive_loop()
{
	int command = 0;

	print_version_check();

	do {
		printf("Interactive HIDAPI testing utility\n");
		printf("    1: List connected devices\n");
		printf("    2: Dynamic hotplug test\n");
		printf("    3: Test specific device [04d8:003f]\n");
		printf("    4: Test hotplug callback management deadlocking scenario\n");
		printf("    q: Quit\n");
		printf("Please enter command:");

		command = tolower(waitkey());

		printf("\n\n========================================\n\n");

	// GET COMMAND
		switch (command) {
		case '1':
			test_static();
			break;
		case '2':
			test_hotplug();
			break;
		case '3':
			test_device();
			break;
		case '4':
			test_hotplug_deadlocks();
		case 'q':
			break;
		default:
			printf("Command not recognized\n");
			break;
		}
		printf("\n\n========================================\n\n");
	} while(command != 'q');
}

//
// Main
int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	if (hid_init())
		return -1;

#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
	// To work properly needs to be called before hid_open/hid_open_path after hid_init.
	// Best/recommended option - call it right after hid_init.
	hid_darwin_set_open_exclusive(0);
#endif

	interactive_loop();

	/* Free static HIDAPI objects. */
	hid_exit();

	return 0;
}
