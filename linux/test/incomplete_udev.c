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

#define HIDAPI_UDEV_TEST_STUB(name) void name(void) {}

HIDAPI_UDEV_TEST_STUB(udev_new)
HIDAPI_UDEV_TEST_STUB(udev_unref)
HIDAPI_UDEV_TEST_STUB(udev_device_new_from_devnum)
HIDAPI_UDEV_TEST_STUB(udev_device_new_from_syspath)
HIDAPI_UDEV_TEST_STUB(udev_device_unref)
HIDAPI_UDEV_TEST_STUB(udev_device_get_parent_with_subsystem_devtype)
HIDAPI_UDEV_TEST_STUB(udev_device_get_syspath)
HIDAPI_UDEV_TEST_STUB(udev_device_get_devnode)
HIDAPI_UDEV_TEST_STUB(udev_device_get_sysattr_value)
HIDAPI_UDEV_TEST_STUB(udev_enumerate_new)
HIDAPI_UDEV_TEST_STUB(udev_enumerate_unref)
HIDAPI_UDEV_TEST_STUB(udev_enumerate_add_match_subsystem)
HIDAPI_UDEV_TEST_STUB(udev_enumerate_scan_devices)
HIDAPI_UDEV_TEST_STUB(udev_enumerate_get_list_entry)
HIDAPI_UDEV_TEST_STUB(udev_list_entry_get_next)

/* udev_list_entry_get_name is intentionally missing. */
