/*******************************************************
 Windows HID simplification

 Alan Ott
 Signal 11 Software

 8/22/2009

 Copyright 2009
 
 This contents of this file may be used by anyone
 for any reason without any conditions and may be
 used as a starting point for your own applications
 which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "hidapi.h"

// Headers needed for sleeping.
#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
#endif

int ctohex(char ch)
{
    ch = tolower(ch);
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

int parseHexByte(const char* text)
{
    int hi = ctohex(text[0]);
    if (hi < 0) {
        return hi;
    }
    int lo = ctohex(text[1]);
    if (lo < 0) {
        return lo;
    }
    return (hi << 4) | lo;
}

int parseHexWord(const char* text)
{
    int hi = parseHexByte(text);
    if (hi < 0) {
        return hi;
    }
	int lo = parseHexByte(text + 2);
	if (lo < 0) {
		return lo;
	}
    return (hi << 8) | lo;
}

int parseHex(const char* text, uint8_t* out)
{
	int length = 0;
	for (;;) {
		char ch = *text;
		// Line terminated by NUL ('\0')
		if (ch == '\0')
			break;
		if (ch == ',') {
			++text;
		}
		int b = parseHexByte(text);
		if (b < 0)
			return -1;
		text += 2;
		out[length++] = (uint8_t) b;
	}
	return length;
}

uint8_t wordfmt = 0;

int parseHexBuf(const char* text, uint8_t* out)
{
	int length = 0;
	for (;;) {
		char ch = *text;
		// Line terminated by NUL ('\0')
		if (ch == '\0')
			break;
		int b1 = parseHexByte(text);
		if (b1 < 0)
			return -1;
		text += 2;
		ch = *text;
		if (ch == ',') {
			++text;
			out[length++] = (uint8_t) b1;
			continue;
		}
		int b2 = parseHexByte(text);
		if (b2 < 0)
			return -1;
		text += 2;
		ch = *text;
		if (ch == ',') {
			++text;
			wordfmt = 1;
			out[length++] = b2;		// Little endian
			out[length++] = b1;
		} else {
			out[length++] = b1;		// Retain order
			out[length++] = b2;
		}
	}
	return length;
}

void dump_buf(const char* format, uint8_t* buf, int off, int len)
{
	int i = off;
	printf(format, buf[0]);
	while (i < len) {
		if (wordfmt) {
			uint8_t lo = buf[i];
			uint8_t hi = buf[i + 1];
			printf("%04x ", (hi << 8) | lo);
			i += 2;
		} else {
			uint8_t b = buf[i++];
			printf("%02hhx ", b);
		}
		if ((i % 16) == off) {
			putchar('\n');
		}
	}
	printf("\n");
}

int main(int argc, char* argv[])
{
	int res;
	uint8_t buf[256];
	uint8_t outbuf[256];
	#define MAX_STR 255
	wchar_t wstr[MAX_STR];
	hid_device *handle;
	int i;
	// Default Vendor & Product Ids
	uint16_t vid = 0x4d8;
	uint16_t pid = 0x3f;
	int oreport = 0;
	int inplen = 17;
	int feature = 0;
	uint8_t index = 0;
	uint8_t report = 0;

	// Display version & date
	fprintf(stderr, "hidtest v1.01 (03-Jun-2020)\n");

	if (argc <= 1) {
		fprintf(stderr, "usage: hidtest -v id -p id [-i index] [-r reportid] [-l len] (-f feature | -o oreport)\n");
		return 0;
	}

    for (i = 1; i < argc; ++i) {
        
		const char* arg = argv[i];
		const char* next;
		char ch = arg[0];
		if (ch == '-') {
			// Parse cmd line option
			ch = arg[1];
			switch (ch) {
				case 'v':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					vid = parseHexWord(next);
					break;
				case 'p':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					pid = parseHexWord(next);
					break;
				case 'i':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					index = parseHexWord(next);
					break;
				case 'r':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					report = parseHexByte(next);
					break;
				case 'l':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					inplen = parseHexByte(next);
					break;
				case 'f':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					} else {
						fprintf(stderr, "Missing hex argument after -%c\n", ch);
						return -2;
					}
					feature = parseHexBuf(next, outbuf);
					break;
				case 'o':
					if (arg[2] != '\0') {
						next = arg + 2;
					} else if (++i < argc) {
						next = argv[i];
					}
					oreport = parseHexBuf(next, outbuf);
					if (oreport < 0) {
						fprintf(stderr, "Error parsing hex command %s\n", next);
						return -1;
					}
					break;
				default:
					fprintf(stderr, "Invalid option '%c'\n", ch);
					return -2;
			} // switch
		} else {
			fprintf(stderr, "Missing option %s\n", arg);
			return -3;
		}
	}

	struct hid_device_info *devs, *cur_dev;
	
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
		printf("  Interface:    %d\n",  cur_dev->interface_number);
		printf("  Usage (page): 0x%hx (0x%hx)\n", cur_dev->usage, cur_dev->usage_page);
		printf("\n");
		cur_dev = cur_dev->next;
	}
	hid_free_enumeration(devs);

	// Open the device using the VID, PID,
	// and optionally the Serial number.
	handle = hid_open(vid, pid, NULL);
	if (!handle) {
		printf("unable to open device %04x:%04x\n", vid, pid);
 		return 1;
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
	printf("Serial Number String: (%d) %ls", wstr[0], wstr);
	printf("\n");

	if (index) {
		// Read Indexed String
		wstr[0] = 0x0000;
		res = hid_get_indexed_string(handle, index, wstr, MAX_STR);
		if (res < 0)
			printf("Unable to read indexed string %d\n", index);
		printf("Indexed String %d: %ls\n", index, wstr);
	}

	// Set the hid_read() function to be non-blocking.
	hid_set_nonblocking(handle, 1);
	
	// Try to read from the device. There should be no
	// data here, but execution should not block.
	res = hid_read(handle, buf, inplen);

	if (oreport > 0) {
		// Write oreport to the device
		res = hid_write(handle, outbuf, oreport);
		if (res < 0) {
			dump_buf("Unable to write oreport %02x\n", outbuf, 1, oreport);
		}
	} else if (feature) {
		// Send a Feature Report to the device
		res = hid_send_feature_report(handle, outbuf, oreport);
		if (res < 0) {
			dump_buf("Unable to send feature %02x\n", outbuf, 1, feature);
		}
	}

	// Read a Feature Report from the device
	memset(buf, 0, sizeof(buf));
	if (report != 0) {
		buf[0] = report;
		res = hid_get_feature_report(handle, buf, inplen);
		if (res <= 0) {
			printf("Unable to get a feature report %02x\n", report);
			printf("Error: %ls\n", hid_error(handle));
		} else {
			// Print out the returned buffer.
			dump_buf("Feature Report %02x\n", buf, 1, res);
		}
	}
	memset(buf,0,sizeof(buf));

	// Read requested state. hid_read() has been set to be
	// non-blocking by the call to hid_set_nonblocking() above.
	// This loop demonstrates the non-blocking nature of hid_read().
	res = 0;
	while (res == 0) {
		res = hid_read(handle, buf, sizeof(buf));
		if (res == 0)
			printf("waiting...\n");
		if (res < 0)
			printf("Unable to read()\n");
#ifdef WIN32
		Sleep(500);
#else
		usleep(500*1000);
#endif
	}

	// Print out the returned buffer.
	dump_buf("Data read:\n", buf, 0, res);

	hid_close(handle);

	/* Free static HIDAPI objects. */
	hid_exit();

	return 0;
}
