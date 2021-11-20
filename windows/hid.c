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

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
// Do not warn about mbsrtowcs and wcsncpy usage.
// https://docs.microsoft.com/cpp/c-runtime-library/security-features-in-the-crt
#define _CRT_SECURE_NO_WARNINGS
#endif

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
#include <string.h>
#include <wctype.h>

#include "hidapi.h"

#undef MIN
#define MIN(x,y) ((x) < (y)? (x): (y))

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
	
	#define HIDP_STATUS_SUCCESS 0x110000

	typedef struct _hid_pp_caps_info {
		USHORT FirstCap;
		USHORT NumberOfCaps; // Includes empty caps after LastCap 
		USHORT LastCap;
		USHORT ReportByteLength;
	} hid_pp_caps_info, * phid_pp_caps_info;


	typedef struct _hid_pp_link_collection_node {
		USAGE  LinkUsage;
		USAGE  LinkUsagePage;
		USHORT Parent;
		USHORT NumberOfChildren;
		USHORT NextSibling;
		USHORT FirstChild;
		ULONG  CollectionType : 8;
		ULONG  IsAlias : 1;
		ULONG  Reserved : 23;
		// Same as the public API structure HIDP_LINK_COLLECTION_NODE, but without PVOID UserContext at the end
	} hid_pp_link_collection_node, * phid_pp_link_collection_node;

	typedef struct _hid_pp_cap {
		USAGE   UsagePage;
		UCHAR   ReportID;
		UCHAR   BitPosition;
		USHORT  BitSize; // WIN32 term for Report Size
		USHORT  ReportCount;
		USHORT  BytePosition;
		USHORT  BitCount;
		ULONG   BitField;
		USHORT  NextBytePosition;
	    USHORT  LinkCollection;
		USAGE   LinkUsagePage;
		USAGE   LinkUsage;

		// 8 Flags in one byte
		BOOLEAN IsMultipleItemsForArray:1;

		BOOLEAN IsPadding:1;
		BOOLEAN IsButtonCap:1;
		BOOLEAN IsAbsolute:1;
		BOOLEAN IsRange:1;
		BOOLEAN IsAlias:1; // IsAlias is set to TRUE in the first n-1 capability structures added to the capability array. IsAlias set to FALSE in the nth capability structure.
		BOOLEAN IsStringRange:1;
		BOOLEAN IsDesignatorRange:1;
		// 8 Flags in one byte
		BOOLEAN Reserved1[3];

		struct _HIDP_UNKNOWN_TOKEN UnknownTokens[4]; // 4 x 8 Byte

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
		union {
			struct {
				LONG    LogicalMin;
				LONG    LogicalMax;
			} Button;
			struct {
				BOOLEAN HasNull;
				UCHAR   Reserved4[3];
				LONG    LogicalMin;
				LONG    LogicalMax;
				LONG    PhysicalMin;
				LONG    PhysicalMax;
			} NotButton;
		};
		ULONG   Units;
		ULONG   UnitsExp;

	} hid_pp_cap, * phid_pp_cap;
	
	typedef struct _hid_preparsed_data {
		UCHAR MagicKey[8];
		USAGE Usage;
		USAGE UsagePage;
		USHORT Reserved[2];

		// CAPS structure for Input, Output and Feature
		hid_pp_caps_info caps_info[3];

		USHORT FirstByteOfLinkCollectionArray;
		USHORT NumberLinkCollectionNodes;

#if defined(__MINGW32__) || defined(__CYGWIN__)
		// MINGW fails with: Flexible array member in union not supported
		// Solution: https://gcc.gnu.org/onlinedocs/gcc/Zero-Length.html
		union {
			hid_pp_cap caps[0];
			hid_pp_link_collection_node LinkCollectionArray[0];
		};
#else
		union {
			hid_pp_cap caps[];
			hid_pp_link_collection_node LinkCollectionArray[];
		};
#endif

	} HIDP_PREPARSED_DATA, * PHIDP_PREPARSED_DATA;

	typedef void(__stdcall *HidD_GetHidGuid_)(LPGUID hid_guid);
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

	static HidD_GetHidGuid_ HidD_GetHidGuid;
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
	
	static HMODULE lib_handle = NULL;
	static BOOLEAN initialized = FALSE;

	typedef DWORD RETURN_TYPE;
	typedef RETURN_TYPE CONFIGRET;
	typedef DWORD DEVNODE, DEVINST;
	typedef DEVNODE* PDEVNODE, * PDEVINST;
	typedef WCHAR* DEVNODEID_W, * DEVINSTID_W;

#define CR_SUCCESS (0x00000000)
#define CR_BUFFER_SMALL (0x0000001A)

#define CM_LOCATE_DEVNODE_NORMAL 0x00000000

#define DEVPROP_TYPEMOD_LIST 0x00002000

#define DEVPROP_TYPE_STRING 0x00000012
#define DEVPROP_TYPE_STRING_LIST (DEVPROP_TYPE_STRING|DEVPROP_TYPEMOD_LIST)

	typedef CONFIGRET(__stdcall* CM_Locate_DevNodeW_)(PDEVINST pdnDevInst, DEVINSTID_W pDeviceID, ULONG ulFlags);
	typedef CONFIGRET(__stdcall* CM_Get_Parent_)(PDEVINST pdnDevInst, DEVINST dnDevInst, ULONG ulFlags);
	typedef CONFIGRET(__stdcall* CM_Get_DevNode_PropertyW_)(DEVINST dnDevInst, CONST DEVPROPKEY* PropertyKey, DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer, PULONG PropertyBufferSize, ULONG ulFlags);
	typedef CONFIGRET(__stdcall* CM_Get_Device_Interface_PropertyW_)(LPCWSTR pszDeviceInterface, CONST DEVPROPKEY* PropertyKey, DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer, PULONG PropertyBufferSize, ULONG ulFlags);

	static CM_Locate_DevNodeW_ CM_Locate_DevNodeW = NULL;
	static CM_Get_Parent_ CM_Get_Parent = NULL;
	static CM_Get_DevNode_PropertyW_ CM_Get_DevNode_PropertyW = NULL;
	static CM_Get_Device_Interface_PropertyW_ CM_Get_Device_Interface_PropertyW = NULL;

	static HMODULE cfgmgr32_lib_handle = NULL;
#endif /* HIDAPI_USE_DDK */

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
		struct hid_device_info* device_info;
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
	dev->device_info = NULL;

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
	free(dev->device_info);
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
		RESOLVE(HidD_GetHidGuid);
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
#undef RESOLVE
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif
	}
	else
		return -1;

	cfgmgr32_lib_handle = LoadLibraryA("cfgmgr32.dll");
	if (cfgmgr32_lib_handle) {
#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#define RESOLVE(x) x = (x##_)GetProcAddress(cfgmgr32_lib_handle, #x);
		RESOLVE(CM_Locate_DevNodeW);
		RESOLVE(CM_Get_Parent);
		RESOLVE(CM_Get_DevNode_PropertyW);
		RESOLVE(CM_Get_Device_Interface_PropertyW);
#undef RESOLVE
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif
	}
	else {
		CM_Locate_DevNodeW = NULL;
		CM_Get_Parent = NULL;
		CM_Get_DevNode_PropertyW = NULL;
		CM_Get_Device_Interface_PropertyW = NULL;
	}

	return 0;
}
#endif

