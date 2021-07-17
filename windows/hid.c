/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 8/22/2009

 Copyright 2009, All Rights Reserved.

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

#include <windows.h>

#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif

#ifdef __MINGW32__
#include <ntdef.h>
#include <winbase.h>
#endif

#ifdef __CYGWIN__
#include <ntdef.h>
#define _wcsdup wcsdup
#endif

/* The maximum number of characters that can be passed into the
   HidD_Get*String() functions without it failing.*/
#define MAX_STRING_WCHARS 0xFFF

/*#define HIDAPI_USE_DDK*/

#ifdef __cplusplus
extern "C" {
#endif
	#include <setupapi.h>
	#include <winioctl.h>
	#ifdef HIDAPI_USE_DDK
		#include <hidsdi.h>
	#endif

	/* Copied from inc/ddk/hidclass.h, part of the Windows DDK. */
	#define HID_OUT_CTL_CODE(id)  \
		CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
	#define IOCTL_HID_GET_FEATURE                   HID_OUT_CTL_CODE(100)
	#define IOCTL_HID_GET_INPUT_REPORT              HID_OUT_CTL_CODE(104)

#ifdef __cplusplus
} /* extern "C" */
#endif

#include <stdio.h>
#include <stdlib.h>


#include "hidapi.h"

#undef MIN
#define MIN(x,y) ((x) < (y)? (x): (y))

#ifdef _MSC_VER
	/* Thanks Microsoft, but I know how to use strncpy(). */
	#pragma warning(disable:4996)
#endif

#ifdef __cplusplus
extern "C" {
#endif

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

#ifndef HIDAPI_USE_DDK
	/* Since we're not building with the DDK, and the HID header
	   files aren't part of the SDK, we have to define all this
	   stuff here. In lookup_functions(), the function pointers
	   defined below are set. */
	typedef struct _HIDD_ATTRIBUTES{
		ULONG Size;
		USHORT VendorID;
		USHORT ProductID;
		USHORT VersionNumber;
	} HIDD_ATTRIBUTES, *PHIDD_ATTRIBUTES;

	typedef USHORT USAGE, *PUSAGE;
	typedef struct _HIDP_CAPS {
		USAGE Usage;
		USAGE UsagePage;
		USHORT InputReportByteLength;
		USHORT OutputReportByteLength;
		USHORT FeatureReportByteLength;
		USHORT Reserved[17];
		USHORT NumberLinkCollectionNodes;
		USHORT NumberInputButtonCaps;
		USHORT NumberInputValueCaps;
		USHORT NumberInputDataIndices;
		USHORT NumberOutputButtonCaps;
		USHORT NumberOutputValueCaps;
		USHORT NumberOutputDataIndices;
		USHORT NumberFeatureButtonCaps;
		USHORT NumberFeatureValueCaps;
		USHORT NumberFeatureDataIndices;
	} HIDP_CAPS, *PHIDP_CAPS;
	typedef struct _HIDP_DATA {
		USHORT DataIndex;
		USHORT Reserved;
		union {
			ULONG   RawValue;
			BOOLEAN On;
		};
	} HIDP_DATA, * PHIDP_DATA;
	typedef void* PHIDP_PREPARSED_DATA;
	typedef struct _HIDP_LINK_COLLECTION_NODE {
		USAGE  LinkUsage;
		USAGE  LinkUsagePage;
		USHORT Parent;
		USHORT NumberOfChildren;
		USHORT NextSibling;
		USHORT FirstChild;
		ULONG  CollectionType : 8;
		ULONG  IsAlias : 1;
		ULONG  Reserved : 23;
		PVOID  UserContext;
	} HIDP_LINK_COLLECTION_NODE, * PHIDP_LINK_COLLECTION_NODE;
	typedef struct _HIDP_BUTTON_CAPS {
		USAGE   UsagePage;
		UCHAR   ReportID;
		BOOLEAN IsAlias;
		USHORT  BitField;
		USHORT  LinkCollection;
		USAGE   LinkUsage;
		USAGE   LinkUsagePage;
		BOOLEAN IsRange;
		BOOLEAN IsStringRange;
		BOOLEAN IsDesignatorRange;
		BOOLEAN IsAbsolute;
		ULONG   Reserved[10];
		union {
			struct {
				USAGE  UsageMin;
				USAGE  UsageMax;
				USHORT StringMin;
				USHORT StringMax;
				USHORT DesignatorMin;
				USHORT DesignatorMax;
				USHORT DataIndexMin;
				USHORT DataIndexMax;
			} Range;
			struct {
				USAGE  Usage;
				USAGE  Reserved1;
				USHORT StringIndex;
				USHORT Reserved2;
				USHORT DesignatorIndex;
				USHORT Reserved3;
				USHORT DataIndex;
				USHORT Reserved4;
			} NotRange;
		};
	} HIDP_BUTTON_CAPS, * PHIDP_BUTTON_CAPS;
	typedef struct _HIDP_VALUE_CAPS {
		USAGE   UsagePage;
		UCHAR   ReportID;
		BOOLEAN IsAlias;
		USHORT  BitField;
		USHORT  LinkCollection;
		USAGE   LinkUsage;
		USAGE   LinkUsagePage;
		BOOLEAN IsRange;
		BOOLEAN IsStringRange;
		BOOLEAN IsDesignatorRange;
		BOOLEAN IsAbsolute;
		BOOLEAN HasNull;
		UCHAR   Reserved;
		USHORT  BitSize;
		USHORT  ReportCount;
		USHORT  Reserved2[5];
		ULONG   UnitsExp;
		ULONG   Units;
		LONG    LogicalMin;
		LONG    LogicalMax;
		LONG    PhysicalMin;
		LONG    PhysicalMax;
		union {
			struct {
				USAGE  UsageMin;
				USAGE  UsageMax;
				USHORT StringMin;
				USHORT StringMax;
				USHORT DesignatorMin;
				USHORT DesignatorMax;
				USHORT DataIndexMin;
				USHORT DataIndexMax;
			} Range;
			struct {
				USAGE  Usage;
				USAGE  Reserved1;
				USHORT StringIndex;
				USHORT Reserved2;
				USHORT DesignatorIndex;
				USHORT Reserved3;
				USHORT DataIndex;
				USHORT Reserved4;
			} NotRange;
		};
	} HIDP_VALUE_CAPS, * PHIDP_VALUE_CAPS;
	typedef enum _HIDP_REPORT_TYPE {
		HidP_Input,
		HidP_Output,
		HidP_Feature,
		NUM_OF_HIDP_REPORT_TYPES
	} HIDP_REPORT_TYPE;
	/// <summary>
	/// Contains information about one global item that the HID parser did not recognize.
	/// </summary>
	typedef struct _HIDP_UNKNOWN_TOKEN {
		UCHAR Token; ///< Specifies the one-byte prefix of a global item.
		UCHAR Reserved[3];
		ULONG BitField; ///< Specifies the data part of the global item.
	} HIDP_UNKNOWN_TOKEN, * PHIDP_UNKNOWN_TOKEN;
	/// <summary>
	/// Contains global report descriptor items for that the HID parser did not recognized, for HID control
	/// </summary>
	typedef struct _HIDP_EXTENDED_ATTRIBUTES {
		UCHAR               NumGlobalUnknowns;
		UCHAR               Reserved[3];
		PHIDP_UNKNOWN_TOKEN GlobalUnknowns;
		ULONG               Data[1];
	} HIDP_EXTENDED_ATTRIBUTES, * PHIDP_EXTENDED_ATTRIBUTES;

#define HIDP_STATUS_SUCCESS 0x110000
#define	HIDP_STATUS_NULL 0x80110001
#define HIDP_STATUS_INVALID_PREPARSED_DATA 0xc0110001
#define HIDP_STATUS_INVALID_REPORT_TYPE 0xc0110002
#define HIDP_STATUS_INVALID_REPORT_LENGTH 0xc0110003
#define HIDP_STATUS_USAGE_NOT_FOUND 0xc0110004
#define HIDP_STATUS_VALUE_OUT_OF_RANGE 0xc0110005
#define HIDP_STATUS_BAD_LOG_PHY_VALUES 0xc0110006
#define HIDP_STATUS_BUFFER_TOO_SMALL 0xc0110007
#define	HIDP_STATUS_INTERNAL_ERROR 0xc0110008
#define	HIDP_STATUS_I8042_TRANS_UNKNOWN 0xc0110009
#define	HIDP_STATUS_INCOMPATIBLE_REPORT_ID 0xc011000a
#define	HIDP_STATUS_NOT_VALUE_ARRAY 0xc011000b
#define	HIDP_STATUS_IS_VALUE_ARRAY 0xc011000c
#define	HIDP_STATUS_DATA_INDEX_NOT_FOUND 0xc011000d
#define	HIDP_STATUS_DATA_INDEX_OUT_OF_RANGE 0xc011000e
#define	HIDP_STATUS_BUTTON_NOT_PRESSED 0xc011000f
#define	HIDP_STATUS_REPORT_DOES_NOT_EXIST 0xc0110010
#define	HIDP_STATUS_NOT_IMPLEMENTED 0xc0110020
#define	HIDP_STATUS_I8242_TRANS_UNKNOWN 0xc0110009

	typedef BOOLEAN (__stdcall *HidD_GetAttributes_)(HANDLE device, PHIDD_ATTRIBUTES attrib);
	typedef BOOLEAN (__stdcall *HidD_GetSerialNumberString_)(HANDLE device, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetManufacturerString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetProductString_)(HANDLE handle, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_SetFeature_)(HANDLE handle, PVOID data, ULONG length);
	typedef BOOLEAN (__stdcall *HidD_GetFeature_)(HANDLE handle, PVOID data, ULONG length);
	typedef BOOLEAN (__stdcall *HidD_GetInputReport_)(HANDLE handle, PVOID data, ULONG length);
	typedef BOOLEAN (__stdcall *HidD_GetIndexedString_)(HANDLE handle, ULONG string_index, PVOID buffer, ULONG buffer_len);
	typedef BOOLEAN (__stdcall *HidD_GetPreparsedData_)(HANDLE handle, PHIDP_PREPARSED_DATA *preparsed_data);
	typedef BOOLEAN (__stdcall *HidD_FreePreparsedData_)(PHIDP_PREPARSED_DATA preparsed_data);
	typedef NTSTATUS (__stdcall *HidP_GetCaps_)(PHIDP_PREPARSED_DATA preparsed_data, HIDP_CAPS *caps);
	typedef BOOLEAN (__stdcall *HidD_SetNumInputBuffers_)(HANDLE handle, ULONG number_buffers);
	typedef NTSTATUS (__stdcall *HidP_GetLinkCollectionNodes_)(PHIDP_LINK_COLLECTION_NODE link_collection_nodes, PULONG link_collection_nodes_length, PHIDP_PREPARSED_DATA preparsed_data);
	typedef NTSTATUS (__stdcall *HidP_GetButtonCaps_)(HIDP_REPORT_TYPE report_type, PHIDP_BUTTON_CAPS button_caps, PUSHORT button_caps_length, PHIDP_PREPARSED_DATA preparsed_data);
	typedef NTSTATUS (__stdcall *HidP_GetValueCaps_)(HIDP_REPORT_TYPE report_type, PHIDP_VALUE_CAPS value_caps,	PUSHORT value_caps_length, PHIDP_PREPARSED_DATA preparsed_data);
	typedef NTSTATUS (__stdcall *HidP_SetData_)(HIDP_REPORT_TYPE report_type, PHIDP_DATA data_list, PULONG data_length, PHIDP_PREPARSED_DATA preparsed_data, PCHAR report, ULONG report_length);
	typedef NTSTATUS (__stdcall* HidP_SetUsageValueArray_)(HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT link_collection, USAGE usage, PCHAR usage_value,	USHORT usage_value_byte_length, PHIDP_PREPARSED_DATA preparsed_data, PCHAR report, ULONG report_length);
	typedef NTSTATUS (__stdcall* HidP_SetUsages_)(HIDP_REPORT_TYPE report_type, USAGE usage_page, USHORT link_collection, PUSAGE usage_list, PULONG usage_length, PHIDP_PREPARSED_DATA preparsed_data, PCHAR report, ULONG report_length);
	typedef NTSTATUS (__stdcall* HidP_GetExtendedAttributes_)(HIDP_REPORT_TYPE report_type, USHORT data_index, PHIDP_PREPARSED_DATA preparsed_data, PHIDP_EXTENDED_ATTRIBUTES attributes, PULONG length_attributes);

	static HidD_GetAttributes_ HidD_GetAttributes;
	static HidD_GetSerialNumberString_ HidD_GetSerialNumberString;
	static HidD_GetManufacturerString_ HidD_GetManufacturerString;
	static HidD_GetProductString_ HidD_GetProductString;
	static HidD_SetFeature_ HidD_SetFeature;
	static HidD_GetFeature_ HidD_GetFeature;
	static HidD_GetInputReport_ HidD_GetInputReport;
	static HidD_GetIndexedString_ HidD_GetIndexedString;
	static HidD_GetPreparsedData_ HidD_GetPreparsedData;
	static HidD_FreePreparsedData_ HidD_FreePreparsedData;
	static HidP_GetCaps_ HidP_GetCaps;
	static HidD_SetNumInputBuffers_ HidD_SetNumInputBuffers;
	static HidP_GetLinkCollectionNodes_ HidP_GetLinkCollectionNodes;
	static HidP_GetButtonCaps_ HidP_GetButtonCaps;
	static HidP_GetValueCaps_ HidP_GetValueCaps;
	static HidP_SetData_ HidP_SetData;
	static HidP_SetUsageValueArray_ HidP_SetUsageValueArray;
	static HidP_SetUsages_ HidP_SetUsages;
	static HidP_GetExtendedAttributes_ HidP_GetExtendedAttributes;

	static HMODULE lib_handle = NULL;
	static BOOLEAN initialized = FALSE;
#endif /* HIDAPI_USE_DDK */

	typedef struct _hid_preparsed_data {
		UINT64 MagicKey;
		USAGE Usage;
		USAGE UsagePage;
		USHORT Reserved[3];	//USHORT NumberLinkCollectionNodes;
		USHORT NumberInputCaps;
		USHORT Reserved2;
		USHORT InputReportByteLength;
		USHORT NumberOutputCaps;
		USHORT Reserved3;
		USHORT OutputReportByteLength;
		USHORT NumberFeatureCaps;
		USHORT NumberCaps;
		USHORT FeatureReportByteLength;
		USHORT CapsByteLength;
		USHORT Reserved4;
	} hid_preparse_data, *phid_preparsed_data;


typedef struct _hid_caps {	
		USAGE   UsagePage;
		UCHAR   ReportID;
		UCHAR   BitPosition;
		USHORT  BitSize; // WIN32 term for Report Size
		USHORT  ReportCount;
		USHORT  BytePosition;
		USHORT  ByteSize;
		USHORT  BitCount;
		// BOOLEAN IsAlias; ???
		ULONG   BitField; // Not 2x USHORT??
		ULONG   Reserved;
		//USHORT  LinkCollection;
		USAGE   LinkUsage;
		USAGE   LinkUsagePage;
		/*
		BOOLEAN IsRange;
		BOOLEAN IsStringRange;
		BOOLEAN IsDesignatorRange;
		BOOLEAN IsAbsolute;
		BOOLEAN HasNull;
		UCHAR   Reserved;
		USHORT  Reserved2[5];*/
		ULONG   Reserved2[9];
		union {
			struct {
				USAGE  UsageMin;
				USAGE  UsageMax;
				USHORT StringMin;
				USHORT StringMax;
				USHORT DesignatorMin;
				USHORT DesignatorMax;
				USHORT DataIndexMin;
				USHORT DataIndexMax;
			} Range;
			struct {
				USAGE  Usage;
				USAGE  Reserved1;
				USHORT StringIndex;
				USHORT Reserved2;
				USHORT DesignatorIndex;
				USHORT Reserved3;
				USHORT DataIndex;
				USHORT Reserved4;
			} NotRange;
		ULONG   Reserved3;
		LONG    LogicalMin;
		LONG    LogicalMax;
		LONG    PhysicalMin;
		LONG    PhysicalMax;
		ULONG   Units;
		ULONG   UnitsExp;		
		};
} hid_caps, *phid_caps;


struct hid_device_ {
		HANDLE device_handle;
		BOOL blocking;
		USHORT output_report_length;
		unsigned char *write_buf;
		size_t input_report_length;
		USHORT feature_report_length;
		unsigned char *feature_buf;
		void *last_error_str;
		DWORD last_error_num;
		BOOL read_pending;
		char *read_buf;
		OVERLAPPED ol;
		OVERLAPPED write_ol;
};

static hid_device *new_hid_device()
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	dev->device_handle = INVALID_HANDLE_VALUE;
	dev->blocking = TRUE;
	dev->output_report_length = 0;
	dev->write_buf = NULL;
	dev->input_report_length = 0;
	dev->feature_report_length = 0;
	dev->feature_buf = NULL;
	dev->last_error_str = NULL;
	dev->last_error_num = 0;
	dev->read_pending = FALSE;
	dev->read_buf = NULL;
	memset(&dev->ol, 0, sizeof(dev->ol));
	dev->ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*initial state f=nonsignaled*/, NULL);
	memset(&dev->write_ol, 0, sizeof(dev->write_ol));
	dev->write_ol.hEvent = CreateEvent(NULL, FALSE, FALSE /*inital state f=nonsignaled*/, NULL);

	return dev;
}

