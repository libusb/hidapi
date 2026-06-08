/*++

Copyright (C) Microsoft Corporation, All Rights Reserved

Module Name:

    vhidmini.h

Abstract:

    This module contains the type definitions for the driver

Environment:

    Windows Driver Framework (WDF)

--*/

/*
 * Modified by the libusb/hidapi team for the HIDAPI virtual-device tests
 * (implements the HIDAPI "scenario" protocol; see
 * src/tests/test_virtual_device.h). Derived from the vhidmini2 sample in
 * microsoft/Windows-driver-samples, which is licensed under the Microsoft
 * Public License (MS-PL); see README.md and LICENSE.txt in this directory.
 */

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include <wdf.h>

#include <hidport.h>  // located in $(DDK_INC_PATH)/wdm

#include "common.h"

typedef UCHAR HID_REPORT_DESCRIPTOR, *PHID_REPORT_DESCRIPTOR;

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           EvtDeviceAdd;
EVT_WDF_TIMER                       EvtTimerFunc;

typedef struct _DEVICE_CONTEXT
{
    WDFDEVICE               Device;
    WDFQUEUE                DefaultQueue;
    WDFQUEUE                ManualQueue;
    HID_DEVICE_ATTRIBUTES   HidDeviceAttributes;
    BYTE                    DeviceData;
    HID_DESCRIPTOR          HidDescriptor;
    PHID_REPORT_DESCRIPTOR  ReportDescriptor;
    BOOLEAN                 ReadReportDescFromRegistry;

    //
    // HIDAPI test scenario state. SetFeature() stores the requested scenario
    // command here (see test_virtual_device.h); the manual-queue timer replays
    // the matching pre-recorded input report and clears it. Accessed from the
    // IO thread and the timer thread via Interlocked*.
    //
    volatile LONG           ScenarioCommand;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

typedef struct _QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;
    UCHAR                   OutputReport;

} QUEUE_CONTEXT, *PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, GetQueueContext);

NTSTATUS
QueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    );

typedef struct _MANUAL_QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;
    WDFTIMER                Timer;

} MANUAL_QUEUE_CONTEXT, *PMANUAL_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MANUAL_QUEUE_CONTEXT, GetManualQueueContext);

NTSTATUS
ManualQueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    );

NTSTATUS
ReadReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request,
    _Always_(_Out_)
          BOOLEAN*          CompleteRequest
    );

NTSTATUS
WriteReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
SetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetInputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
SetOutputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetString(
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetIndexedString(
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetStringId(
    _In_  WDFREQUEST        Request,
    _Out_ ULONG            *StringId,
    _Out_ ULONG            *LanguageId
    );

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
    _In_  size_t            NumBytesToCopyFrom
    );

NTSTATUS
RequestGetHidXferPacket_ToReadFromDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET  *Packet
    );

NTSTATUS
RequestGetHidXferPacket_ToWriteToDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET  *Packet
    );

NTSTATUS
CheckRegistryForDescriptor(
    _In_ WDFDEVICE Device
    );

NTSTATUS
ReadDescriptorFromRegistry(
    _In_ WDFDEVICE Device
    );

//
// Misc definitions
//
#define CONTROL_FEATURE_REPORT_ID   0x01

//
// These are the device attributes returned by the mini driver in response
// to IOCTL_HID_GET_DEVICE_ATTRIBUTES.
//
// VID/PID match the HIDAPI test (see src/tests/test_read_interrupt.c).
#define HIDMINI_PID             0x9001
#define HIDMINI_VID             0xF1D0
#define HIDMINI_VERSION         0x0101

//
// HIDAPI test scenario protocol (kept in sync with src/tests/test_virtual_device.h).
// A Feature SET_REPORT whose payload contains one of these commands makes the
// device replay the matching 8-byte input report.
//
#define TEST_VDEV_REPORT_SIZE   8
#define TEST_VDEV_CMD_EMIT_A    0x01
#define TEST_VDEV_CMD_EMIT_B    0x02
#define TEST_VDEV_INPUT_A_BYTES 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8
#define TEST_VDEV_INPUT_B_BYTES 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8