/// <summary>
/// Enumeration of all report descriptor item One-Byte prefixes from the USB HID spec 1.11
/// The two least significiant bits nn represent the size of the item and must be added to this values
/// </summary>
typedef enum rd_items_ {
	rd_main_input =               0x80, ///< 1000 00 nn
	rd_main_output =              0x90, ///< 1001 00 nn
	rd_main_feature =             0xB0, ///< 1011 00 nn
	rd_main_collection =          0xA0, ///< 1010 00 nn
	rd_main_collection_end =      0xC0, ///< 1100 00 nn
	rd_global_usage_page =        0x04, ///< 0000 01 nn
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

typedef enum _RD_MAIN_ITEMS {
	rd_input = HidP_Input,
	rd_output = HidP_Output,
	rd_feature = HidP_Feature,
	rd_collection,
	rd_collection_end,
	rd_delimiter_open,
	rd_delimiter_usage,
	rd_delimiter_close,
	RD_NUM_OF_MAIN_ITEMS
} RD_MAIN_ITEMS;

typedef struct _RD_BIT_RANGE {
	int FirstBit;
	int LastBit;
} RD_BIT_RANGE;

typedef enum _RD_ITEM_NODE_TYPE {
	rd_item_node_cap,
	rd_item_node_padding,
	rd_item_node_collection,
	RD_NUM_OF_ITEM_NODE_TYPES
} RD_NODE_TYPE;

struct rd_main_item_node
{
	int FirstBit; ///< Position of first bit in report (counting from 0)
	int LastBit; ///< Position of last bit in report (counting from 0)
	RD_NODE_TYPE TypeOfNode; ///< Information if caps index refers to the array of button caps, value caps,
                             ///< or if the node is just a padding element to fill unused bit positions.
                             ///< The node can also be a collection node without any bits in the report.
	int CapsIndex; ///< Index in the array of caps
	int CollectionIndex; ///< Index in the array of link collections
	RD_MAIN_ITEMS MainItemType; ///< Input, Output, Feature, Collection or Collection End
	unsigned char ReportID;
	struct rd_main_item_node* next; 
};

	
static struct rd_main_item_node* rd_append_main_item_node(int first_bit, int last_bit, RD_NODE_TYPE type_of_node, int caps_index, int collection_index, RD_MAIN_ITEMS main_item_type, unsigned char report_id, struct rd_main_item_node** list) {
	struct rd_main_item_node* new_list_node;

	// Determine last node in the list
	while (*list != NULL)
	{
		list = &(*list)->next;
	}

	new_list_node = malloc(sizeof(*new_list_node)); // Create new list entry
	new_list_node->FirstBit = first_bit;
	new_list_node->LastBit = last_bit;
	new_list_node->TypeOfNode = type_of_node;
	new_list_node->CapsIndex = caps_index;
	new_list_node->CollectionIndex = collection_index;
	new_list_node->MainItemType = main_item_type;
	new_list_node->ReportID = report_id;
	new_list_node->next = NULL; // NULL marks last node in the list

	*list = new_list_node;
	return new_list_node;
}

static struct  rd_main_item_node* rd_insert_main_item_node(int first_bit, int last_bit, RD_NODE_TYPE type_of_node, int caps_index, int collection_index, RD_MAIN_ITEMS main_item_type, unsigned char report_id, struct rd_main_item_node** list) {
	// Insert item after the main item node referenced by list
	struct rd_main_item_node* next_item = (*list)->next;
	(*list)->next = NULL;
	rd_append_main_item_node(first_bit, last_bit, type_of_node, caps_index, collection_index, main_item_type, report_id, list);
	(*list)->next->next = next_item;
	return (*list)->next;
}

static struct rd_main_item_node* rd_search_main_item_list_for_bit_position(int search_bit, RD_MAIN_ITEMS main_item_type, unsigned char report_id, struct rd_main_item_node** list) {
	// Determine first INPUT/OUTPUT/FEATURE main item, where the last bit position is equal or greater than the search bit position
	
	while (((*list)->next->MainItemType != rd_collection) &&
		   ((*list)->next->MainItemType != rd_collection_end) &&
		   !(((*list)->next->LastBit >= search_bit) &&
		   ((*list)->next->ReportID == report_id) &&
		   ((*list)->next->MainItemType == main_item_type))
		)
	{
		list = &(*list)->next;
	}
	return *list;
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
	if (cfgmgr32_lib_handle)
		FreeLibrary(cfgmgr32_lib_handle);
	cfgmgr32_lib_handle = NULL;
	initialized = FALSE;
#endif
	return 0;
}

static void hid_internal_get_ble_info(struct hid_device_info* dev, DEVINST dev_node)
{
	ULONG len;
	CONFIGRET cr;
	DEVPROPTYPE property_type;

	static DEVPROPKEY DEVPKEY_NAME = { { 0xb725f130, 0x47ef, 0x101a, 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac }, 10 }; // DEVPROP_TYPE_STRING
	static DEVPROPKEY PKEY_DeviceInterface_Bluetooth_DeviceAddress = { { 0x2BD67D8B, 0x8BEB, 0x48D5, 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A }, 1 }; // DEVPROP_TYPE_STRING
	static DEVPROPKEY PKEY_DeviceInterface_Bluetooth_Manufacturer = { { 0x2BD67D8B, 0x8BEB, 0x48D5, 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A }, 4 }; // DEVPROP_TYPE_STRING

	/* Manufacturer String */
	len = 0;
	cr = CM_Get_DevNode_PropertyW(dev_node, &PKEY_DeviceInterface_Bluetooth_Manufacturer, &property_type, NULL, &len, 0);
	if (cr == CR_BUFFER_SMALL && property_type == DEVPROP_TYPE_STRING) {
		free(dev->manufacturer_string);
		dev->manufacturer_string = (wchar_t*)calloc(len, sizeof(BYTE));
		CM_Get_DevNode_PropertyW(dev_node, &PKEY_DeviceInterface_Bluetooth_Manufacturer, &property_type, (PBYTE)dev->manufacturer_string, &len, 0);
	}

	/* Serial Number String (MAC Address) */
	len = 0;
	cr = CM_Get_DevNode_PropertyW(dev_node, &PKEY_DeviceInterface_Bluetooth_DeviceAddress, &property_type, NULL, &len, 0);
	if (cr == CR_BUFFER_SMALL && property_type == DEVPROP_TYPE_STRING) {
		free(dev->serial_number);
		dev->serial_number = (wchar_t*)calloc(len, sizeof(BYTE));
		CM_Get_DevNode_PropertyW(dev_node, &PKEY_DeviceInterface_Bluetooth_DeviceAddress, &property_type, (PBYTE)dev->serial_number, &len, 0);
	}

	/* Get devnode grandparent to reach out Bluetooth LE device node */
	cr = CM_Get_Parent(&dev_node, dev_node, 0);
	if (cr != CR_SUCCESS)
		return;

	/* Product String */
	len = 0;
	cr = CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_NAME, &property_type, NULL, &len, 0);
	if (cr == CR_BUFFER_SMALL && property_type == DEVPROP_TYPE_STRING) {
		free(dev->product_string);
		dev->product_string = (wchar_t*)calloc(len, sizeof(BYTE));
		CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_NAME, &property_type, (PBYTE)dev->product_string, &len, 0);
	}
}