static void free_hid_device(hid_device *dev)
{
	CloseHandle(dev->ol.hEvent);
	CloseHandle(dev->write_ol.hEvent);
	CloseHandle(dev->device_handle);
	LocalFree(dev->last_error_str);
	free(dev->write_buf);
	free(dev->feature_buf);
	free(dev->read_buf);
	free(dev);
}

static void register_error(hid_device *dev, const char *op)
{
	WCHAR *ptr, *msg;
	(void)op; // unreferenced  param
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPWSTR)&msg, 0/*sz*/,
		NULL);

	/* Get rid of the CR and LF that FormatMessage() sticks at the
	   end of the message. Thanks Microsoft! */
	ptr = msg;
	while (*ptr) {
		if (*ptr == L'\r') {
			*ptr = L'\0';
			break;
		}
		ptr++;
	}

	/* Store the message off in the Device entry so that
	   the hid_error() function can pick it up. */
	LocalFree(dev->last_error_str);
	dev->last_error_str = msg;
}

#ifndef HIDAPI_USE_DDK
static int lookup_functions()
{
	lib_handle = LoadLibraryA("hid.dll");
	if (lib_handle) {
#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#define RESOLVE(x) x = (x##_)GetProcAddress(lib_handle, #x); if (!x) return -1;
		RESOLVE(HidD_GetAttributes);
		RESOLVE(HidD_GetSerialNumberString);
		RESOLVE(HidD_GetManufacturerString);
		RESOLVE(HidD_GetProductString);
		RESOLVE(HidD_SetFeature);
		RESOLVE(HidD_GetFeature);
		RESOLVE(HidD_GetInputReport);
		RESOLVE(HidD_GetIndexedString);
		RESOLVE(HidD_GetPreparsedData);
		RESOLVE(HidD_FreePreparsedData);
		RESOLVE(HidP_GetCaps);
		RESOLVE(HidD_SetNumInputBuffers);
		RESOLVE(HidP_GetLinkCollectionNodes);
		RESOLVE(HidP_GetButtonCaps);
		RESOLVE(HidP_GetValueCaps);
		RESOLVE(HidP_SetData);
		RESOLVE(HidP_SetUsageValueArray);
		RESOLVE(HidP_SetUsages);
		RESOLVE(HidP_GetExtendedAttributes);
#undef RESOLVE
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif
	}
	else
		return -1;

	return 0;
}
#endif

/// <summary>
/// Enumeration of all report descriptor item One-Byte prefixes from the USB HID spec 1.11
/// The two least significiant bits nn represent the size of the item and must be added to this values
/// </summary>
typedef enum rd_items_ {
	rd_main_input = 0x80, ///< 1000 00 nn
	rd_main_output =			  0x90, ///< 1001 00 nn
	rd_main_feature =			  0xB0, ///< 1011 00 nn
	rd_main_collection =		  0xA0, ///< 1010 00 nn
	rd_main_collection_end =	  0xC0, ///< 1100 00 nn
	rd_global_usage_page =		  0x04, ///< 0000 01 nn
	rd_global_logical_minimum =   0x14, ///< 0001 01 nn
	rd_global_logical_maximum =   0x24, ///< 0010 01 nn
	rd_global_physical_minimum =  0x34, ///< 0011 01 nn
	rd_global_physical_maximum =  0x44, ///< 0100 01 nn
	rd_global_unit_exponent =     0x54, ///< 0101 01 nn
	rd_global_unit =              0x64, ///< 0110 01 nn
	rd_global_report_size =       0x74, ///< 0111 01 nn
	rd_global_report_id =         0x84, ///< 1000 01 nn
	rd_global_report_count =      0x94, ///< 1001 01 nn
	rd_global_push =              0xA4, ///< 1010 01 nn
	rd_global_pop =               0xB4, ///< 1011 01 nn
	rd_local_usage =              0x08, ///< 0000 10 nn
	rd_local_usage_minimum =      0x18, ///< 0001 10 nn
	rd_local_usage_maximum =      0x28, ///< 0010 10 nn
	rd_local_designator_index =   0x38, ///< 0011 10 nn
	rd_local_designator_minimum = 0x48, ///< 0100 10 nn
	rd_local_designator_maximum = 0x58, ///< 0101 10 nn
	rd_local_string =             0x78, ///< 0111 10 nn
	rd_local_string_minimum =     0x88, ///< 1000 10 nn
	rd_local_string_maximum =     0x98, ///< 1001 10 nn
	rd_local_delimiter =          0xA8  ///< 1010 10 nn
} RD_ITEMS;

/// <summary>
/// List element of the encoded report descriptor
/// </summary>
struct rd_item_byte
{
	unsigned char byte;
	struct rd_item_byte* next;
};

/// <summary>
/// Function that appends a byte to encoded report descriptor list
/// </summary>
/// <param name="byte">Single byte to append</param>
/// <param name="list">Pointer to the list</param>
static void rd_append_byte(unsigned char byte, struct rd_item_byte** list) {
		struct rd_item_byte* new_list_element;

		/* Determine last list position */
		while (*list != NULL)
		{
			list = &(*list)->next;
		}

		new_list_element = malloc(sizeof(*new_list_element)); // Create new list entry
		new_list_element->byte = byte;
		new_list_element->next = NULL; // Marks last element of list

		*list = new_list_element;
	}


/// <summary>
///  Writes a short report descriptor item according USB HID spec 1.11 chapter 6.2.2.2
/// </summary>
/// <param name="rd_item">Enumeration identifying type (Main, Global, Local) and function (e.g Usage or Report Count) of the item.</param>
/// <param name="data">Data (Size depends on rd_item 0,1,2 or 4bytes)</param>
/// <param name="list">Chained list of report descriptor bytes</param>
/// <returns>Returns 0 if successful, -1 for error</returns>
static int rd_write_short_item(RD_ITEMS rd_item, LONG64 data, struct rd_item_byte** list) {
	if (rd_item & 0x03) {
		return -1; // Invaid input data
	}

	if (rd_item == rd_main_collection_end) {
		// Item without data
		unsigned char oneBytePrefix = rd_item + 0x00;
		rd_append_byte(oneBytePrefix, list);
		printf("%02X               ", oneBytePrefix);
	} else if ((rd_item == rd_global_logical_minimum) ||
			   (rd_item == rd_global_logical_maximum) ||
	      	   (rd_item == rd_global_physical_minimum) ||
		       (rd_item == rd_global_physical_maximum)) {
		// Item with signed integer data
		if ((data >= -128) && (data <= 127)) {
			unsigned char oneBytePrefix = rd_item + 0x01;
			char localData = (char)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			printf("%02X %02X            ", oneBytePrefix, localData & 0xFF);
		}
		else if ((data >= -32768) && (data <= 32767)) {
			unsigned char oneBytePrefix = rd_item + 0x02;
			INT16 localData = (INT16)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			rd_append_byte(localData >> 8 & 0xFF, list);
			printf("%02X %02X %02X         ", oneBytePrefix, localData & 0xFF, localData >> 8 & 0xFF);
		}
		else if ((data >= -2147483648LL) && (data <= 2147483647)) {
			unsigned char oneBytePrefix = rd_item + 0x03;
			INT32 localData = (INT32)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			rd_append_byte(localData >> 8 & 0xFF, list);
			rd_append_byte(localData >> 16 & 0xFF, list);
			rd_append_byte(localData >> 24 & 0xFF, list);
			printf("%02X %02X %02X %02X %02X   ", oneBytePrefix, localData & 0xFF, localData >> 8 & 0xFF, localData >> 16 & 0xFF, localData >> 24 & 0xFF);
		} else {
			// Error data out of range
			return -1;
		}
	} else {
		// Item with unsigned integer data
		if ((data >= 0) && (data <= 0xFF)) {
			unsigned char oneBytePrefix = rd_item + 0x01;
			unsigned char localData = (unsigned char)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			printf("%02X %02X            ", oneBytePrefix, localData & 0xFF);
		}
		else if ((data >= 0) && (data <= 0xFFFF)) {
			unsigned char oneBytePrefix = rd_item + 0x02;
			UINT16 localData = (UINT16)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			rd_append_byte(localData >> 8 & 0xFF, list);
			printf("%02X %02X %02X         ", oneBytePrefix, localData & 0xFF, localData >> 8 & 0xFF);
		}
		else if ((data >= 0) && (data <= 0xFFFFFFFF)) {
			unsigned char oneBytePrefix = rd_item + 0x03;
			UINT32 localData = (UINT32)data;
			rd_append_byte(oneBytePrefix, list);
			rd_append_byte(localData & 0xFF, list);
			rd_append_byte(localData >> 8 & 0xFF, list);
			rd_append_byte(localData >> 16 & 0xFF, list);
			rd_append_byte(localData >> 24 & 0xFF, list);
			printf("%02X %02X %02X %02X %02X   ", oneBytePrefix, localData & 0xFF, localData >> 8 & 0xFF, localData >> 16 & 0xFF, localData >> 24 & 0xFF);
		} else {
			// Error data out of range
			return -1;
		}
	}
	return 0;
}

