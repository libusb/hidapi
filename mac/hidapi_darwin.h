/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

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

/** @file
 * @defgroup API hidapi API
 */

#ifndef HIDAPI_DARWIN_H__
#define HIDAPI_DARWIN_H__

#include <stdint.h>

#include "hidapi.h"

#ifdef __cplusplus
extern "C" {
#endif

		/** @brief Get the location ID for a HID device.

			@ingroup API
			@param dev A device handle returned from hid_open().
			@param location_id The device's location ID on return.

			@returns
				This function returns 0 on success and -1 on error.
		*/
		int HID_API_EXPORT_CALL hid_darwin_get_location_id(hid_device *dev, uint32_t *location_id);

#ifdef __cplusplus
}
#endif

#endif