static void hid_internal_get_info(struct hid_device_info* dev)
{
	const char *tmp = NULL;
	wchar_t *interface_path = NULL, *device_id = NULL, *compatible_ids = NULL;
	mbstate_t state;
	ULONG len;
	CONFIGRET cr;
	DEVPROPTYPE property_type;
	DEVINST dev_node;

	static DEVPROPKEY DEVPKEY_Device_InstanceId = { { 0x78c34fc8, 0x104a, 0x4aca, 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 }, 256 }; // DEVPROP_TYPE_STRING
	static DEVPROPKEY DEVPKEY_Device_CompatibleIds = { { 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}, 4 }; // DEVPROP_TYPE_STRING_LIST

	if (!CM_Get_Device_Interface_PropertyW ||
		!CM_Locate_DevNodeW ||
		!CM_Get_Parent ||
		!CM_Get_DevNode_PropertyW)
		goto end;

	tmp = dev->path;

	len = (ULONG)strlen(tmp);
	interface_path = (wchar_t*)calloc(len + 1, sizeof(wchar_t));
	memset(&state, 0, sizeof(state));

	if (mbsrtowcs(interface_path, &tmp, len, &state) == (size_t)-1)
		goto end;

	/* Get the device id from interface path */
	len = 0;
	cr = CM_Get_Device_Interface_PropertyW(interface_path, &DEVPKEY_Device_InstanceId, &property_type, NULL, &len, 0);
	if (cr == CR_BUFFER_SMALL && property_type == DEVPROP_TYPE_STRING) {
		device_id = (wchar_t*)calloc(len, sizeof(BYTE));
		cr = CM_Get_Device_Interface_PropertyW(interface_path, &DEVPKEY_Device_InstanceId, &property_type, (PBYTE)device_id, &len, 0);
	}
	if (cr != CR_SUCCESS)
		goto end;

	/* Open devnode from device id */
	cr = CM_Locate_DevNodeW(&dev_node, (DEVINSTID_W)device_id, CM_LOCATE_DEVNODE_NORMAL);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get devnode parent */
	cr = CM_Get_Parent(&dev_node, dev_node, 0);
	if (cr != CR_SUCCESS)
		goto end;

	/* Get the compatible ids from parent devnode */
	len = 0;
	cr = CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_Device_CompatibleIds, &property_type, NULL, &len, 0);
	if (cr == CR_BUFFER_SMALL && property_type == DEVPROP_TYPE_STRING_LIST) {
		compatible_ids = (wchar_t*)calloc(len, sizeof(BYTE));
		cr = CM_Get_DevNode_PropertyW(dev_node, &DEVPKEY_Device_CompatibleIds, &property_type, (PBYTE)compatible_ids, &len, 0);
	}
	if (cr != CR_SUCCESS)
		goto end;

	/* Now we can parse parent's compatible IDs to find out the device bus type */
	for (wchar_t* compatible_id = compatible_ids; *compatible_id; compatible_id += wcslen(compatible_id) + 1) {
		/* Normalize to upper case */
		for (wchar_t* p = compatible_id; *p; ++p) *p = towupper(*p);

		/* Bluetooth LE devices */
		if (wcsstr(compatible_id, L"BTHLEDEVICE") != NULL) {
			/* HidD_GetProductString/HidD_GetManufacturerString/HidD_GetSerialNumberString is not working for BLE HID devices
			   Request this info via dev node properties instead.
			   https://docs.microsoft.com/answers/questions/401236/hidd-getproductstring-with-ble-hid-device.html */
			hid_internal_get_ble_info(dev, dev_node);
			break;
		}
	}
end:
	free(interface_path);
	free(device_id);
	free(compatible_ids);
}

static struct hid_device_info *hid_get_device_info(const char *path, HANDLE handle)
{
	struct hid_device_info *dev = NULL; /* return object */

	BOOL res;
	HIDD_ATTRIBUTES attrib;
	PHIDP_PREPARSED_DATA pp_data = NULL;
	HIDP_CAPS caps;

	#define WSTR_LEN 512
	wchar_t wstr[WSTR_LEN]; /* TODO: Determine Size */

	/* Create the record. */
	dev = (struct hid_device_info*)calloc(1, sizeof(struct hid_device_info));

	/* Fill out the record */
	dev->next = NULL;

	if (path) {
		size_t len = strlen(path);
		dev->path = (char*)calloc(len + 1, sizeof(char));
		memcpy(dev->path, path, len + 1);
	}
	else
		dev->path = NULL;

	attrib.Size = sizeof(HIDD_ATTRIBUTES);
	res = HidD_GetAttributes(handle, &attrib);
	if (res) {
		/* VID/PID */
		dev->vendor_id = attrib.VendorID;
		dev->product_id = attrib.ProductID;

		/* Release Number */
		dev->release_number = attrib.VersionNumber;
	}

	/* Get the Usage Page and Usage for this device. */
	res = HidD_GetPreparsedData(handle, &pp_data);
	if (res) {
		NTSTATUS nt_res = HidP_GetCaps(pp_data, &caps);
		if (nt_res == HIDP_STATUS_SUCCESS) {
			dev->usage_page = caps.UsagePage;
			dev->usage = caps.Usage;
		}

		HidD_FreePreparsedData(pp_data);
	}

	/* Serial Number */
	wstr[0] = L'\0';
	res = HidD_GetSerialNumberString(handle, wstr, sizeof(wstr));
	wstr[WSTR_LEN - 1] = L'\0';
	dev->serial_number = _wcsdup(wstr);

	/* Manufacturer String */
	wstr[0] = L'\0';
	res = HidD_GetManufacturerString(handle, wstr, sizeof(wstr));
	wstr[WSTR_LEN - 1] = L'\0';
	dev->manufacturer_string = _wcsdup(wstr);

	/* Product String */
	wstr[0] = L'\0';
	res = HidD_GetProductString(handle, wstr, sizeof(wstr));
	wstr[WSTR_LEN - 1] = L'\0';
	dev->product_string = _wcsdup(wstr);

	/* Interface Number. It can sometimes be parsed out of the path
	   on Windows if a device has multiple interfaces. See
	   https://docs.microsoft.com/windows-hardware/drivers/hid/hidclass-hardware-ids-for-top-level-collections
	   or search for "HIDClass Hardware IDs for Top-Level Collections" at Microsoft Docs. If it's not
	   in the path, it's set to -1. */
	dev->interface_number = -1;
	if (dev->path) {
		char* interface_component = strstr(dev->path, "&mi_");
		if (interface_component) {
			char* hex_str = interface_component + 4;
			char* endptr = NULL;
			dev->interface_number = strtol(hex_str, &endptr, 16);
			if (endptr == hex_str) {
				/* The parsing failed. Set interface_number to -1. */
				dev->interface_number = -1;
			}
		}
	}

	hid_internal_get_info(dev);

	return dev;
}

