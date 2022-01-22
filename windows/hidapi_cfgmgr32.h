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

#ifndef HIDAPI_CFGMGR32_H
#define HIDAPI_CFGMGR32_H

#ifdef HIDAPI_USE_DDK

#include <cfgmgr32.h>

#else

/* This part of the header mimics cfgmgr32.h,
    but only what is used by HIDAPI */

typedef DWORD RETURN_TYPE;
typedef RETURN_TYPE CONFIGRET;
typedef DWORD DEVNODE, DEVINST;
typedef DEVNODE* PDEVNODE, * PDEVINST;
typedef WCHAR* DEVNODEID_W, * DEVINSTID_W;

#define CR_SUCCESS (0x00000000)
#define CR_BUFFER_SMALL (0x0000001A)

#define CM_LOCATE_DEVNODE_NORMAL 0x00000000

#define CM_GET_DEVICE_INTERFACE_LIST_PRESENT (0x00000000)

typedef CONFIGRET(__stdcall* CM_Locate_DevNodeW_)(PDEVINST pdnDevInst, DEVINSTID_W pDeviceID, ULONG ulFlags);
typedef CONFIGRET(__stdcall* CM_Get_Parent_)(PDEVINST pdnDevInst, DEVINST dnDevInst, ULONG ulFlags);
typedef CONFIGRET(__stdcall* CM_Get_DevNode_PropertyW_)(DEVINST dnDevInst, CONST DEVPROPKEY* PropertyKey, DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer, PULONG PropertyBufferSize, ULONG ulFlags);
typedef CONFIGRET(__stdcall* CM_Get_Device_Interface_PropertyW_)(LPCWSTR pszDeviceInterface, CONST DEVPROPKEY* PropertyKey, DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer, PULONG PropertyBufferSize, ULONG ulFlags);
typedef CONFIGRET(__stdcall* CM_Get_Device_Interface_List_SizeW_)(PULONG pulLen, LPGUID InterfaceClassGuid, DEVINSTID_W pDeviceID, ULONG ulFlags);
typedef CONFIGRET(__stdcall* CM_Get_Device_Interface_ListW_)(LPGUID InterfaceClassGuid, DEVINSTID_W pDeviceID, PZZWSTR Buffer, ULONG BufferLen, ULONG ulFlags);

#endif

#endif /* HIDAPI_CFGMGR32_H */