/// <summary>
/// Writes a long report descriptor item according USB HID spec 1.11 chapter 6.2.2.3
/// </summary>
/// <param name="data">Optional data items (NULL if bDataSize is 0)</param>
/// <param name="bLongItemTag">Long item tag (8 bits)</param>
/// <param name="bDataSize">Size of long item data (range 0-255 in Bytes)</param>
/// <returns></returns>
static int rd_write_long_item(unsigned char* data, unsigned char bLongItemTag, unsigned char bDataSize) {
	// Not implemented
}

/// <summary>
/// Function that determines the first bit in 1 state in a report
/// (The first byte containing the Report-ID byte is ignored and not counted)
/// </summary>
/// <param name="dummy_report">Report array</param>
/// <param name="max_report_length">Length of the report in bytes</param>
/// <returns>Position of the determined bit (postion of first bit is 0)</returns>
static int rd_find_first_one_bit(char* dummy_report, unsigned int max_report_length) {
	int first_one_bit = -1;
	for (unsigned int byteIdx = 1; byteIdx < max_report_length; byteIdx++)
	{
		if (dummy_report[byteIdx] != 0) {
			for (int bitIdx = 0; bitIdx < 8; bitIdx++)
			{
				if (dummy_report[byteIdx] & (0x01 << bitIdx))
				{
					first_one_bit = 8 * (byteIdx - 1) + bitIdx;// First byte with the Report ID isn't counted
					break;
				}
			}
			break;
		}
	}
	return first_one_bit;
}

/// <summary>
/// Function that determines the last bit in 1 state in a report
/// (The first byte containing the Report-ID byte is ignored and not counted)
/// </summary>
/// <param name="dummy_report">Report array</param>
/// <param name="max_report_length">Length of the report in bytes</param>
/// <returns>Position of the determined bit (postion of first bit is 0)</returns>
static int rd_find_last_one_bit(char* dummy_report, unsigned int max_report_length) {
	int last_one_bit = -1;
	for (unsigned int byteIdx = max_report_length - 1; byteIdx > 1; byteIdx--)
	{
		if (dummy_report[byteIdx] != 0) {
			for (int bitIdx = 0; bitIdx < 7; bitIdx--)
			{
				if (dummy_report[byteIdx] & (0x01 << bitIdx))
				{
					last_one_bit = 8 * (byteIdx - 1) + (8 - bitIdx); // First byte with the Report ID isn't counted
					break;
				}
			}
			break;
		}
	}
	return last_one_bit;
}

/// <summary>
/// Function that determines the bit position of a button cap in a report
/// This is done by setting values in empty reports using HidP_SetData vor variable bit fields
/// and HidP_SetUsages for button arrays
/// </summary>
/// <param name="report_type">Input, Output or Feature</param>
/// <param name="button_cap">Data structure that represents the button cap</param>
/// <param name="first_bit">Returns the position of the first bit of this button cap in the report</param>
/// <param name="last_bit">Returns the position of the last bit of this button cap in the report</param>
/// <param name="button_array_count">If the button cap represents an array, this returns the number of array elements (Report Count) otherwise -1</param>
/// <param name="button_array_size">If the button cap represents an array, this returns the size in bits of an array element (Report Size) otherwise -1</param>
/// <param name="max_report_length">Maximum size of any report of this type (input, Output, or Feature)</param>
/// <param name="pp_data">Preparsed Data structure containing the report descriptor information of the HID device</param>
static void rd_determine_button_bitpositions(HIDP_REPORT_TYPE report_type, PHIDP_BUTTON_CAPS button_cap, int* first_bit, int* last_bit, int* button_array_count, int * button_array_size, unsigned int max_report_length, PHIDP_PREPARSED_DATA pp_data) {
	char* dummy_report;

	dummy_report = (char*)malloc((max_report_length) * sizeof(char));
	dummy_report[0] = button_cap->ReportID;
	for (unsigned int i = 1; i < max_report_length; i++) {
		dummy_report[i] = 0x00;
	}

	NTSTATUS status;

	*(first_bit) = -1;
	*(last_bit) = -1;

	// USB HID spec 1.11 chapter 6.2.2.4 Main Items, defines bit 1 as: {Array (0) | Variable (1)}  
	if ((button_cap->BitField & 0x02) == 0x02) {
		// Variable (1) - also refered as Bit Field (one bit in the report for each button)

		HIDP_DATA dummy_hidp_data;

		unsigned int bit_size;
		if (button_cap->IsRange) {
			dummy_hidp_data.DataIndex = button_cap->Range.DataIndexMin;
			bit_size = button_cap->Range.DataIndexMax - button_cap->Range.DataIndexMin + 1; // Number of buttons
		}
		else {
			dummy_hidp_data.DataIndex = button_cap->NotRange.DataIndex;
			bit_size = 1; // Single button
		}
		dummy_hidp_data.RawValue = 0xffffffffu;
		ULONG dummy_datalength = 1;

		status = HidP_SetData(report_type, &dummy_hidp_data, &dummy_datalength, pp_data, dummy_report, max_report_length);
		if (status == HIDP_STATUS_SUCCESS) {
			*(first_bit) = rd_find_first_one_bit(dummy_report, max_report_length);
			*(last_bit) = *(first_bit)+bit_size - 1; // Button variable fields have only one bit
			*(button_array_count) = -1;
			*(button_array_size) = -1;
		}
		else if (status == HIDP_STATUS_IS_VALUE_ARRAY) {
			// NOT WORKING YET (at least not for the case of usage == 0)
			// According to https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_setdata 
			// -> Except for usage value arrays, a user - mode application or kernel - mode driver can use HidP_SetData to set buttons and usage values in a report.To set a usage value array, an application or driver must use HidP_SetUsageValueArray.
			CHAR usage_list[2] = { 0xFF,0xFF };
			USHORT datalength = 2;
			// Problem: HidP_SetUsageValueArray sets no bits if Usage == 0
			status = HidP_SetUsageValueArray(report_type, button_cap->UsagePage, button_cap->LinkCollection, button_cap->NotRange.Usage, &usage_list[0], datalength, pp_data, dummy_report, max_report_length);

			if (status == HIDP_STATUS_SUCCESS) {
				*(first_bit) = rd_find_first_one_bit(dummy_report, max_report_length);
				*(last_bit) = rd_find_last_one_bit(dummy_report, max_report_length);
				*(button_array_count) = *(last_bit)-*(first_bit)+1;
				*(button_array_size) = 1; // This is known, because it's recognized as button_cap
			}
			// NOT WORKING YET (at least not for the case of usage == 0)
		}
	}
	else {
		// Array (0)

		int report_count = -1;

		for (ULONG report_count_idx = 0; report_count_idx <= (ULONG)(button_cap->Range.UsageMax - button_cap->Range.UsageMin); report_count_idx++) {

			for (unsigned int i = 1; i < max_report_length; i++) {
				dummy_report[i] = 0x00;
			}
			ULONG usage_len = report_count_idx + 1;
			PUSAGE usage_list;
			usage_list = (PUSAGE)malloc(usage_len * sizeof(USAGE));
			for (unsigned int i = 0; i < usage_len; i++) {
				usage_list[i] = button_cap->Range.UsageMax - i; // Will only set bits, if usage not 0. UsageMin might be 0, therfore go down from UsageMax
			}
			status = HidP_SetUsages(report_type, button_cap->UsagePage, button_cap->LinkCollection, usage_list, &usage_len, pp_data, dummy_report, max_report_length);
			free(usage_list);
			if (status == HIDP_STATUS_BUFFER_TOO_SMALL) {
				printf("HIDP_STATUS_BUFFER_TOO_SMALL\n");
				report_count = report_count_idx;
				break;
			}
		}

		// Find first bit position
		ULONG   usage_len = 1;
		USAGE   usage_list[] = { button_cap->Range.UsageMin };
		for (USAGE usage_idx = button_cap->Range.UsageMin; usage_idx < button_cap->Range.UsageMax; usage_idx++) {

			for (unsigned int i = 1; i < max_report_length; i++) {
				dummy_report[i] = 0x00;
			}
			usage_list[0] = usage_idx;
			status = HidP_SetUsages(report_type, button_cap->UsagePage, button_cap->LinkCollection, &usage_list[0], &usage_len, pp_data, dummy_report, max_report_length);
			if (status == HIDP_STATUS_SUCCESS) {
				int local_first_bit = rd_find_first_one_bit(dummy_report, max_report_length);
				if ((local_first_bit != -1) &&
					((*(first_bit) == -1) ||
						(local_first_bit < *(first_bit)))) {
					*(first_bit) = local_first_bit;
				}
			}
		}
		
		if (report_count == 1 || !button_cap->IsRange || (button_cap->Range.UsageMin == 0 && button_cap->Range.UsageMax <= 1)) {
			// Calculate how many bits are needed to represent the maximum usage value
			// The condition (button_cap->Range.UsageMin == 0 && button_cap->Range.UsageMax <= 1) is 
			// a needed, because the algorithm for multiple array elements can only work with two or more usages != 0
			int number_of_bits = 0; // Number of bits needed to represent the (maximum) usage value
			USAGE temp;
			if (button_cap->IsRange) {
				temp = button_cap->Range.UsageMax;
			}
			else
			{
				temp = button_cap->NotRange.Usage;
			}
			while (temp >= 1) { temp = temp >> 1; ++number_of_bits; }
			*(last_bit) = *(first_bit)+(report_count * number_of_bits) - 1;
			*(button_array_count) = report_count;
			*(button_array_size) = number_of_bits;
		}
		else {
			// An array with multiple elements might have padding bits after each value
			// e.g. a 102 key keyboard needs only 7 bit to represent all keys, but the array 
			//      has always 8 bit elements
			// To cover this case, it's not suficient to know only the bit positions of the first array element,
			// it's necessary to detect the bit positions of another array element too.

			// Find last bit position of first and second array element (for UsageMax)

			ULONG   usage_len;
			USAGE   usage_list[2];

			// Set only first array element (first usage => UsageMax)
			usage_len = 1;
			usage_list[0] = button_cap->Range.UsageMax;
			int first_element_last_bit = -1;

			for (unsigned int i = 1; i < max_report_length; i++) {
				dummy_report[i] = 0x00;
			}

			status = HidP_SetUsages(report_type, button_cap->UsagePage, button_cap->LinkCollection, &usage_list[0], &usage_len, pp_data, dummy_report, max_report_length);
			if (status == HIDP_STATUS_SUCCESS) {
				first_element_last_bit = rd_find_last_one_bit(dummy_report, max_report_length);
			}
			else {
				printf("first_element_last_bit detection failed: %d", status);
			}
			

			// fill first array element with data (first usage => UsageMax-1)
			// Set second array element (second usage => UsageMax)
			usage_len = 2;
			usage_list[0] = button_cap->Range.UsageMax-1;
			usage_list[1] = button_cap->Range.UsageMax;
			int second_element_last_bit = -1;

			for (unsigned int i = 1; i < max_report_length; i++) {
				dummy_report[i] = 0x00;
			}

			status = HidP_SetUsages(report_type, button_cap->UsagePage, button_cap->LinkCollection, &usage_list[0], &usage_len, pp_data, dummy_report, max_report_length);
			if (status == HIDP_STATUS_SUCCESS) {
				second_element_last_bit = rd_find_last_one_bit(dummy_report, max_report_length);
			}
			else {
				printf("second_element_last_bit detection failed: %d", status);
			}
			int bit_distance_between_array_elements = second_element_last_bit - first_element_last_bit;
			*(last_bit) = *(first_bit)+(report_count * bit_distance_between_array_elements) - 1;
			*(button_array_count) = report_count;
			*(button_array_size) = bit_distance_between_array_elements;
		}
	}
	free(dummy_report);
}