struct hid_device_info HID_API_EXPORT * HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	BOOL res;
	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;
	GUID interface_class_guid;

	/* Windows objects for interacting with the driver. */
	SP_DEVINFO_DATA devinfo_data;
	SP_DEVICE_INTERFACE_DATA device_interface_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA_A *device_interface_detail_data = NULL;
	HDEVINFO device_info_set = INVALID_HANDLE_VALUE;
	char driver_name[256];
	int device_index = 0;

	if (hid_init() < 0)
		return NULL;

	/* Retrieve HID Interface Class GUID
	   https://docs.microsoft.com/windows-hardware/drivers/install/guid-devinterface-hid */
	HidD_GetHidGuid(&interface_class_guid);

	/* Initialize the Windows objects. */
	memset(&devinfo_data, 0x0, sizeof(devinfo_data));
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
	device_interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	/* Get information for all the devices belonging to the HID class. */
	device_info_set = SetupDiGetClassDevsA(&interface_class_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	/* Iterate over each device in the HID class, looking for the right one. */

	for (;;) {
		HANDLE read_handle = INVALID_HANDLE_VALUE;
		DWORD required_size = 0;
		HIDD_ATTRIBUTES attrib;

		res = SetupDiEnumDeviceInterfaces(device_info_set,
			NULL,
			&interface_class_guid,
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

		/* Open read-only handle to the device */
		read_handle = open_device(device_interface_detail_data->DevicePath, FALSE);

		/* Check validity of read_handle. */
		if (read_handle == INVALID_HANDLE_VALUE) {
			/* Unable to open the device. */
			//register_error(dev, "CreateFile");
			goto cont;
		}

		/* Get the Vendor ID and Product ID for this device. */
		attrib.Size = sizeof(HIDD_ATTRIBUTES);
		HidD_GetAttributes(read_handle, &attrib);
		//wprintf(L"Product/Vendor: %x %x\n", attrib.ProductID, attrib.VendorID);

		/* Check the VID/PID to see if we should add this
		   device to the enumeration list. */
		if ((vendor_id == 0x0 || attrib.VendorID == vendor_id) &&
		    (product_id == 0x0 || attrib.ProductID == product_id)) {

			/* VID/PID match. Create the record. */
			struct hid_device_info *tmp = hid_get_device_info(device_interface_detail_data->DevicePath, read_handle);

			if (tmp == NULL) {
				goto cont_close;
			}

			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;
		}

cont_close:
		CloseHandle(read_handle);
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

	dev->device_info = hid_get_device_info(path, dev->device_handle);

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

static int hid_get_report(hid_device *dev, DWORD report_type, unsigned char *data, size_t length)
{
	BOOL res;
	DWORD bytes_returned = 0;

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	res = DeviceIoControl(dev->device_handle,
		report_type,
		data, (DWORD) length,
		data, (DWORD) length,
		&bytes_returned, &ol);

	if (!res) {
		if (GetLastError() != ERROR_IO_PENDING) {
			/* DeviceIoControl() failed. Return error. */
			register_error(dev, "Get Input/Feature Report DeviceIoControl");
			return -1;
		}
	}

	/* Wait here until the write is done. This makes
	   hid_get_feature_report() synchronous. */
	res = GetOverlappedResult(dev->device_handle, &ol, &bytes_returned, TRUE/*wait*/);
	if (!res) {
		/* The operation failed. */
		register_error(dev, "Get Input/Feature Report GetOverLappedResult");
		return -1;
	}

	/* When numbered reports aren't used,
	   bytes_returned seem to include only what is actually received from the device
	   (not including the first byte with 0, as an indication "no numbered reports"). */
	if (data[0] == 0x0) {
		bytes_returned++;
	}

	return bytes_returned;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetFeature() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_FEATURE, data, length);
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	/* We could use HidD_GetInputReport() instead, but it doesn't give us an actual length, unfortunately */
	return hid_get_report(dev, IOCTL_HID_GET_INPUT_REPORT, data, length);
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
	if (!dev->device_info || !string || !maxlen)
		return -1;

	wcsncpy(string, dev->device_info->manufacturer_string, maxlen);
	string[maxlen] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!dev->device_info || !string || !maxlen)
		return -1;

	wcsncpy(string, dev->device_info->product_string, maxlen);
	string[maxlen] = L'\0';

	return 0;
}

int HID_API_EXPORT_CALL HID_API_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!dev->device_info || !string || !maxlen)
		return -1;

	wcsncpy(string, dev->device_info->serial_number, maxlen);
	string[maxlen] = L'\0';

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

