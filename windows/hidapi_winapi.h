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
 */

#ifndef HIDAPI_WINAPI_H__
#define HIDAPI_WINAPI_H__

#include <stdint.h>

#include "hidapi.h"

#ifdef __cplusplus
extern "C" {
#endif

		/**
		 * @brief Reconstructs a HID Report Descriptor from a Win32 HIDP_PREPARSED_DATA structure.
		 *  This reconstructed report descriptor is logical identical to the real report descriptor,
		 *  but not byte wise identical.
		 *
		 * @param[in]  hidp_preparsed_data Pointer to the HIDP_PREPARSED_DATA to read, i.e.: the value of PHIDP_PREPARSED_DATA,
		 *   as returned by HidD_GetPreparsedData WinAPI function.
		 * @param      buf       Pointer to the buffer where the report descriptor should be stored.
		 * @param[in]  buf_size  Size of the buffer. The recommended size for the buffer is @ref HID_API_MAX_REPORT_DESCRIPTOR_SIZE bytes.
		 *
		 * @return Returns size of reconstructed report descriptor if successful, -1 for error.
		 */
		int HID_API_EXPORT_CALL hid_winapi_descriptor_reconstruct_pp_data(void *hidp_preparsed_data, unsigned char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