/// <summary>
/// Function that determines the bit position of a value cap in a report
/// This is done by setting values in empty reports using HidP_SetData vor variable fields
/// and HidP_SetUsageValueArray for value arrays
/// </summary>
/// <param name="report_type">Input, Output or Feature</param>
/// <param name="value_cap">Data structure that represents the value cap</param>
/// <param name="first_bit">Returns the position of the first bit of this value cap in the report</param>
/// <param name="last_bit">Returns the position of the last bit of this value cap in the report</param>
/// <param name="max_report_length">Maximum size of any report of this type (input, Output, or Feature)</param>
/// <param name="pp_data">Preparsed Data structure containing the report descriptor information of the HID device</param>
/// <summary>
static void rd_determine_value_bitpositions(HIDP_REPORT_TYPE report_type, PHIDP_VALUE_CAPS value_cap, int* first_bit, int* last_bit, unsigned int max_report_length, PHIDP_PREPARSED_DATA pp_data) {
	unsigned char* dummy_report;

	dummy_report = (unsigned char*)malloc(max_report_length * sizeof(unsigned char));

	dummy_report[0] = value_cap->ReportID;
	for (unsigned int i = 1; i < max_report_length; i++) {
		dummy_report[i] = 0x00;
	}
		
	*(first_bit) = -1;
	*(last_bit) = -1;

	// USB HID spec 1.11 chapter 6.2.2.4 Main Items, defines bit 1 as: {Array (0) | Variable (1)}  
	if ((value_cap->BitField & 0x02) == 0x02) {
		// Variable (1)

		HIDP_DATA dummy_hidp_data;
		
		dummy_hidp_data.DataIndex = value_cap->NotRange.DataIndex;
		dummy_hidp_data.RawValue = 0xffffffffu;
		ULONG dummy_datalength = 1;

		if (HidP_SetData(report_type, &dummy_hidp_data, &dummy_datalength, pp_data, dummy_report, max_report_length) == HIDP_STATUS_SUCCESS) {
			*(first_bit) = rd_find_first_one_bit(dummy_report, max_report_length);
			*(last_bit) = *(first_bit)+(value_cap->ReportCount * value_cap->BitSize) - 1;
		}
	}
	else {
		// Array (0)

		// EXPERIMENTAL - No device available for test

		int number_of_dummy_usage_bits = (value_cap->ReportCount * value_cap->BitSize + 7);
		
		PUCHAR usage_value = malloc(number_of_dummy_usage_bits / 8 * sizeof(unsigned char));

		for (int i = 0; i < number_of_dummy_usage_bits / 8; i++) { usage_value[i] = 0xFF; }

		if (HidP_SetUsageValueArray(report_type, value_cap->UsagePage, value_cap->LinkCollection, value_cap->Range.UsageMin, usage_value, number_of_dummy_usage_bits / 8, pp_data, dummy_report, max_report_length) == HIDP_STATUS_SUCCESS) {
			*(first_bit) = rd_find_first_one_bit(dummy_report, max_report_length);
			*(last_bit) = *(first_bit)+((value_cap->Range.DataIndexMax - value_cap->Range.DataIndexMin + 1) * value_cap->BitSize) - 1;
		}
		free(usage_value);
		// EXPERIMENTAL - No device available for test
	}
	free(dummy_report);
}

typedef enum _RD_MAIN_ITEMS {
	rd_input = HidP_Input,
	rd_output = HidP_Output,
	rd_feature = HidP_Feature,
	rd_collection,
	rd_collection_end,
	RD_NUM_OF_MAIN_ITEMS
} RD_MAIN_ITEMS;

typedef struct _RD_BIT_RANGE {
	int FirstBit;
	int LastBit;
} RD_BIT_RANGE;

typedef enum _RD_ITEM_NODE_TYPE {
	rd_item_node_value,
	rd_item_node_button,
	rd_item_node_padding,
	rd_item_node_collection,
	RD_NUM_OF_ITEM_NODE_TYPES
} RD_NODE_TYPE;

struct rd_main_item_node
{
	int FirstBit; ///< Position of first bit in report (counting from 0)
	int LastBit; ///< Position of last bit in report (counting from 0)
	int ButtonArrayCount; ///< If the button cap represents an array, this is the number of array elements (Report Count) otherwise -1
	int ButtonArraySize; ///< If the button cap represents an array, this is the size in bits of an array element (Report Size) otherwise -1
	RD_NODE_TYPE TypeOfNode; ///< Information if caps index refers to the array of button caps, value caps,
							 ///< or if the node is just a padding element to fill unused bit positions.
	                         ///< The node can also be a collection node without any bits in the report.
	int CapsIndex; ///< Index in the array of button_caps or value caps
	int CollectionIndex; ///< Index in the array of link collections
	RD_MAIN_ITEMS MainItemType; ///< Input, Output, Feature, Collection or Collection End
	unsigned char ReportID; 
	struct rd_main_item_node* next; 
};

	
static struct rd_main_item_node* rd_append_main_item_node(int first_bit, int last_bit, int button_array_count, int button_array_size, RD_NODE_TYPE type_of_node, int caps_index, int collection_index, RD_MAIN_ITEMS main_item_type, unsigned char report_id, struct rd_main_item_node** list) {
	struct rd_main_item_node* new_list_node;

	// Determine last node in the list
	while (*list != NULL)
	{
		list = &(*list)->next;
	}

	new_list_node = malloc(sizeof(*new_list_node)); // Create new list entry
	new_list_node->FirstBit = first_bit;
	new_list_node->LastBit = last_bit;
	new_list_node->ButtonArrayCount = button_array_count;
	new_list_node->ButtonArraySize = button_array_size;
	new_list_node->TypeOfNode = type_of_node;
	new_list_node->CapsIndex = caps_index;
	new_list_node->CollectionIndex = collection_index;
	new_list_node->MainItemType = main_item_type;
	new_list_node->ReportID = report_id;
	new_list_node->next = NULL; // NULL marks last node in the list

	*list = new_list_node;
	return new_list_node;
}

static struct rd_main_item_node* rd_insert_main_item_node(int search_bit, int first_bit, int last_bit, int button_array_count, int button_array_size, RD_NODE_TYPE type_of_node, int caps_index, int collection_index, RD_MAIN_ITEMS main_item_type, unsigned char report_id, struct rd_main_item_node** list) {
	struct rd_main_item_node* new_list_node;
	// Determine last node in the list
	
	while (((*list)->next->MainItemType != rd_collection) &&
		   ((*list)->next->MainItemType != rd_collection_end) &&
		   !(((*list)->next->FirstBit > search_bit) &&
		   ((*list)->next->ReportID == report_id) &&
		   ((*list)->next->MainItemType == main_item_type))
		)
	{
		list = &(*list)->next;
	}

	new_list_node = malloc(sizeof(*new_list_node)); // Create new list entry
	new_list_node->FirstBit = first_bit;
	new_list_node->LastBit= last_bit;
	new_list_node->ButtonArrayCount = button_array_count;
	new_list_node->ButtonArraySize = button_array_size;
	new_list_node->TypeOfNode = type_of_node;
	new_list_node->CapsIndex = caps_index;
	new_list_node->CollectionIndex = collection_index;
	new_list_node->MainItemType = main_item_type;
	new_list_node->ReportID = report_id;
	new_list_node->next = (*list)->next;

	(*list)->next = new_list_node;
	return new_list_node;
}

static HANDLE open_device(const char *path, BOOL open_rw)
{
	HANDLE handle;
	DWORD desired_access = (open_rw)? (GENERIC_WRITE | GENERIC_READ): 0;
	DWORD share_mode = FILE_SHARE_READ|FILE_SHARE_WRITE;

	handle = CreateFileA(path,
		desired_access,
		share_mode,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,/*FILE_ATTRIBUTE_NORMAL,*/
		0);

	return handle;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version()
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str()
{
	return HID_API_VERSION_STR;
}

int HID_API_EXPORT hid_init(void)
{
#ifndef HIDAPI_USE_DDK
	if (!initialized) {
		if (lookup_functions() < 0) {
			hid_exit();
			return -1;
		}
		initialized = TRUE;
	}
#endif
	return 0;
}

int HID_API_EXPORT hid_exit(void)
{
#ifndef HIDAPI_USE_DDK
	if (lib_handle)
		FreeLibrary(lib_handle);
	lib_handle = NULL;
	initialized = FALSE;
#endif
	return 0;
}

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	BOOL res;
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;

	/* Hard-coded GUID retreived by HidD_GetHidGuid */
	GUID InterfaceClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} };

	/* Windows objects for interacting with the driver. */
	SP_DEVINFO_DATA devinfo_data;
	SP_DEVICE_INTERFACE_DATA device_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
	HDEVINFO device_info_set = INVALID_HANDLE_VALUE;
	char driver_name[256];
	int device_index = 0;

	if (hid_init() < 0)
		return NULL;

	/* Initialize the Windows objects. */
	memset(&devinfo_data, 0x0, sizeof(devinfo_data));
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	/* Get information for all the devices belonging to the HID class. */
	device_info_set = SetupDiGetClassDevsA(&InterfaceClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	/* Iterate over each device in the HID class, looking for the right one. */

	for (;;) {
		HANDLE write_handle = INVALID_HANDLE_VALUE;
		DWORD required_size = 0;
		HIDD_ATTRIBUTES attrib;

		res = SetupDiEnumDeviceInterfaces(device_info_set,
			NULL,
			&InterfaceClassGuid,
			device_index,
			&device_interface_data);

		if (!res) {
			/* A return of FALSE from this function means that
			   there are no more devices. */
			break;
		}

		/* Call with 0-sized detail size, and let the function
		   tell us how long the detail struct needs to be. The
		   size is put in &required_size. */
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			NULL,
			0,
			&required_size,
			NULL);

		/* Allocate a long enough structure for device_interface_detail_data. */
		device_interface_detail_data = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*) malloc(required_size);
		device_interface_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		/* Get the detailed data for this device. The detail data gives us
		   the device path for this device, which is then passed into
		   CreateFile() to get a handle to the device. */
		res = SetupDiGetDeviceInterfaceDetailA(device_info_set,
			&device_interface_data,
			device_interface_detail_data,
			required_size,
			NULL,
			NULL);

		if (!res) {
			/* register_error(dev, "Unable to call SetupDiGetDeviceInterfaceDetail");
			   Continue to the next device. */
			goto cont;
		}

		/* Populate devinfo_data. This function will return failure
		   when the device with such index doesn't exist. We've already checked it does. */
		res = SetupDiEnumDeviceInfo(device_info_set, device_index, &devinfo_data);
		if (!res)
			goto cont;


		/* Make sure this device has a driver bound to it. */
		res = SetupDiGetDeviceRegistryPropertyA(device_info_set, &devinfo_data,
			   SPDRP_DRIVER, NULL, (PBYTE)driver_name, sizeof(driver_name), NULL);
		if (!res)
			goto cont;

		//wprintf(L"HandleName: %s\n", device_interface_detail_data->DevicePath);

		/* Open a handle to the device */
		write_handle = open_device(device_interface_detail_data->DevicePath, FALSE);

		/* Check validity of write_handle. */
		if (write_handle == INVALID_HANDLE_VALUE) {
			/* Unable to open the device. */
			//register_error(dev, "CreateFile");
			goto cont_close;
		}


		/* Get the Vendor ID and Product ID for this device. */
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		HidD_GetAttributes(write_handle, &attrib);
		//wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);

		/* Check the VID/PID to see if we should add this
		   device to the enumeration list. */
		if ((vendor_id == 0x0 || attrib.VendorID == vendor_id) &&
		    (product_id == 0x0 || attrib.ProductID == product_id)) {

			#define WSTR_LEN 512
			const char *str;
			struct hid_device_info *tmp;
			PHIDP_PREPARSED_DATA pp_data = NULL;
			HIDP_CAPS caps;
			NTSTATUS nt_res;
			wchar_t wstr[WSTR_LEN]; /* TODO: Determine Size */
			size_t len;

			/* VID/PID match. Create the record. */
			tmp = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;

			/* Get the Usage Page and Usage for this device. */
			res = HidD_GetPreparsedData(write_handle, &pp_data);
			if (res) {
				nt_res = HidP_GetCaps(pp_data, &caps);
				if (nt_res == HIDP_STATUS_SUCCESS) {
					cur_dev->usage_page = caps.UsagePage;
					cur_dev->usage = caps.Usage;
				}
				HidD_FreePreparsedData(pp_data);
			}

			/* Fill out the record */
			cur_dev->next = NULL;
			str = device_interface_detail_data->DevicePath;
			if (str) {
				len = strlen(str);
				cur_dev->path = (char*) calloc(len+1, sizeof(char));
				strncpy(cur_dev->path, str, len+1);
				cur_dev->path[len] = '\0';
			}
			else
				cur_dev->path = NULL;

			/* Serial Number */
			wstr[0]= L'\0';
			res = HidD_GetSerialNumberString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = L'\0';
			cur_dev->serial_number = _wcsdup(wstr);

			/* Manufacturer String */
			wstr[0]= L'\0';
			res = HidD_GetManufacturerString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = L'\0';
			cur_dev->manufacturer_string = _wcsdup(wstr);

			/* Product String */
			wstr[0]= L'\0';
			res = HidD_GetProductString(write_handle, wstr, sizeof(wstr));
			wstr[WSTR_LEN-1] = L'\0';
			cur_dev->product_string = _wcsdup(wstr);

			/* VID/PID */
			cur_dev->vendor_id = attrib.VendorID;
			cur_dev->product_id = attrib.ProductID;

			/* Release Number */
			cur_dev->release_number = attrib.VersionNumber;

			/* Interface Number. It can sometimes be parsed out of the path
			   on Windows if a device has multiple interfaces. See
			   http://msdn.microsoft.com/en-us/windows/hardware/gg487473 or
			   search for "Hardware IDs for HID Devices" at MSDN. If it's not
			   in the path, it's set to -1. */
			cur_dev->interface_number = -1;
			if (cur_dev->path) {
				char *interface_component = strstr(cur_dev->path, "&mi_");
				if (interface_component) {
					char *hex_str = interface_component + 4;
					char *endptr = NULL;
					cur_dev->interface_number = strtol(hex_str, &endptr, 16);
					if (endptr == hex_str) {
						/* The parsing failed. Set interface_number to -1. */
						cur_dev->interface_number = -1;
					}
				}
			}
		}

cont_close:
		CloseHandle(write_handle);
cont:
		/* We no longer need the detail data. It can be freed */
		free(device_interface_detail_data);

		device_index++;

	}

	/* Close the device information handle. */
	SetupDiDestroyDeviceInfoList(device_info_set);

	return root;

}

