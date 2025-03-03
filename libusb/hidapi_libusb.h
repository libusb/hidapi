/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 libusb/hidapi Team

 Copyright 2021, All Rights Reserved.

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

/** @file
 * @defgroup API hidapi API

 * Since version 0.11.0, @ref HID_API_VERSION >= HID_API_MAKE_VERSION(0, 11, 0).
 */

#ifndef HIDAPI_LIBUSB_H__
#define HIDAPI_LIBUSB_H__

#include <stdint.h>

#include "hidapi.h"

#ifdef __cplusplus
extern "C" {
#endif

		/** @brief Open a HID device using libusb_wrap_sys_device.
			See https://libusb.sourceforge.io/api-1.0/group__libusb__dev.html#ga98f783e115ceff4eaf88a60e6439563c,
			for details on libusb_wrap_sys_device.

			@ingroup API
			@param sys_dev Platform-specific file descriptor that can be recognized by libusb.
			@param interface_num USB interface number of the device to be used as HID interface.
			Pass -1 to select first HID interface of the device.

			@returns
				This function returns a pointer to a #hid_device object on
				success or NULL on failure.
		*/
		HID_API_EXPORT hid_device * HID_API_CALL hid_libusb_wrap_sys_device(intptr_t sys_dev, int interface_num);

              /** @brief Similar to @ref hid_error but gives a libusb error code.

                     Since version 0.15.0, @ref HID_API_VERSION >= HID_API_MAKE_VERSION(0, 15, 0)

                     If the error occurred is not immediately caused by a libusb function call,
                     the returned value is 1. @ref hid_error would still contain a valid and meaningful error message.

                     @ingroup API
                     @param dev A device handle returned from hid_open(),
                       or NULL to get the last non-device-specific error
                       (e.g. for errors in hid_open() or hid_enumerate()).

                     @returns
                            enum libusb_error value representing last error code.
              */
              HID_API_EXPORT int HID_API_CALL hid_libusb_error(hid_device *dev);

#ifdef __cplusplus
}
#endif

#endif