int reconstruct_report_descriptor(hid_device * dev, PHIDP_PREPARSED_DATA pp_data, unsigned char* buf, size_t buf_size) {
	struct rd_item_byte* byte_list = NULL;
	HIDP_CAPS caps;

	if (HidP_GetCaps(pp_data, &caps) != HIDP_STATUS_SUCCESS) {
		return -1;
	}

	// See: https://docs.microsoft.com/en-us/windows-hardware/drivers/hid/link-collections#ddk-link-collection-array-kg
	PHIDP_LINK_COLLECTION_NODE link_collection_nodes;
	link_collection_nodes = (PHIDP_LINK_COLLECTION_NODE)malloc(caps.NumberLinkCollectionNodes * sizeof(HIDP_LINK_COLLECTION_NODE));
	ULONG                     link_collection_nodes_len = caps.NumberLinkCollectionNodes;

		
	ULONG max_datalist_len[NUM_OF_HIDP_REPORT_TYPES];

	if (HidP_GetLinkCollectionNodes(link_collection_nodes, &link_collection_nodes_len, pp_data) != HIDP_STATUS_SUCCESS) {
		//register_error(dev, "HidP_GetLinkCollectionNodes: Buffer to small");
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
			for (USHORT caps_idx = pp_data->caps_info[rt_idx].FirstCap; caps_idx < pp_data->caps_info[rt_idx].LastCap; caps_idx++) {
				int first_bit, last_bit;
				first_bit = (pp_data->caps[caps_idx].BytePosition - 1) * 8 +
						        pp_data->caps[caps_idx].BitPosition;
				last_bit = first_bit + pp_data->caps[caps_idx].BitSize *
						                pp_data->caps[caps_idx].ReportCount - 1;
				if (coll_bit_range[pp_data->caps[caps_idx].LinkCollection][pp_data->caps[caps_idx].ReportID][rt_idx]->FirstBit == -1 ||
					coll_bit_range[pp_data->caps[caps_idx].LinkCollection][pp_data->caps[caps_idx].ReportID][rt_idx]->FirstBit > first_bit) {
					coll_bit_range[pp_data->caps[caps_idx].LinkCollection][pp_data->caps[caps_idx].ReportID][rt_idx]->FirstBit = first_bit;
				}
				if (coll_bit_range[pp_data->caps[caps_idx].LinkCollection][pp_data->caps[caps_idx].ReportID][rt_idx]->LastBit < last_bit) {
					coll_bit_range[pp_data->caps[caps_idx].LinkCollection][pp_data->caps[caps_idx].ReportID][rt_idx]->LastBit = last_bit;
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
			struct rd_main_item_node* firstDelimiterNode = NULL;
			struct rd_main_item_node* delimiterCloseNode = NULL;
			coll_begin_lookup[0] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
			while (actual_coll_level >= 0) {
				if ((coll_number_of_direct_childs[collection_node_idx] != 0) &&
					(coll_last_written_child[collection_node_idx] == -1)) {
					// Collection has child collections, but none is written to the list yet

					coll_last_written_child[collection_node_idx] = coll_child_order[collection_node_idx][0];
					collection_node_idx = coll_child_order[collection_node_idx][0];

					// In a HID Report Descriptor, the first usage declared is the most preferred usage for the control.
					// While the order in the WIN32 capabiliy strutures is the opposite:
					// Here the preferred usage is the last aliased usage in the sequence.

					if (link_collection_nodes[collection_node_idx].IsAlias && (firstDelimiterNode == NULL)) {
						// Alliased Collection (First node in link_collection_nodes -> Last entry in report descriptor output)
						firstDelimiterNode = main_item_list;
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_usage, 0, &main_item_list);
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_close, 0, &main_item_list);
						delimiterCloseNode = main_item_list;
					}
					else {
						// Normal not aliased collection
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
						actual_coll_level++;
					}


				}
				else if ((coll_number_of_direct_childs[collection_node_idx] > 1) &&
					(coll_last_written_child[collection_node_idx] != coll_child_order[collection_node_idx][coll_number_of_direct_childs[collection_node_idx] - 1])) {
					// Collection has child collections, and this is not the first child

					int nextChild = 1;
					while (coll_last_written_child[collection_node_idx] != coll_child_order[collection_node_idx][nextChild - 1]) {
						nextChild++;
					}
					coll_last_written_child[collection_node_idx] = coll_child_order[collection_node_idx][nextChild];
					collection_node_idx = coll_child_order[collection_node_idx][nextChild];
												
					if (link_collection_nodes[collection_node_idx].IsAlias && (firstDelimiterNode == NULL)) {
						// Alliased Collection (First node in link_collection_nodes -> Last entry in report descriptor output)
						firstDelimiterNode = main_item_list;
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_usage, 0, &main_item_list);
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_close, 0, &main_item_list);
						delimiterCloseNode = main_item_list;
					}
					else if (link_collection_nodes[collection_node_idx].IsAlias && (firstDelimiterNode != NULL)) {
						coll_begin_lookup[collection_node_idx] = rd_insert_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_usage, 0, &firstDelimiterNode);
					}
					else if (!link_collection_nodes[collection_node_idx].IsAlias && (firstDelimiterNode != NULL)) {
						coll_begin_lookup[collection_node_idx] = rd_insert_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_usage, 0, &firstDelimiterNode);
						coll_begin_lookup[collection_node_idx] = rd_insert_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_delimiter_open, 0, &firstDelimiterNode);
						firstDelimiterNode = NULL;
						main_item_list = delimiterCloseNode;
						delimiterCloseNode = NULL; // Last entry of alias has .IsAlias == FALSE
					}
					if (!link_collection_nodes[collection_node_idx].IsAlias) {
						coll_begin_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_collection, 0, &main_item_list);
						actual_coll_level++;
					}
				}
				else {
					actual_coll_level--;
					coll_end_lookup[collection_node_idx] = rd_append_main_item_node(0, 0, rd_item_node_collection, 0, collection_node_idx, rd_collection_end, 0, &main_item_list);
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
			// Add all value caps to node list
			struct rd_main_item_node* firstDelimiterNode = NULL;
			struct rd_main_item_node* delimiterCloseNode = NULL;
			for (USHORT caps_idx = pp_data->caps_info[rt_idx].FirstCap; caps_idx < pp_data->caps_info[rt_idx].LastCap; caps_idx++) {
				struct rd_main_item_node* coll_begin = coll_begin_lookup[pp_data->caps[caps_idx].LinkCollection];
				int first_bit, last_bit;
				first_bit = (pp_data->caps[caps_idx].BytePosition - 1) * 8 +
					pp_data->caps[caps_idx].BitPosition;
				last_bit = first_bit + pp_data->caps[caps_idx].BitSize *
					pp_data->caps[caps_idx].ReportCount - 1;

				for (int child_idx = 0; child_idx < coll_number_of_direct_childs[pp_data->caps[caps_idx].LinkCollection]; child_idx++) {
					// Determine in which section before/between/after child collection the item should be inserted
					if (first_bit < coll_bit_range[coll_child_order[pp_data->caps[caps_idx].LinkCollection][child_idx]][pp_data->caps[caps_idx].ReportID][rt_idx]->FirstBit)
					{
						// Note, that the default value for undefined coll_bit_range is -1, which cant be greater than the bit position
						break;
					}
					coll_begin = coll_end_lookup[coll_child_order[pp_data->caps[caps_idx].LinkCollection][child_idx]];
				}
				struct rd_main_item_node* list_node;
				list_node = rd_search_main_item_list_for_bit_position(first_bit, rt_idx, pp_data->caps[caps_idx].ReportID, &coll_begin);

				// In a HID Report Descriptor, the first usage declared is the most preferred usage for the control.
				// While the order in the WIN32 capabiliy strutures is the opposite:
				// Here the preferred usage is the last aliased usage in the sequence.

				if (pp_data->caps[caps_idx].IsAlias && (firstDelimiterNode == NULL)) {
					// Alliased Usage (First node in pp_data->caps -> Last entry in report descriptor output)
					firstDelimiterNode = list_node;
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rd_delimiter_usage, pp_data->caps[caps_idx].ReportID, &list_node);
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rd_delimiter_close, pp_data->caps[caps_idx].ReportID, &list_node);
					delimiterCloseNode = list_node;
				} else if (pp_data->caps[caps_idx].IsAlias && (firstDelimiterNode != NULL)) {
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rd_delimiter_usage, pp_data->caps[caps_idx].ReportID, &list_node);
				}
				else if (!pp_data->caps[caps_idx].IsAlias && (firstDelimiterNode != NULL)) {
					// Alliased Collection (Last node in pp_data->caps -> First entry in report descriptor output)
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rd_delimiter_usage, pp_data->caps[caps_idx].ReportID, &list_node);
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rd_delimiter_open, pp_data->caps[caps_idx].ReportID, &list_node);
					firstDelimiterNode = NULL;
					list_node = delimiterCloseNode;
					delimiterCloseNode = NULL; // Last entry of alias has .IsAlias == FALSE
				}
				if (!pp_data->caps[caps_idx].IsAlias) {
					rd_insert_main_item_node(first_bit, last_bit, rd_item_node_cap, caps_idx, pp_data->caps[caps_idx].LinkCollection, rt_idx, pp_data->caps[caps_idx].ReportID, &list_node);
				}
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

			struct rd_main_item_node* list = main_item_list; // List root;
				
			while (list->next != NULL)
			{
				if ((list->MainItemType >= rd_input) &&
					(list->MainItemType <= rd_feature)) {
					// INPUT, OUTPUT or FEATURE
					if (list->FirstBit != -1) {
						if ((last_bit_position[list->MainItemType][list->ReportID] + 1 != list->FirstBit) &&
							(last_report_item_lookup[list->MainItemType][list->ReportID]->FirstBit != list->FirstBit) // Happens in case of IsMultipleItemsForArray for multiple dedicated usages for a multi-button array
							) {
							struct rd_main_item_node* list_node;
							list_node = rd_search_main_item_list_for_bit_position(last_bit_position[list->MainItemType][list->ReportID], list->MainItemType, list->ReportID, &last_report_item_lookup[list->MainItemType][list->ReportID]);
							rd_insert_main_item_node(last_bit_position[list->MainItemType][list->ReportID], list->FirstBit - 1, rd_item_node_padding, -1, 0, list->MainItemType, list->ReportID, &list_node);
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
							// Insert padding item after item referenced in last_report_item_lookup
							rd_insert_main_item_node(last_bit_position[rt_idx][reportid_idx], last_bit_position[rt_idx][reportid_idx] + padding, rd_item_node_padding, -1, 0, rt_idx, reportid_idx, &last_report_item_lookup[rt_idx][reportid_idx]);
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
		BOOLEAN inhibit_write_of_usage = FALSE; // Needed in case of delimited usage print, before the normal collection or cap
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
				if (inhibit_write_of_usage) {
					// Inhibit only once after DELIMITER statement
					inhibit_write_of_usage = FALSE;
				}
				else {
					rd_write_short_item(rd_local_usage, link_collection_nodes[main_item_list->CollectionIndex].LinkUsage, &byte_list);
					printf("Usage  (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].LinkUsage);
				}
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
					rd_write_short_item(rd_main_collection, link_collection_nodes[main_item_list->CollectionIndex].CollectionType, &byte_list);
					printf("Collection (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].CollectionType);
				}
			}
			else if (main_item_list->MainItemType == rd_collection_end) {
				rd_write_short_item(rd_main_collection_end, 0, &byte_list);
				printf("End Collection\n");
			}
			else if (main_item_list->MainItemType == rd_delimiter_open) {
				if (main_item_list->CollectionIndex != -1) {
					// Print usage page when changed
					if (last_usage_page != link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage) {
						rd_write_short_item(rd_global_usage_page, link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage, &byte_list);
						printf("Usage Page (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage);
						last_usage_page = link_collection_nodes[main_item_list->CollectionIndex].LinkUsagePage;
					}
				}
				else if (main_item_list->CapsIndex != 0) {
					// Print usage page when changed
					int caps_idx = main_item_list->CapsIndex;
					if (pp_data->caps[caps_idx].UsagePage != last_usage_page) {
						rd_write_short_item(rd_global_usage_page, pp_data->caps[caps_idx].UsagePage, &byte_list);
						printf("Usage Page (%d)\n", pp_data->caps[caps_idx].UsagePage);
						last_usage_page = pp_data->caps[caps_idx].UsagePage;
					}
				}
				rd_write_short_item(rd_local_delimiter, 1, &byte_list); // 1 = open set of aliased usages
				printf("Delimiter Open (%d)\n", 1);
			}
			else if (main_item_list->MainItemType == rd_delimiter_usage) {
				if (main_item_list->CollectionIndex != -1) {
					// Print Aliased Collection usage
					rd_write_short_item(rd_local_usage, link_collection_nodes[main_item_list->CollectionIndex].LinkUsage, &byte_list);
					printf("Usage  (%d)\n", link_collection_nodes[main_item_list->CollectionIndex].LinkUsage);
				}  if (main_item_list->CapsIndex != 0) {
					int caps_idx = main_item_list->CapsIndex;
					// Print Aliased Usage
					if (pp_data->caps[caps_idx].IsRange) {
						rd_write_short_item(rd_local_usage_minimum, pp_data->caps[caps_idx].Range.UsageMin, &byte_list);
						printf("Usage Minimum (%d)\n", pp_data->caps[caps_idx].Range.UsageMin);
						rd_write_short_item(rd_local_usage_maximum, pp_data->caps[caps_idx].Range.UsageMax, &byte_list);
						printf("Usage Maximum (%d)\n", pp_data->caps[caps_idx].Range.UsageMax);
					}
					else {
						rd_write_short_item(rd_local_usage, pp_data->caps[caps_idx].NotRange.Usage, &byte_list);
						printf("Usage (%d)\n", pp_data->caps[caps_idx].NotRange.Usage);
					}
				}
			}
			else if (main_item_list->MainItemType == rd_delimiter_close) {
				rd_write_short_item(rd_local_delimiter, 0, &byte_list); // 0 = close set of aliased usages
				printf("Delimiter Close (%d)\n", 0);
				// Inhibit next usage write
				inhibit_write_of_usage = TRUE;
			}
			else if (main_item_list->TypeOfNode == rd_item_node_padding) {
				// Padding 

				rd_write_short_item(rd_global_report_size, (main_item_list->LastBit - main_item_list->FirstBit), &byte_list);
				printf("Report Size (%d)  Padding\n", (main_item_list->LastBit - main_item_list->FirstBit));

				rd_write_short_item(rd_global_report_count, 1, &byte_list);
				printf("Report Count (%d) Padding\n", 1);

				if (rt_idx == HidP_Input) {
					rd_write_short_item(rd_main_input, 0x03, &byte_list); // Const / Abs
					printf("Input (0x%02X)     Padding\n", 0x03);
				}
				else if (rt_idx == HidP_Output) {
					rd_write_short_item(rd_main_output, 0x03, &byte_list); // Const / Abs
					printf("Output (0x%02X)    Padding\n", 0x03);
				}
				else if (rt_idx == HidP_Feature) {
					rd_write_short_item(rd_main_feature, 0x03, &byte_list); // Const / Abs
					printf("Feature (0x%02X)   Padding\n", 0x03);
				}
				report_count = 0;
			}
			else if (pp_data->caps[caps_idx].IsButtonCap) {
				// Button
				if (last_report_id != pp_data->caps[caps_idx].ReportID) {
					// Write Report ID if changed
					rd_write_short_item(rd_global_report_id, pp_data->caps[caps_idx].ReportID, &byte_list);
					printf("Report ID (%d)\n", pp_data->caps[caps_idx].ReportID);
					last_report_id = pp_data->caps[caps_idx].ReportID;
				}

				// Print usage page when changed
				if (pp_data->caps[caps_idx].UsagePage != last_usage_page) {
					rd_write_short_item(rd_global_usage_page, pp_data->caps[caps_idx].UsagePage, &byte_list);
					printf("Usage Page (%d)\n", pp_data->caps[caps_idx].UsagePage);
					last_usage_page = pp_data->caps[caps_idx].UsagePage;
				}

				// Print only local report items for each cap, if ReportCount > 1
				if (pp_data->caps[caps_idx].IsRange) {
					report_count += (pp_data->caps[caps_idx].Range.DataIndexMax - pp_data->caps[caps_idx].Range.DataIndexMin);
				}

				if (inhibit_write_of_usage) {
					// Inhibit only once after DELIMITER statement
					inhibit_write_of_usage = FALSE;
				}
				else {
					if (pp_data->caps[caps_idx].IsRange) {
						rd_write_short_item(rd_local_usage_minimum, pp_data->caps[caps_idx].Range.UsageMin, &byte_list);
						printf("Usage Minimum (%d)\n", pp_data->caps[caps_idx].Range.UsageMin);
						rd_write_short_item(rd_local_usage_maximum, pp_data->caps[caps_idx].Range.UsageMax, &byte_list);
						printf("Usage Maximum (%d)\n", pp_data->caps[caps_idx].Range.UsageMax);
					}
					else {
						rd_write_short_item(rd_local_usage, pp_data->caps[caps_idx].NotRange.Usage, &byte_list);
						printf("Usage (%d)\n", pp_data->caps[caps_idx].NotRange.Usage);
					}
				}

				if (pp_data->caps[caps_idx].IsDesignatorRange) {
					rd_write_short_item(rd_local_designator_minimum, pp_data->caps[caps_idx].Range.DesignatorMin, &byte_list);
					printf("Designator Minimum (%d)\n", pp_data->caps[caps_idx].Range.DesignatorMin);
					rd_write_short_item(rd_local_designator_maximum, pp_data->caps[caps_idx].Range.DesignatorMax, &byte_list);
					printf("Designator Maximum (%d)\n", pp_data->caps[caps_idx].Range.DesignatorMax);
				}
				else if (pp_data->caps[caps_idx].NotRange.DesignatorIndex != 0) {
					// Designator set 0 is a special descriptor set (of the HID Physical Descriptor),
					// that specifies the number of additional descriptor sets.
					// Therefore Designator Index 0 can never be a useful reference for a control.
					rd_write_short_item(rd_local_designator_index, pp_data->caps[caps_idx].NotRange.DesignatorIndex, &byte_list);
					printf("Designator Index (%d)\n", pp_data->caps[caps_idx].NotRange.DesignatorIndex);
				}

				if (pp_data->caps[caps_idx].IsStringRange) {
					rd_write_short_item(rd_local_string_minimum, pp_data->caps[caps_idx].Range.StringMin, &byte_list);
					printf("String Minimum (%d)\n", pp_data->caps[caps_idx].Range.StringMin);
					rd_write_short_item(rd_local_string_maximum, pp_data->caps[caps_idx].Range.StringMax, &byte_list);
					printf("String Maximum (%d)\n", pp_data->caps[caps_idx].Range.StringMax);
				}
				else if (pp_data->caps[caps_idx].NotRange.StringIndex != 0) {
					// String Index 0 is a special entry, that contains a list of supported languages,
					// therefore Designator Index 0 can never be a useful reference for a control.
					rd_write_short_item(rd_local_string, pp_data->caps[caps_idx].NotRange.StringIndex, &byte_list);
					printf("String Index (%d)\n", pp_data->caps[caps_idx].NotRange.StringIndex);
				}

				if ((main_item_list->next != NULL) &&
					(main_item_list->next->MainItemType == rt_idx) &&
					(main_item_list->next->TypeOfNode == rd_item_node_cap) &&
					(pp_data->caps[main_item_list->next->CapsIndex].IsButtonCap) &&
					(!pp_data->caps[caps_idx].IsRange) && // This node in list is no array
					(!pp_data->caps[main_item_list->next->CapsIndex].IsRange) && // Next node in list is no array
					(pp_data->caps[main_item_list->next->CapsIndex].UsagePage == pp_data->caps[caps_idx].UsagePage) &&
					(pp_data->caps[main_item_list->next->CapsIndex].ReportID == pp_data->caps[caps_idx].ReportID) &&
					(pp_data->caps[main_item_list->next->CapsIndex].BitField == pp_data->caps[caps_idx].BitField)
					) {
					if (main_item_list->next->FirstBit != main_item_list->FirstBit) {
						// In case of IsMultipleItemsForArray for multiple dedicated usages for a multi-button array, the report count should be incremented 
							
						// Skip global items until any of them changes, than use ReportCount item to write the count of identical report fields
						report_count++;
					}
				}
				else {

					if ((pp_data->caps[caps_idx].Button.LogicalMin == 0) &&
						(pp_data->caps[caps_idx].Button.LogicalMax == 0)) {
						rd_write_short_item(rd_global_logical_minimum, 0, &byte_list);
						printf("Logical Minimum (%d)\n", 0);

						rd_write_short_item(rd_global_logical_maximum, 1, &byte_list);
						printf("Logical Maximum (%d)\n", 1);
					}
					else {
						rd_write_short_item(rd_global_logical_minimum, pp_data->caps[caps_idx].Button.LogicalMin, &byte_list);
						printf("Logical Minimum (%d)\n", pp_data->caps[caps_idx].Button.LogicalMin);

						rd_write_short_item(rd_global_logical_maximum, pp_data->caps[caps_idx].Button.LogicalMax, &byte_list);
						printf("Logical Maximum (%d)\n", pp_data->caps[caps_idx].Button.LogicalMax);
					}

					rd_write_short_item(rd_global_report_size, pp_data->caps[caps_idx].BitSize, &byte_list);
					printf("Report Size (%d)\n", pp_data->caps[caps_idx].BitSize);

					if (!pp_data->caps[caps_idx].IsRange) {
						// Variable bit field with one bit per button
						rd_write_short_item(rd_global_report_count, pp_data->caps[caps_idx].ReportCount + report_count, &byte_list);
						printf("Report Count (%d)\n", pp_data->caps[caps_idx].ReportCount + report_count);
					}
					else {
						// Button array of Report Size x Report Count
						rd_write_short_item(rd_global_report_count, pp_data->caps[caps_idx].ReportCount, &byte_list);
						printf("Report Count (%d)\n", pp_data->caps[caps_idx].ReportCount);
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
						rd_write_short_item(rd_main_input, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Input (0x%02X)\n", pp_data->caps[caps_idx].BitField);
					}
					else if (rt_idx == HidP_Output) {
						rd_write_short_item(rd_main_output, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Output (0x%02X)\n", pp_data->caps[caps_idx].BitField);
					}
					else if (rt_idx == HidP_Feature) {
						rd_write_short_item(rd_main_feature, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Feature (0x%02X)\n", pp_data->caps[caps_idx].BitField);
					}
					report_count = 0;
				}
			}
			else {

				if (last_report_id != pp_data->caps[caps_idx].ReportID) {
					// Write Report ID if changed
					rd_write_short_item(rd_global_report_id, pp_data->caps[caps_idx].ReportID, &byte_list);
					printf("Report ID (%d)\n", pp_data->caps[caps_idx].ReportID);
					last_report_id = pp_data->caps[caps_idx].ReportID;
				}

				// Print usage page when changed
				if (pp_data->caps[caps_idx].UsagePage != last_usage_page) {
					rd_write_short_item(rd_global_usage_page, pp_data->caps[caps_idx].UsagePage, &byte_list);
					printf("Usage Page (%d)\n", pp_data->caps[caps_idx].UsagePage);
					last_usage_page = pp_data->caps[caps_idx].UsagePage;
				}

				if (inhibit_write_of_usage) {
					// Inhibit only once after DELIMITER statement
					inhibit_write_of_usage = FALSE;
				}
				else {
					if (pp_data->caps[caps_idx].IsRange) {
						rd_write_short_item(rd_local_usage_minimum, pp_data->caps[caps_idx].Range.UsageMin, &byte_list);
						printf("Usage Minimum (%d)\n", pp_data->caps[caps_idx].Range.UsageMin);
						rd_write_short_item(rd_local_usage_maximum, pp_data->caps[caps_idx].Range.UsageMax, &byte_list);
						printf("Usage Maximum (%d)\n", pp_data->caps[caps_idx].Range.UsageMax);
					}
					else {
						rd_write_short_item(rd_local_usage, pp_data->caps[caps_idx].NotRange.Usage, &byte_list);
						printf("Usage (%d)\n", pp_data->caps[caps_idx].NotRange.Usage);
					}
				}

				if (pp_data->caps[caps_idx].IsDesignatorRange) {
					rd_write_short_item(rd_local_designator_minimum, pp_data->caps[caps_idx].Range.DesignatorMin, &byte_list);
					printf("Designator Minimum (%d)\n", pp_data->caps[caps_idx].Range.DesignatorMin);
					rd_write_short_item(rd_local_designator_maximum, pp_data->caps[caps_idx].Range.DesignatorMax, &byte_list);
					printf("Designator Maximum (%d)\n", pp_data->caps[caps_idx].Range.DesignatorMax);
				}
				else if (pp_data->caps[caps_idx].NotRange.DesignatorIndex != 0) {
					// Designator set 0 is a special descriptor set (of the HID Physical Descriptor),
					// that specifies the number of additional descriptor sets.
					// Therefore Designator Index 0 can never be a useful reference for a control.
					rd_write_short_item(rd_local_designator_index, pp_data->caps[caps_idx].NotRange.DesignatorIndex, &byte_list);
					printf("Designator Index (%d)\n", pp_data->caps[caps_idx].NotRange.DesignatorIndex);
				}

				if (pp_data->caps[caps_idx].IsStringRange) {
					rd_write_short_item(rd_local_string_minimum, pp_data->caps[caps_idx].Range.StringMin, &byte_list);
					printf("String Minimum (%d)\n", pp_data->caps[caps_idx].Range.StringMin);
					rd_write_short_item(rd_local_string_maximum, pp_data->caps[caps_idx].Range.StringMax, &byte_list);
					printf("String Maximum (%d)\n", pp_data->caps[caps_idx].Range.StringMax);
				}
				else if (pp_data->caps[caps_idx].NotRange.StringIndex != 0) {
					// String Index 0 is a special entry, that contains a list of supported languages,
					// therefore Designator Index 0 can never be a useful reference for a control.
					rd_write_short_item(rd_local_string, pp_data->caps[caps_idx].NotRange.StringIndex, &byte_list);
					printf("String Index (%d)\n", pp_data->caps[caps_idx].NotRange.StringIndex);
				}

				if ((pp_data->caps[caps_idx].BitField & 0x02) != 0x02) {
					// In case of an value array overwrite Report Count
					pp_data->caps[caps_idx].ReportCount = pp_data->caps[caps_idx].Range.DataIndexMax - pp_data->caps[caps_idx].Range.DataIndexMin + 1;
				}

					
				// Print only local report items for each cap, if ReportCount > 1
				if ((main_item_list->next != NULL) &&
					(main_item_list->next->MainItemType == rt_idx) &&
					(main_item_list->next->TypeOfNode == rd_item_node_cap) &&
					(!pp_data->caps[main_item_list->next->CapsIndex].IsButtonCap) &&
					(!pp_data->caps[caps_idx].IsRange) && // This node in list is no array
					(!pp_data->caps[main_item_list->next->CapsIndex].IsRange) && // Next node in list is no array
					(pp_data->caps[main_item_list->next->CapsIndex].UsagePage == pp_data->caps[caps_idx].UsagePage) &&
					(pp_data->caps[main_item_list->next->CapsIndex].NotButton.LogicalMin == pp_data->caps[caps_idx].NotButton.LogicalMin) &&
					(pp_data->caps[main_item_list->next->CapsIndex].NotButton.LogicalMax == pp_data->caps[caps_idx].NotButton.LogicalMax) &&
					(pp_data->caps[main_item_list->next->CapsIndex].NotButton.PhysicalMin == pp_data->caps[caps_idx].NotButton.PhysicalMin) &&
					(pp_data->caps[main_item_list->next->CapsIndex].NotButton.PhysicalMax == pp_data->caps[caps_idx].NotButton.PhysicalMax) &&
					(pp_data->caps[main_item_list->next->CapsIndex].UnitsExp == pp_data->caps[caps_idx].UnitsExp) &&
					(pp_data->caps[main_item_list->next->CapsIndex].Units == pp_data->caps[caps_idx].Units) &&
					(pp_data->caps[main_item_list->next->CapsIndex].BitSize == pp_data->caps[caps_idx].BitSize) &&
					(pp_data->caps[main_item_list->next->CapsIndex].ReportID == pp_data->caps[caps_idx].ReportID) &&
					(pp_data->caps[main_item_list->next->CapsIndex].BitField == pp_data->caps[caps_idx].BitField) &&
					(pp_data->caps[main_item_list->next->CapsIndex].ReportCount == 1) &&
					(pp_data->caps[caps_idx].ReportCount == 1)
					) {
					// Skip global items until any of them changes, than use ReportCount item to write the count of identical report fields
					report_count++;
				}
				else {

					rd_write_short_item(rd_global_logical_minimum, pp_data->caps[caps_idx].NotButton.LogicalMin, &byte_list);
					printf("Logical Minimum (%d)\n", pp_data->caps[caps_idx].NotButton.LogicalMin);

					rd_write_short_item(rd_global_logical_maximum, pp_data->caps[caps_idx].NotButton.LogicalMax, &byte_list);
					printf("Logical Maximum (%d)\n", pp_data->caps[caps_idx].NotButton.LogicalMax);

					if ((last_physical_min != pp_data->caps[caps_idx].NotButton.PhysicalMin) ||
						(last_physical_max != pp_data->caps[caps_idx].NotButton.PhysicalMax)) {
						// Write Physical Min and Max only if one of them changed
						rd_write_short_item(rd_global_physical_minimum, pp_data->caps[caps_idx].NotButton.PhysicalMin, &byte_list);
						printf("Physical Minimum (%d)\n", pp_data->caps[caps_idx].NotButton.PhysicalMin);
						last_physical_min = pp_data->caps[caps_idx].NotButton.PhysicalMin;

						rd_write_short_item(rd_global_physical_maximum, pp_data->caps[caps_idx].NotButton.PhysicalMax, &byte_list);
						printf("Physical Maximum (%d)\n", pp_data->caps[caps_idx].NotButton.PhysicalMax);
						last_physical_max = pp_data->caps[caps_idx].NotButton.PhysicalMax;
					}


					if (last_unit_exponent != pp_data->caps[caps_idx].UnitsExp) {
						// Write Unit Exponent only if changed
						rd_write_short_item(rd_global_unit_exponent, pp_data->caps[caps_idx].UnitsExp, &byte_list);
						printf("Unit Exponent (%d)\n", pp_data->caps[caps_idx].UnitsExp);
						last_unit_exponent = pp_data->caps[caps_idx].UnitsExp;
					}

					if (last_unit != pp_data->caps[caps_idx].Units) {
						// Write Unit only if changed
						rd_write_short_item(rd_global_unit, pp_data->caps[caps_idx].Units, &byte_list);
						printf("Unit (%d)\n", pp_data->caps[caps_idx].Units);
						last_unit = pp_data->caps[caps_idx].Units;
					}

					rd_write_short_item(rd_global_report_size, pp_data->caps[caps_idx].BitSize, &byte_list);
					printf("Report Size (%d)\n", pp_data->caps[caps_idx].BitSize);

					rd_write_short_item(rd_global_report_count, pp_data->caps[caps_idx].ReportCount + report_count, &byte_list);
					printf("Report Count (%d)\n", pp_data->caps[caps_idx].ReportCount + report_count);

					if (rt_idx == HidP_Input) {
						rd_write_short_item(rd_main_input, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Input (0x%02X)\n", pp_data->caps[caps_idx].BitField);
					}
					else if (rt_idx == HidP_Output) {
						rd_write_short_item(rd_main_output, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Output (0x%02X)\n", pp_data->caps[caps_idx].BitField);
					}
					else if (rt_idx == HidP_Feature) {
						rd_write_short_item(rd_main_feature, pp_data->caps[caps_idx].BitField, &byte_list);
						printf("Feature (0x%02X)\n", pp_data->caps[caps_idx].BitField);
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
	free(link_collection_nodes);

	if (byte_list_len > buf_size) {
		return -1;
	}
	else {
		return byte_list_len;
	}
}

int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device* dev, unsigned char* buf, size_t buf_size)
{
	PHIDP_PREPARSED_DATA pp_data = NULL;

	if (!HidD_GetPreparsedData(dev->device_handle, &pp_data)) {
		register_error(dev, "HidD_GetPreparsedData");
		return -1;
	}
	else {
		int res;
		res = reconstruct_report_descriptor(dev, pp_data, buf, buf_size);

		HidD_FreePreparsedData(pp_data);

		return res;
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