void  HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info *devs)
{
	/* TODO: Merge this with the Linux version. This function is platform-independent. */
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}


HID_API_EXPORT hid_device * HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	/* TODO: Merge this functions with the Linux version. This function should be platform independent. */
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	devs = hid_enumerate(vendor_id, product_id);
	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (cur_dev->serial_number && wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	}

	hid_free_enumeration(devs);

	return handle;
}

HID_API_EXPORT hid_device * HID_API_CALL hid_open_path(const char *path)
{
	hid_device *dev;
	HIDP_CAPS caps;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	BOOLEAN res;
	NTSTATUS nt_res;

	if (hid_init() < 0) {
		return NULL;
	}

	dev = new_hid_device();

	/* Open a handle to the device */
	dev->device_handle = open_device(path, TRUE);

	/* Check validity of write_handle. */
	if (dev->device_handle == INVALID_HANDLE_VALUE) {
		/* System devices, such as keyboards and mice, cannot be opened in
		   read-write mode, because the system takes exclusive control over
		   them.  This is to prevent keyloggers.  However, feature reports
		   can still be sent and received.  Retry opening the device, but
		   without read/write access. */
		dev->device_handle = open_device(path, FALSE);

		/* Check the validity of the limited device_handle. */
		if (dev->device_handle == INVALID_HANDLE_VALUE) {
			/* Unable to open the device, even without read-write mode. */
			register_error(dev, "CreateFile");
			goto err;
		}
	}

	/* Set the Input Report buffer size to 64 reports. */
	res = HidD_SetNumInputBuffers(dev->device_handle, 64);
	if (!res) {
		register_error(dev, "HidD_SetNumInputBuffers");
		goto err;
	}

	/* Get the Input Report length for the device. */
	res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
	if (!res) {
		register_error(dev, "HidD_GetPreparsedData");
		goto err;
	}
	nt_res = HidP_GetCaps(pp_data, &caps);
	if (nt_res != HIDP_STATUS_SUCCESS) {
		register_error(dev, "HidP_GetCaps");
		goto err_pp_data;
	}
	dev->output_report_length = caps.OutputReportByteLength;
	dev->input_report_length = caps.InputReportByteLength;
	dev->feature_report_length = caps.FeatureReportByteLength;
	HidD_FreePreparsedData(pp_data);

	dev->read_buf = (char*) malloc(dev->input_report_length);

	return dev;

err_pp_data:
		HidD_FreePreparsedData(pp_data);
err:
		free_hid_device(dev);
		return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	DWORD bytes_written = 0;
	int function_result = -1;
	BOOL res;
	BOOL overlapped = FALSE;

	unsigned char *buf;

	if (!data || (length==0)) {
		register_error(dev, "Zero length buffer");
		return function_result;
	}

	/* Make sure the right number of bytes are passed to WriteFile. Windows
	   expects the number of bytes which are in the _longest_ report (plus
	   one for the report number) bytes even if the data is a report
	   which is shorter than that. Windows gives us this value in
	   caps.OutputReportByteLength. If a user passes in fewer bytes than this,
	   use cached temporary buffer which is the proper size. */
	if (length >= dev->output_report_length) {
		/* The user passed the right number of bytes. Use the buffer as-is. */
		buf = (unsigned char *) data;
	} else {
		if (dev->write_buf == NULL)
			dev->write_buf = (unsigned char *) malloc(dev->output_report_length);
		buf = dev->write_buf;
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->output_report_length - length);
		length = dev->output_report_length;
	}

	res = WriteFile(dev->device_handle, buf, (DWORD) length, NULL, &dev->write_ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* WriteFile() failed. Return error. */
			register_error(dev, "WriteFile");
			goto end_of_function;
		}
		overlapped = TRUE;
	}

	if (overlapped) {
		/* Wait for the transaction to complete. This makes
		   hid_write() synchronous. */
		res = WaitForSingleObject(dev->write_ol.hEvent, 1000);
		if (res != WAIT_OBJECT_0) {
			/* There was a Timeout. */
			register_error(dev, "WriteFile/WaitForSingleObject Timeout");
			goto end_of_function;
		}

		/* Get the result. */
		res = GetOverlappedResult(dev->device_handle, &dev->write_ol, &bytes_written, FALSE/*wait*/);
		if (res) {
			function_result = bytes_written;
		}
		else {
			/* The Write operation failed. */
			register_error(dev, "WriteFile");
			goto end_of_function;
		}
	}

end_of_function:
	return function_result;
}


int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	DWORD bytes_read = 0;
	size_t copy_len = 0;
	BOOL res = FALSE;
	BOOL overlapped = FALSE;

	/* Copy the handle for convenience. */
	HANDLE ev = dev->ol.hEvent;

	if (!dev->read_pending) {
		/* Start an Overlapped I/O read. */
		dev->read_pending = TRUE;
		memset(dev->read_buf, 0, dev->input_report_length);
		ResetEvent(ev);
		res = ReadFile(dev->device_handle, dev->read_buf, (DWORD) dev->input_report_length, &bytes_read, &dev->ol);

		if (!res) {
			if (GetLastError() != ERROR_IO_PENDING) {
				/* ReadFile() has failed.
				   Clean up and return error. */
				CancelIo(dev->device_handle);
				dev->read_pending = FALSE;
				goto end_of_function;
			}
			overlapped = TRUE;
		}
	}
	else {
		overlapped = TRUE;
	}

	if (overlapped) {
		if (milliseconds >= 0) {
			/* See if there is any data yet. */
			res = WaitForSingleObject(ev, milliseconds);
			if (res != WAIT_OBJECT_0) {
				/* There was no data this time. Return zero bytes available,
				   but leave the Overlapped I/O running. */
				return 0;
			}
		}

		/* Either WaitForSingleObject() told us that ReadFile has completed, or
		   we are in non-blocking mode. Get the number of bytes read. The actual
		   data has been copied to the data[] array which was passed to ReadFile(). */
		res = GetOverlappedResult(dev->device_handle, &dev->ol, &bytes_read, TRUE/*wait*/);
	}
	/* Set pending back to false, even if GetOverlappedResult() returned error. */
	dev->read_pending = FALSE;

	if (res && bytes_read > 0) {
		if (dev->read_buf[0] == 0x0) {
			/* If report numbers aren't being used, but Windows sticks a report
			   number (0x0) on the beginning of the report anyway. To make this
			   work like the other platforms, and to make it work more like the
			   HID spec, we'll skip over this byte. */
			bytes_read--;
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf+1, copy_len);
		}
		else {
			/* Copy the whole buffer, report number and all. */
			copy_len = length > bytes_read ? bytes_read : length;
			memcpy(data, dev->read_buf, copy_len);
		}
	}

end_of_function:
	if (!res) {
		register_error(dev, "GetOverlappedResult");
		return -1;
	}

	return (int) copy_len;
}

int HID_API_EXPORT HID_API_CALL hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device *dev, int nonblock)
{
	dev->blocking = !nonblock;
	return 0; /* Success */
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	BOOL res = FALSE;
	unsigned char *buf;
	size_t length_to_send;

	/* Windows expects at least caps.FeatureReportByteLength bytes passed
	   to HidD_SetFeature(), even if the report is shorter. Any less sent and
	   the function fails with error ERROR_INVALID_PARAMETER set. Any more
	   and HidD_SetFeature() silently truncates the data sent in the report
	   to caps.FeatureReportByteLength. */
	if (length >= dev->feature_report_length) {
		buf = (unsigned char *) data;
		length_to_send = length;
	} else {
		if (dev->feature_buf == NULL)
			dev->feature_buf = (unsigned char *) malloc(dev->feature_report_length);
		buf = dev->feature_buf;
		memcpy(buf, data, length);
		memset(buf + length, 0, dev->feature_report_length - length);
		length_to_send = dev->feature_report_length;
	}

	res = HidD_SetFeature(dev->device_handle, (PVOID)buf, (DWORD) length_to_send);

	if (!res) {
		register_error(dev, "HidD_SetFeature");
		return -1;
	}

	return (int) length;
}


int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	BOOL res;
#if 0
	res = HidD_GetFeature(dev->device_handle, data, length);
	if (!res) {
		register_error(dev, "HidD_GetFeature");
		return -1;
	}
	return 0; /* HidD_GetFeature() doesn't give us an actual length, unfortunately */
#else
	DWORD bytes_returned;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = DeviceIoControl(dev->device_handle,
		IOCTL_HID_GET_FEATURE,
		data, (DWORD) length,
		data, (DWORD) length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl() failed. Return error. */
			register_error(dev, "Send Feature Report DeviceIoControl");
			return -1;
		}
	}

	/* Wait here until the write is done. This makes
	   hid_get_feature_report() synchronous. */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* The operation failed. */
		register_error(dev, "Send Feature Report GetOverLappedResult");
		return -1;
	}

	return bytes_returned;
#endif
}


int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	BOOL res;
#if 0
	res = HidD_GetInputReport(dev->device_handle, data, length);
	if (!res) {
		register_error(dev, "HidD_GetInputReport");
		return -1;
	}
	return length;
#else
	DWORD bytes_returned;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = DeviceIoControl(dev->device_handle,
		IOCTL_HID_GET_INPUT_REPORT,
		data, (DWORD) length,
		data, (DWORD) length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl() failed. Return error. */
			register_error(dev, "Send Input Report DeviceIoControl");
			return -1;
		}
	}

	/* Wait here until the write is done. This makes
	   hid_get_feature_report() synchronous. */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* The operation failed. */
		register_error(dev, "Send Input Report GetOverLappedResult");
		return -1;
	}

	return bytes_returned;
#endif
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device *dev)
{
	if (!dev)
		return;
	CancelIo(dev->device_handle);
	free_hid_device(dev);
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetManufacturerString(dev->device_handle, string, sizeof(wchar_t) * (DWORD) MIN(maxlen, MAX_STRING_WCHARS));
	if (!res) {
		register_error(dev, "HidD_GetManufacturerString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetProductString(dev->device_handle, string, sizeof(wchar_t) * (DWORD) MIN(maxlen, MAX_STRING_WCHARS));
	if (!res) {
		register_error(dev, "HidD_GetProductString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetSerialNumberString(dev->device_handle, string, sizeof(wchar_t) * (DWORD) MIN(maxlen, MAX_STRING_WCHARS));
	if (!res) {
		register_error(dev, "HidD_GetSerialNumberString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	BOOL res;

	res = HidD_GetIndexedString(dev->device_handle, string_index, string, sizeof(wchar_t) * (DWORD) MIN(maxlen, MAX_STRING_WCHARS));
	if (!res) {
		register_error(dev, "HidD_GetIndexedString");
		return -1;
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device* dev, unsigned char* buf, size_t buf_size)
{
	BOOL res;
	PHIDP_PREPARSED_DATA pp_data = NULL;

	res = HidD_GetPreparsedData(dev->device_handle, &pp_data);
	if (!res) {
		register_error(dev, "HidD_GetPreparsedData");
		return -1;
	}
	else {
		struct rd_item_byte* byte_list = NULL;
		HIDP_CAPS caps;

		if (HidP_GetCaps(pp_data, &caps) != HIDP_STATUS_SUCCESS) {
			return -1;
		}

		// See: https://docs.microsoft.com/en-us/windows-hardware/drivers/hid/link-collections#ddk-link-collection-array-kg
		PHIDP_LINK_COLLECTION_NODE link_collection_nodes;
		link_collection_nodes = (PHIDP_LINK_COLLECTION_NODE)malloc(caps.NumberLinkCollectionNodes * sizeof(HIDP_LINK_COLLECTION_NODE));
		ULONG                     link_collection_nodes_len = caps.NumberLinkCollectionNodes;

		PHIDP_BUTTON_CAPS button_caps[NUM_OF_HIDP_REPORT_TYPES];
		USHORT button_caps_len[NUM_OF_HIDP_REPORT_TYPES];

		button_caps[HidP_Input] = (PHIDP_BUTTON_CAPS)malloc(caps.NumberInputButtonCaps * sizeof(HIDP_BUTTON_CAPS));
		button_caps_len[HidP_Input] = caps.NumberInputButtonCaps;
		button_caps[HidP_Output] = (PHIDP_BUTTON_CAPS)malloc(caps.NumberOutputButtonCaps * sizeof(HIDP_BUTTON_CAPS));
		button_caps_len[HidP_Output] = caps.NumberOutputButtonCaps;
		button_caps[HidP_Feature] = (PHIDP_BUTTON_CAPS)malloc(caps.NumberFeatureButtonCaps * sizeof(HIDP_BUTTON_CAPS));
		button_caps_len[HidP_Feature] = caps.NumberFeatureButtonCaps;


		PHIDP_VALUE_CAPS value_caps[NUM_OF_HIDP_REPORT_TYPES];
		USHORT value_caps_len[NUM_OF_HIDP_REPORT_TYPES];

		value_caps[HidP_Input] = (PHIDP_VALUE_CAPS)malloc(caps.NumberInputValueCaps * sizeof(HIDP_VALUE_CAPS));
		value_caps_len[HidP_Input] = caps.NumberInputValueCaps;
		value_caps[HidP_Output] = (PHIDP_VALUE_CAPS)malloc(caps.NumberOutputValueCaps * sizeof(HIDP_VALUE_CAPS));
		value_caps_len[HidP_Output] = caps.NumberOutputValueCaps;
		value_caps[HidP_Feature] = (PHIDP_VALUE_CAPS)malloc(caps.NumberFeatureValueCaps * sizeof(HIDP_VALUE_CAPS));
		value_caps_len[HidP_Feature] = caps.NumberFeatureValueCaps;

		ULONG max_datalist_len[NUM_OF_HIDP_REPORT_TYPES];

		if (HidP_GetLinkCollectionNodes(link_collection_nodes, &link_collection_nodes_len, pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetLinkCollectionNodes: Buffer to small");
		}
		else if ((button_caps_len[HidP_Input] != 0) && HidP_GetButtonCaps(HidP_Input, button_caps[HidP_Input], &button_caps_len[HidP_Input], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetButtonCaps: HidP_Input: The preparsed data is not valid. ");
		}
		else if ((button_caps_len[HidP_Output] != 0) && HidP_GetButtonCaps(HidP_Output, button_caps[HidP_Output], &button_caps_len[HidP_Output], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetButtonCaps: HidP_Output: The preparsed data is not valid. ");
		}
		else if ((button_caps_len[HidP_Feature] != 0) && HidP_GetButtonCaps(HidP_Feature, button_caps[HidP_Feature], &button_caps_len[HidP_Feature], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetButtonCaps: HidP_Feature: The preparsed data is not valid. ");
		}
		else if ((value_caps_len[HidP_Input] != 0) && HidP_GetValueCaps(HidP_Input, value_caps[HidP_Input], &value_caps_len[HidP_Input], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetValueCaps: HidP_Input: The preparsed data is not valid. ");
		}
		else if ((value_caps_len[HidP_Output] != 0) && HidP_GetValueCaps(HidP_Output, value_caps[HidP_Output], &value_caps_len[HidP_Output], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetValueCaps: HidP_Output: The preparsed data is not valid. ");
		}
		else if ((value_caps_len[HidP_Feature] != 0) && HidP_GetValueCaps(HidP_Feature, value_caps[HidP_Feature], &value_caps_len[HidP_Feature], pp_data) != HIDP_STATUS_SUCCESS) {
			//register_error(dev, "HidP_GetValueCaps: HidP_Feature: The preparsed data is not valid. ");
		}
		else {
			// All data read successfull
			max_datalist_len[HidP_Input] = caps.InputReportByteLength;
			max_datalist_len[HidP_Output] = caps.OutputReportByteLength;
			max_datalist_len[HidP_Feature] = caps.FeatureReportByteLength;


			// *************************************************************************************************************************
			// Create lookup tables for the bit range each report per collection (position of first bit and last bit in each collection)
			// [COLLECTION_INDEX][REPORT_ID][INPUT/OUTPUT/FEATURE]
			// *************************************************************************************************************************
			RD_BIT_RANGE**** coll_bit_range;
			coll_bit_range = malloc(link_collection_nodes_len * sizeof(*coll_bit_range));
			for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
				coll_bit_range[collection_node_idx] = malloc(256 * sizeof(coll_bit_range[0])); // 256 possible report IDs (incl. 0x00)
				for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
					coll_bit_range[collection_node_idx][reportid_idx] = malloc(NUM_OF_HIDP_REPORT_TYPES * sizeof(coll_bit_range[0][0]));
					for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
						coll_bit_range[collection_node_idx][reportid_idx][rt_idx] = malloc(sizeof(RD_BIT_RANGE));
						coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->FirstBit = -1;
						coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->LastBit = -1;
						// IsButton and CapIndex are not used in this lookup table
					}
				}
			}

			// Fill the lookup table where caps exist
			for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
				for (USHORT caps_idx = 0; caps_idx < button_caps_len[rt_idx]; caps_idx++) {
					int first_bit, last_bit, button_array_count, button_array_size;
					rd_determine_button_bitpositions(rt_idx, &button_caps[rt_idx][caps_idx], &first_bit, &last_bit, &button_array_count, &button_array_size, max_datalist_len[rt_idx], pp_data);
					if (coll_bit_range[button_caps[rt_idx][caps_idx].LinkCollection][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit == -1 ||
						coll_bit_range[button_caps[rt_idx][caps_idx].LinkCollection][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit > first_bit) {
						coll_bit_range[button_caps[rt_idx][caps_idx].LinkCollection][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit = first_bit;
					}
					if (coll_bit_range[button_caps[rt_idx][caps_idx].LinkCollection][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->LastBit < last_bit) {
						coll_bit_range[button_caps[rt_idx][caps_idx].LinkCollection][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->LastBit = last_bit;
					}
				}
				for (USHORT caps_idx = 0; caps_idx < value_caps_len[rt_idx]; caps_idx++) {
					int first_bit, last_bit;
					rd_determine_value_bitpositions(rt_idx, &value_caps[rt_idx][caps_idx], &first_bit, &last_bit, max_datalist_len[rt_idx], pp_data);
					if (coll_bit_range[value_caps[rt_idx][caps_idx].LinkCollection][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit == -1 ||
						coll_bit_range[value_caps[rt_idx][caps_idx].LinkCollection][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit > first_bit) {
						coll_bit_range[value_caps[rt_idx][caps_idx].LinkCollection][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit = first_bit;
					}
					if (coll_bit_range[value_caps[rt_idx][caps_idx].LinkCollection][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->LastBit < last_bit) {
						coll_bit_range[value_caps[rt_idx][caps_idx].LinkCollection][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->LastBit = last_bit;
					}
				}
			}

			// *****************************************************
			// Determine hierachy levels of collections
			// Determine number of direct childs of each collections
			// *****************************************************
			int max_coll_level = 0;
			int* coll_levels;
			coll_levels = malloc(link_collection_nodes_len * sizeof(coll_levels[0]));
			int* coll_number_of_direct_childs;
			coll_number_of_direct_childs = malloc(link_collection_nodes_len * sizeof(coll_number_of_direct_childs[0]));
			for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
				coll_levels[collection_node_idx] = -1;
				coll_number_of_direct_childs[collection_node_idx] = 0;
			}

			{
				int actual_coll_level = 0;
				USHORT collection_node_idx = 0;
				while (actual_coll_level >= 0) {
					coll_levels[collection_node_idx] = actual_coll_level;
					if ((link_collection_nodes[collection_node_idx].NumberOfChildren > 0) &&
						(coll_levels[link_collection_nodes[collection_node_idx].FirstChild] == -1)) {
						actual_coll_level++;
						coll_levels[collection_node_idx] = actual_coll_level;
						if (max_coll_level < actual_coll_level) {
							max_coll_level = actual_coll_level;
						}
						coll_number_of_direct_childs[collection_node_idx]++;
						collection_node_idx = link_collection_nodes[collection_node_idx].FirstChild;
					}
					else if (link_collection_nodes[collection_node_idx].NextSibling != 0) {
						coll_number_of_direct_childs[link_collection_nodes[collection_node_idx].Parent]++;
						collection_node_idx = link_collection_nodes[collection_node_idx].NextSibling;
					}
					else {
						actual_coll_level--;
						if (actual_coll_level >= 0) {
							collection_node_idx = link_collection_nodes[collection_node_idx].Parent;
						}
					}
				}
			}

			// *********************************************************************************
			// Propagate the bit range of each report from the child collections to their parent
			// and store the merged result for the parent
			// *********************************************************************************
			for (int actual_coll_level = max_coll_level - 1; actual_coll_level >= 0; actual_coll_level--) {
				for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
					if (coll_levels[collection_node_idx] == actual_coll_level) {
						USHORT child_idx = link_collection_nodes[collection_node_idx].FirstChild;
						while (child_idx) {
							for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
								for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
									// Merge bit range from childs
									if ((coll_bit_range[child_idx][reportid_idx][rt_idx]->FirstBit != -1) &&
										(coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->FirstBit > coll_bit_range[child_idx][reportid_idx][rt_idx]->FirstBit)) {
										coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->FirstBit = coll_bit_range[child_idx][reportid_idx][rt_idx]->FirstBit;
									}
									if (coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->LastBit < coll_bit_range[child_idx][reportid_idx][rt_idx]->LastBit) {
										coll_bit_range[collection_node_idx][reportid_idx][rt_idx]->LastBit = coll_bit_range[child_idx][reportid_idx][rt_idx]->LastBit;
									}
									child_idx = link_collection_nodes[child_idx].NextSibling;
								}
							}
						}
					}
				}
			}

			// ************************************************************************************************
			// Determine child collection order of the whole hierachy based on previously determined bit ranges
			// ************************************************************************************************
			int** coll_child_order;
			coll_child_order = malloc(link_collection_nodes_len * sizeof(*coll_child_order));
			{
				BOOLEAN* coll_parsed_flag;
				coll_parsed_flag = malloc(link_collection_nodes_len * sizeof(coll_parsed_flag[0]));
				for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
					coll_parsed_flag[collection_node_idx] = FALSE;
				}
				int actual_coll_level = 0;
				USHORT collection_node_idx = 0;
				while (actual_coll_level >= 0) {
					if ((coll_number_of_direct_childs[collection_node_idx] != 0) &&
						(coll_parsed_flag[link_collection_nodes[collection_node_idx].FirstChild] == FALSE)) {
						coll_parsed_flag[link_collection_nodes[collection_node_idx].FirstChild] = TRUE;
						coll_child_order[collection_node_idx] = malloc((coll_number_of_direct_childs[collection_node_idx]) * sizeof(coll_child_order[0]));

						{
							// Create list of child collection indices
							// sorted reverse to the order returned to HidP_GetLinkCollectionNodeschild
							// which seems to match teh original order, as long as no bit position needs to be considered
							USHORT child_idx = link_collection_nodes[collection_node_idx].FirstChild;
							int child_count = coll_number_of_direct_childs[collection_node_idx] - 1;
							coll_child_order[collection_node_idx][child_count] = child_idx;
							while (link_collection_nodes[child_idx].NextSibling) {
								child_count--;
								child_idx = link_collection_nodes[child_idx].NextSibling;
								coll_child_order[collection_node_idx][child_count] = child_idx;
							}
						}

						if (coll_number_of_direct_childs[collection_node_idx] > 1) {
							// Sort child collections indices by bit positions
							for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
								for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
									for (int child_idx = 1; child_idx < coll_number_of_direct_childs[collection_node_idx]; child_idx++) {
										if ((coll_bit_range[child_idx - 1][reportid_idx][rt_idx]->FirstBit != -1) &&
											(coll_bit_range[child_idx][reportid_idx][rt_idx]->FirstBit != -1) &&
											(coll_bit_range[child_idx - 1][reportid_idx][rt_idx]->FirstBit > coll_bit_range[child_idx][reportid_idx][rt_idx]->FirstBit)) {
											// Swap position indices of the two compared child collections
											int idx_latch = coll_child_order[collection_node_idx][child_idx - 1];
											coll_child_order[collection_node_idx][child_idx - 1] = coll_child_order[collection_node_idx][child_idx];
											coll_child_order[collection_node_idx][child_idx] = idx_latch;
										}
									}
								}
							}
						}
						actual_coll_level++;
						collection_node_idx = link_collection_nodes[collection_node_idx].FirstChild;
					}
					else if (link_collection_nodes[collection_node_idx].NextSibling != 0) {
						collection_node_idx = link_collection_nodes[collection_node_idx].NextSibling;
					}
					else {
						actual_coll_level--;
						if (actual_coll_level >= 0) {
							collection_node_idx = link_collection_nodes[collection_node_idx].Parent;
						}
					}
				}
				free(coll_parsed_flag);
			}


			// *****************************************************************************
			// Create sorted list containing all the Collection and CollectionEnd main items
			// *****************************************************************************
			struct rd_main_item_node* main_item_list;
			main_item_list = (struct rd_main_item_node*)malloc(sizeof(main_item_list));
			main_item_list = NULL; // List root
			// Lookup table to find the Collection items in the list by index
			struct rd_main_item_node** coll_begin_lookup;
			struct rd_main_item_node** coll_end_lookup;
			coll_begin_lookup = malloc(link_collection_nodes_len * sizeof(*coll_begin_lookup));
			coll_end_lookup = malloc(link_collection_nodes_len * sizeof(*coll_end_lookup));
			{
				int* coll_last_written_child;
				coll_last_written_child = malloc(link_collection_nodes_len * sizeof(coll_last_written_child[0]));
				for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
					coll_last_written_child[collection_node_idx] = -1;
				}

				int actual_coll_level = 0;
				USHORT collection_node_idx = 0;
				coll_begin_lookup[0] = rd_append_main_item_node(0, 0, -1, -1, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
				while (actual_coll_level >= 0) {
					if ((coll_number_of_direct_childs[collection_node_idx] != 0) &&
						(coll_last_written_child[collection_node_idx] == -1)) {
						coll_last_written_child[collection_node_idx] = coll_child_order[collection_node_idx][0];
						collection_node_idx = coll_child_order[collection_node_idx][0];
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, -1, -1, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
						actual_coll_level++;


					}
					else if ((coll_number_of_direct_childs[collection_node_idx] > 1) &&
						(coll_last_written_child[collection_node_idx] != coll_child_order[collection_node_idx][coll_number_of_direct_childs[collection_node_idx] - 1])) {
						int nextChild = 1;
						while (coll_last_written_child[collection_node_idx] != coll_child_order[collection_node_idx][nextChild - 1]) {
							nextChild++;
						}
						coll_last_written_child[collection_node_idx] = coll_child_order[collection_node_idx][nextChild];
						collection_node_idx = coll_child_order[collection_node_idx][nextChild];
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, -1, -1, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
						actual_coll_level++;
					}
					else {
						actual_coll_level--;
						coll_end_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, -1, -1, rd_item_node_collection, 0, collection_node_idx, rd_collection_end, 0, &main_item_list);
						collection_node_idx = link_collection_nodes[collection_node_idx].Parent;
					}
				}
				free(coll_last_written_child);
			}


			// ******************************************************
			// Inserted Input/Output/Feature main items into the list
			// in order of reconstructed bit positions
			// ******************************************************
			for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
				// Add all button caps to node list
				for (USHORT caps_idx = 0; caps_idx < button_caps_len[rt_idx]; caps_idx++) {
					struct rd_main_item_node* coll_begin = coll_begin_lookup[button_caps[rt_idx][caps_idx].LinkCollection];
					int first_bit, last_bit, button_array_count, button_array_size;
					rd_determine_button_bitpositions(rt_idx, &button_caps[rt_idx][caps_idx], &first_bit, &last_bit, &button_array_count, &button_array_size, max_datalist_len[rt_idx], pp_data);

					for (int child_idx = 0; child_idx < coll_number_of_direct_childs[button_caps[rt_idx][caps_idx].LinkCollection]; child_idx++) {
						// Determine in which section before/between/after child collection the item should be inserted
						if (first_bit < coll_bit_range[coll_child_order[button_caps[rt_idx][caps_idx].LinkCollection][child_idx]][button_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit)
						{
							// Note, that the default value for undefined coll_bit_range is -1, which can't be greater than the bit position
							break;
						}
						coll_begin = coll_end_lookup[coll_child_order[button_caps[rt_idx][caps_idx].LinkCollection][child_idx]];
					}

					rd_insert_main_item_node(first_bit, first_bit, last_bit, button_array_count, button_array_size, rd_item_node_button, caps_idx, button_caps[rt_idx][caps_idx].LinkCollection, rt_idx, button_caps[rt_idx][caps_idx].ReportID, &coll_begin);
				}
				// Add all value caps to node list
				for (USHORT caps_idx = 0; caps_idx < value_caps_len[rt_idx]; caps_idx++) {
					struct rd_main_item_node* coll_begin = coll_begin_lookup[value_caps[rt_idx][caps_idx].LinkCollection];
					int first_bit, last_bit;
					rd_determine_value_bitpositions(rt_idx, &value_caps[rt_idx][caps_idx], &first_bit, &last_bit, max_datalist_len[rt_idx], pp_data);

					for (int child_idx = 0; child_idx < coll_number_of_direct_childs[value_caps[rt_idx][caps_idx].LinkCollection]; child_idx++) {
						// Determine in which section before/between/after child collection the item should be inserted
						if (first_bit < coll_bit_range[coll_child_order[value_caps[rt_idx][caps_idx].LinkCollection][child_idx]][value_caps[rt_idx][caps_idx].ReportID][rt_idx]->FirstBit)
						{
							// Note, that the default value for undefined coll_bit_range is -1, which cant be greater than the bit position
							break;
						}
						coll_begin = coll_end_lookup[coll_child_order[value_caps[rt_idx][caps_idx].LinkCollection][child_idx]];
					}
					rd_insert_main_item_node(first_bit, first_bit, last_bit, -1, -1, rd_item_node_value, caps_idx, value_caps[rt_idx][caps_idx].LinkCollection, rt_idx, value_caps[rt_idx][caps_idx].ReportID, &coll_begin);
				}
			}



			// ***********************************************************************
			// Add const items for all bit gaps and at the report end for 8bit padding
			// ***********************************************************************
			{
				int last_bit_position[NUM_OF_HIDP_REPORT_TYPES][256];
				struct rd_main_item_node* last_report_item_lookup[NUM_OF_HIDP_REPORT_TYPES][256];
				for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
					for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
						last_bit_position[rt_idx][reportid_idx] = -1;
						last_report_item_lookup[rt_idx][reportid_idx] = NULL;
					}
				}

				struct rd_main_item_node* list;
				list = (struct rd_main_item_node*)malloc(sizeof(main_item_list));
				list = main_item_list; // List root

				while (list->next != NULL)
				{
					if ((list->MainItemType >= rd_input) &&
						(list->MainItemType <= rd_feature)) {
						// INPUT, OUTPUT or FEATURE
						if (list->FirstBit != -1) {
							if (last_bit_position[list->MainItemType][list->ReportID] + 1 != list->FirstBit) {
								rd_insert_main_item_node(last_bit_position[list->MainItemType][list->ReportID], last_bit_position[list->MainItemType][list->ReportID], list->FirstBit - 1, -1, -1, rd_item_node_padding, -1, 0, list->MainItemType, list->ReportID, &last_report_item_lookup[list->MainItemType][list->ReportID]);
							}
							last_bit_position[list->MainItemType][list->ReportID] = list->LastBit;
							last_report_item_lookup[list->MainItemType][list->ReportID] = list;
						}
					}
					list = list->next;
				}
				// Add 8 bit padding at each report end
				for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
					for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
						if (last_bit_position[rt_idx][reportid_idx] != -1) {
							int padding = 8 - ((last_bit_position[rt_idx][reportid_idx] + 1) % 8);
							if (padding < 8) {
								rd_insert_main_item_node(last_bit_position[rt_idx][reportid_idx], last_bit_position[rt_idx][reportid_idx], last_bit_position[rt_idx][reportid_idx] + padding, -1, -1, rd_item_node_padding, -1, 0, rt_idx, reportid_idx, &last_report_item_lookup[rt_idx][reportid_idx]);
							}
						}
					}
				}
			}





			// ***********************************
			// Encode the report descriptor output
			// ***********************************
			UCHAR last_report_id = 0;
			USAGE last_usage_page = 0;
			LONG last_physical_min = 0;// If both, Physical Minimum and Physical Maximum are 0, the logical limits should be taken as physical limits according USB HID spec 1.11 chapter 6.2.2.7
			LONG last_physical_max = 0;
			ULONG last_unit_exponent = 0; // If Unit Exponent is Undefined it should be considered as 0 according USB HID spec 1.11 chapter 6.2.2.7
			ULONG last_unit = 0; // If the first nibble is 7, or second nibble of Unit is 0, the unit is None according USB HID spec 1.11 chapter 6.2.2.7
			int report_count = 0;
			printf("\n");
			while (main_item_list != NULL)
			{
				int rt_idx = main_item_list->MainItemType;
				int	caps_idx = main_item_list->CapsIndex;
				UCHAR report_id = main_item_list->ReportID;
				if (main_item_list->MainItemType == rd_collection) {
					if (last_usage_page != link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage) {
						rd_write_short_item(rd_global_usage_page, link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage, &byte_list);
						printf("Usage Page (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage);
						last_usage_page = link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage;
					}
					rd_write_short_item(rd_local_usage, link_collection_nodes[main_item_list->CollectionIndex].LinkUsage, &byte_list);
					printf("Usage  (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].LinkUsage);
					if (link_collection_nodes[main_item_list->CollectionIndex].CollectionType == 0) {
						rd_write_short_item(rd_main_collection, 0x00, &byte_list);
						printf("Collection (Physical)\n");
					}
					else if (link_collection_nodes[main_item_list->CollectionIndex].CollectionType == 1) {
						rd_write_short_item(rd_main_collection, 0x01, &byte_list);
						printf("Collection (Application)\n");
					}
					else if (link_collection_nodes[main_item_list->CollectionIndex].CollectionType == 2) {
						rd_write_short_item(rd_main_collection, 0x02, &byte_list);
						printf("Collection (Logical)\n");
					}
					else {
						printf("Collection (nnn)\n");
					}
				}
				else if (main_item_list->MainItemType == rd_collection_end) {
					rd_write_short_item(rd_main_collection_end, 0, &byte_list);
					printf("End Collection\n");
				}
				else if (main_item_list->TypeOfNode == rd_item_node_padding) {
					// Padding 

					rd_write_short_item(rd_global_report_size, (main_item_list->LastBit - main_item_list->FirstBit), &byte_list);
					printf("Report Size (%d)\n", (main_item_list->LastBit - main_item_list->FirstBit));

					rd_write_short_item(rd_global_report_count, 1, &byte_list);
					printf("Report Count (%d)\n", 1);

					if (rt_idx == HidP_Input) {
						rd_write_short_item(rd_main_input, 0x03, &byte_list); // Const / Abs
						printf("Input (0x%02X) Padding\n", 0x03);
					}
					else if (rt_idx == HidP_Output) {
						rd_write_short_item(rd_main_output, 0x03, &byte_list); // Const / Abs
						printf("Output (0x%02X) Padding\n", 0x03);
					}
					else if (rt_idx == HidP_Feature) {
						rd_write_short_item(rd_main_feature, 0x03, &byte_list); // Const / Abs
						printf("Feature (0x%02X) Padding\n", 0x03);
					}
					report_count = 0;
				}
				else if (main_item_list->TypeOfNode == rd_item_node_button) {
					// Button
					if (last_report_id != button_caps[rt_idx][caps_idx].ReportID) {
						// Write Report ID if changed
						rd_write_short_item(rd_global_report_id, button_caps[rt_idx][caps_idx].ReportID, &byte_list);
						printf("Report ID (%d)\n", button_caps[rt_idx][caps_idx].ReportID);
						last_report_id = button_caps[rt_idx][caps_idx].ReportID;
					}

					// Print usage page when changed
					if (button_caps[rt_idx][caps_idx].UsagePage != last_usage_page) {
						rd_write_short_item(rd_global_usage_page, button_caps[rt_idx][caps_idx].UsagePage, &byte_list);
						printf("Usage Page (%d)\n", button_caps[rt_idx][caps_idx].UsagePage);
						last_usage_page = button_caps[rt_idx][caps_idx].UsagePage;
					}

					// Print only local report items for each cap, if ReportCount > 1
					if (button_caps[rt_idx][caps_idx].IsRange) {
						report_count += (button_caps[rt_idx][caps_idx].Range.DataIndexMax - button_caps[rt_idx][caps_idx].Range.DataIndexMin);
						rd_write_short_item(rd_local_usage_minimum, button_caps[rt_idx][caps_idx].Range.UsageMin, &byte_list);
						printf("Usage Minimum (%d)\n", button_caps[rt_idx][caps_idx].Range.UsageMin);
						rd_write_short_item(rd_local_usage_maximum, button_caps[rt_idx][caps_idx].Range.UsageMax, &byte_list);
						printf("Usage Maximum (%d)\n", button_caps[rt_idx][caps_idx].Range.UsageMax);
					}
					else {
						rd_write_short_item(rd_local_usage, button_caps[rt_idx][caps_idx].NotRange.Usage, &byte_list);
						printf("Usage (%d)\n", button_caps[rt_idx][caps_idx].NotRange.Usage);
					}
					// EXPERIMENTAL - No device available for test
					NTSTATUS status;
					ULONG data[10];
					HIDP_EXTENDED_ATTRIBUTES attribs;
					attribs.Data[0] = data[0];
					ULONG attrib_len = sizeof(HIDP_EXTENDED_ATTRIBUTES);
					status = HidP_GetExtendedAttributes(rt_idx, button_caps[rt_idx][caps_idx].NotRange.DataIndex, pp_data, &attribs, &attrib_len);
					if (attribs.NumGlobalUnknowns > 0) {
						printf("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx Found Global HID items unknown by Windows hidpi - these are now stored in HIDP_EXTENDED_ATTRIBUTES structure");
					}
					// EXPERIMENTAL - No device available for test

					if ((main_item_list->next != NULL) &&
						(main_item_list->next->MainItemType == rt_idx) &&
						(main_item_list->next->TypeOfNode == rd_item_node_button) &&
						(main_item_list->next->ButtonArrayCount == -1) && // Next node in list is no Button array
						(button_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].UsagePage == button_caps[rt_idx][caps_idx].UsagePage) &&
						(button_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].ReportID == button_caps[rt_idx][caps_idx].ReportID) &&
						(button_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].BitField == button_caps[rt_idx][caps_idx].BitField)
						) {
						// Skip global items until any of them changes, than use ReportCount item to write the count of identical report fields
						report_count++;
					}
					else {

						if (main_item_list->ButtonArrayCount == -1) {
							// Variable bit field with one bit per button

							rd_write_short_item(rd_global_logical_minimum, 0, &byte_list);
							printf("Logical Minimum (%d)\n", 0);

							rd_write_short_item(rd_global_logical_maximum, 1, &byte_list);
							printf("Logical Maximum (%d)\n", 1);

							rd_write_short_item(rd_global_report_size, 1, &byte_list);
							printf("Report Size (%d)\n", 1);

							rd_write_short_item(rd_global_report_count, 1 + report_count, &byte_list);
							printf("Report Count (%d)\n", 1 + report_count);
						}
						else {
							// Button array of Report Size x Report Count
							rd_write_short_item(rd_global_logical_minimum, button_caps[rt_idx][caps_idx].Range.UsageMin, &byte_list);
							printf("Logical Minimum (%d)\n", button_caps[rt_idx][caps_idx].Range.UsageMin);

							rd_write_short_item(rd_global_logical_maximum, button_caps[rt_idx][caps_idx].Range.UsageMax, &byte_list);
							printf("Logical Maximum (%d)\n", button_caps[rt_idx][caps_idx].Range.UsageMax);

							rd_write_short_item(rd_global_report_size, main_item_list->ButtonArraySize, &byte_list);
							printf("Report Size (%d)\n", main_item_list->ButtonArraySize);

							rd_write_short_item(rd_global_report_count, main_item_list->ButtonArrayCount, &byte_list);
							printf("Report Count (%d)\n", main_item_list->ButtonArrayCount);
						}


						// Buttons have only 1 bit and therefore no physical limits/units -> Set to undefined state

						if (last_physical_min != 0) {
							// Write Physical Min only if changed
							last_physical_min = 0;
							rd_write_short_item(rd_global_physical_minimum, last_physical_min, &byte_list);
							printf("Physical Minimum (%d)\n", last_physical_min);
						}

						if (last_physical_max != 0) {
							// Write Physical Max only if changed
							last_physical_max = 0;
							rd_write_short_item(rd_global_physical_maximum, last_physical_max, &byte_list);
							printf("Physical Maximum (%d)\n", last_physical_max);
						}

						if (last_unit_exponent != 0) {
							// Write Unit Exponent only if changed
							last_unit_exponent = 0;
							rd_write_short_item(rd_global_unit_exponent, last_unit_exponent, &byte_list);
							printf("Unit Exponent (%d)\n", last_unit_exponent);
						}

						if (last_unit != 0) {
							// Write Unit only if changed
							last_unit = 0;
							rd_write_short_item(rd_global_unit, last_unit, &byte_list);
							printf("Unit (%d)\n", last_unit);
						}


						if (rt_idx == HidP_Input) {
							rd_write_short_item(rd_main_input, button_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Input (0x%02X)\n", button_caps[rt_idx][caps_idx].BitField);
						}
						else if (rt_idx == HidP_Output) {
							rd_write_short_item(rd_main_output, button_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Output (0x%02X)\n", button_caps[rt_idx][caps_idx].BitField);
						}
						else if (rt_idx == HidP_Feature) {
							rd_write_short_item(rd_main_feature, button_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Feature (0x%02X)\n", button_caps[rt_idx][caps_idx].BitField);
						}
						report_count = 0;
					}
				}
				else {
					// EXPERIMENTAL - No device available for test
					NTSTATUS status;
					ULONG data[10];
					HIDP_EXTENDED_ATTRIBUTES attribs;
					attribs.Data[0] = data[0];
					ULONG attrib_len = sizeof(HIDP_EXTENDED_ATTRIBUTES);
					status = HidP_GetExtendedAttributes(rt_idx, value_caps[rt_idx][caps_idx].NotRange.DataIndex, pp_data, &attribs, &attrib_len);
					if (attribs.NumGlobalUnknowns > 0) {
						printf("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx Found Global HID items unknown by Windows hidpi - these are now stored in HIDP_EXTENDED_ATTRIBUTES structure");
					}
					// EXPERIMENTAL - No device available for test

					if (last_report_id != value_caps[rt_idx][caps_idx].ReportID) {
						// Write Report ID if changed
						rd_write_short_item(rd_global_report_id, value_caps[rt_idx][caps_idx].ReportID, &byte_list);
						printf("Report ID (%d)\n", value_caps[rt_idx][caps_idx].ReportID);
						last_report_id = value_caps[rt_idx][caps_idx].ReportID;
					}

					// Print usage page when changed
					if (value_caps[rt_idx][caps_idx].UsagePage != last_usage_page) {
						rd_write_short_item(rd_global_usage_page, value_caps[rt_idx][caps_idx].UsagePage, &byte_list);
						printf("Usage Page (%d)\n", value_caps[rt_idx][caps_idx].UsagePage);
						last_usage_page = value_caps[rt_idx][caps_idx].UsagePage;
					}


					if (value_caps[rt_idx][caps_idx].IsRange) {
						rd_write_short_item(rd_local_usage_minimum, value_caps[rt_idx][caps_idx].Range.UsageMin, &byte_list);
						printf("Usage Minimum (%d)\n", value_caps[rt_idx][caps_idx].Range.UsageMin);
						rd_write_short_item(rd_local_usage_maximum, value_caps[rt_idx][caps_idx].Range.UsageMax, &byte_list);
						printf("Usage Maximum (%d)\n", value_caps[rt_idx][caps_idx].Range.UsageMax);
					}
					else {
						rd_write_short_item(rd_local_usage, value_caps[rt_idx][caps_idx].NotRange.Usage, &byte_list);
						printf("Usage (%d)\n", value_caps[rt_idx][caps_idx].NotRange.Usage);
					}

					if ((value_caps[rt_idx][caps_idx].BitField & 0x02) != 0x02) {
						// In case of an value array overwrite Report Count
						value_caps[rt_idx][caps_idx].ReportCount = value_caps[rt_idx][caps_idx].Range.DataIndexMax - value_caps[rt_idx][caps_idx].Range.DataIndexMin + 1;
					}

					// Print only local report items for each cap, if ReportCount > 1
					if ((main_item_list->next != NULL) &&
						(main_item_list->next->MainItemType == rt_idx) &&
						(main_item_list->next->TypeOfNode == rd_item_node_value) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].UsagePage == value_caps[rt_idx][caps_idx].UsagePage) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].LogicalMin == value_caps[rt_idx][caps_idx].LogicalMin) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].LogicalMax == value_caps[rt_idx][caps_idx].LogicalMax) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].PhysicalMin == value_caps[rt_idx][caps_idx].PhysicalMin) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].PhysicalMax == value_caps[rt_idx][caps_idx].PhysicalMax) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].UnitsExp == value_caps[rt_idx][caps_idx].UnitsExp) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].Units == value_caps[rt_idx][caps_idx].Units) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].BitSize == value_caps[rt_idx][caps_idx].BitSize) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].ReportID == value_caps[rt_idx][caps_idx].ReportID) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].BitField == value_caps[rt_idx][caps_idx].BitField) &&
						(value_caps[main_item_list->next->MainItemType][main_item_list->next->CapsIndex].ReportCount == 1) &&
						(value_caps[rt_idx][caps_idx].ReportCount == 1)
						) {
						// Skip global items until any of them changes, than use ReportCount item to write the count of identical report fields
						report_count++;
					}
					else {

						rd_write_short_item(rd_global_logical_minimum, value_caps[rt_idx][caps_idx].LogicalMin, &byte_list);
						printf("Logical Minimum (%d)\n", value_caps[rt_idx][caps_idx].LogicalMin);

						rd_write_short_item(rd_global_logical_maximum, value_caps[rt_idx][caps_idx].LogicalMax, &byte_list);
						printf("Logical Maximum (%d)\n", value_caps[rt_idx][caps_idx].LogicalMax);

						if ((last_physical_min != value_caps[rt_idx][caps_idx].PhysicalMin) ||
							(last_physical_max != value_caps[rt_idx][caps_idx].PhysicalMax)) {
							// Write Physical Min and Max only if one of them changed
							rd_write_short_item(rd_global_physical_minimum, value_caps[rt_idx][caps_idx].PhysicalMin, &byte_list);
							printf("Physical Minimum (%d)\n", value_caps[rt_idx][caps_idx].PhysicalMin);
							last_physical_min = value_caps[rt_idx][caps_idx].PhysicalMin;

							rd_write_short_item(rd_global_physical_maximum, value_caps[rt_idx][caps_idx].PhysicalMax, &byte_list);
							printf("Physical Maximum (%d)\n", value_caps[rt_idx][caps_idx].PhysicalMax);
							last_physical_max = value_caps[rt_idx][caps_idx].PhysicalMax;
						}


						if (last_unit_exponent != value_caps[rt_idx][caps_idx].UnitsExp) {
							// Write Unit Exponent only if changed
							rd_write_short_item(rd_global_unit_exponent, value_caps[rt_idx][caps_idx].UnitsExp, &byte_list);
							printf("Unit Exponent (%d)\n", value_caps[rt_idx][caps_idx].UnitsExp);
							last_unit_exponent = value_caps[rt_idx][caps_idx].UnitsExp;
						}

						if (last_unit != value_caps[rt_idx][caps_idx].Units) {
							// Write Unit only if changed
							rd_write_short_item(rd_global_unit, value_caps[rt_idx][caps_idx].Units, &byte_list);
							printf("Unit (%d)\n", value_caps[rt_idx][caps_idx].Units);
							last_unit = value_caps[rt_idx][caps_idx].Units;
						}

						rd_write_short_item(rd_global_report_size, value_caps[rt_idx][caps_idx].BitSize, &byte_list);
						printf("Report Size (%d)\n", value_caps[rt_idx][caps_idx].BitSize);

						rd_write_short_item(rd_global_report_count, value_caps[rt_idx][caps_idx].ReportCount + report_count, &byte_list);
						printf("Report Count (%d)\n", value_caps[rt_idx][caps_idx].ReportCount + report_count);

						if (rt_idx == HidP_Input) {
							rd_write_short_item(rd_main_input, value_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Input (0x%02X)\n", value_caps[rt_idx][caps_idx].BitField);
						}
						else if (rt_idx == HidP_Output) {
							rd_write_short_item(rd_main_output, value_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Output (0x%02X)\n", value_caps[rt_idx][caps_idx].BitField);
						}
						else if (rt_idx == HidP_Feature) {
							rd_write_short_item(rd_main_feature, value_caps[rt_idx][caps_idx].BitField, &byte_list);
							printf("Feature (0x%02X)\n", value_caps[rt_idx][caps_idx].BitField);
						}
						report_count = 0;
					}
				}
				main_item_list = main_item_list->next;
			}



			for (USHORT collection_node_idx = 0; collection_node_idx < link_collection_nodes_len; collection_node_idx++) {
				for (int reportid_idx = 0; reportid_idx < 256; reportid_idx++) {
					for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
						free(coll_bit_range[collection_node_idx][reportid_idx][rt_idx]);
					}
					free(coll_bit_range[collection_node_idx][reportid_idx]);
				}
				free(coll_bit_range[collection_node_idx]);
				free(coll_begin_lookup[collection_node_idx]);
				free(coll_end_lookup[collection_node_idx]);
			}
			free(coll_bit_range);
			free(coll_begin_lookup);
			free(coll_end_lookup);
		}


		// Copy report temporary descriptor list into buf array
		unsigned int byte_list_len = 0;

		while ((byte_list != NULL))
		{
			if (byte_list_len < buf_size) {
				// In case of too small buf size, just inhibt write to buffer,
				// to ensure proper free of list memory
				*(buf + byte_list_len) = (unsigned char)byte_list->byte;
			}
			byte_list_len++;
			struct rd_item_byte* byte_list_prev = byte_list;
			byte_list = byte_list->next;
			free(byte_list_prev);
		}
		
		// Free allocated memory
		for (HIDP_REPORT_TYPE rt_idx = 0; rt_idx < NUM_OF_HIDP_REPORT_TYPES; rt_idx++) {
			free(button_caps[rt_idx]);
			free(value_caps[rt_idx]);
		}
		free(link_collection_nodes);

		HidD_FreePreparsedData(pp_data);

		if (byte_list_len > buf_size) {
			return -1;
		}
		else {
			return byte_list_len;
		}
	}
}

HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->last_error_str == NULL)
			return L"Success";
		return (wchar_t*)dev->last_error_str;
	}

	// Global error messages are not (yet) implemented on Windows.
	return L"hid_error for global errors is not implemented yet";
}


/*#define PICPGM*/
/*#define S11*/
#define P32
#ifdef S11
  unsigned short VendorID = 0xa0a0;
	unsigned short ProductID = 0x0001;
#endif

#ifdef P32
  unsigned short VendorID = 0x04d8;
	unsigned short ProductID = 0x3f;
#endif


#ifdef PICPGM
  unsigned short VendorID = 0x04d8;
  unsigned short ProductID = 0x0033;
#endif


#if 0
int __cdecl main(int argc, char* argv[])
{
	int res;
	unsigned char buf[65];

	UNREFERENCED_PARAMETER(argc);
	UNREFERENCED_PARAMETER(argv);

	/* Set up the command buffer. */
	memset(buf,0x00,sizeof(buf));
	buf[0] = 0;
	buf[1] = 0x81;


	/* Open the device. */
	int handle = open(VendorID, ProductID, L"12345");
	if (handle < 0)
		printf("unable to open device\n");


	/* Toggle LED (cmd 0x80) */
	buf[1] = 0x80;
	res = write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write()\n");

	/* Request state (cmd 0x81) */
	buf[1] = 0x81;
	write(handle, buf, 65);
	if (res < 0)
		printf("Unable to write() (2)\n");

	/* Read requested state */
	read(handle, buf, 65);
	if (res < 0)
		printf("Unable to read()\n");

	/* Print out the returned buffer. */
	for (int i = 0; i < 4; i++)
		printf("buf[%d]: %d\n", i, buf[i]);

	return 0;
}
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
