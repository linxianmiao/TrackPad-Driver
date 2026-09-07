#pragma once
#include <ntifs.h>
#include <wdf.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>
#include <vhf.h>
#include "amtptp_source.h"

#define SOURCE_MTU 672u
#define SOURCE_IOCTL_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
/* Status contains counters only; no address, serial, or raw coordinates. */
typedef struct SOURCE_STATUS {
    ULONG Size, Version;
    LONG Connected, ModeEnabled, LastStatus;
    LONG Packets, TouchPackets, InvalidPackets, Reports, Reconnects;
    ULONG InputMode, SurfaceEnabled, ButtonEnabled;
} SOURCE_STATUS;

typedef struct SOURCE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET Target;
    WDFREQUEST Request;
    WDFMEMORY BrbMemory;
    BRB Brb;
    BTH_PROFILE_DRIVER_INTERFACE Profile;
    BOOLEAN InterfaceAcquired;
    BTH_ADDR Address;
    L2CAP_CHANNEL_HANDLE ControlChannel, InterruptChannel;
    HANDLE Thread;
    KEVENT StopEvent, CompletedEvent;
    NTSTATUS CompletionStatus;
    volatile LONG Disconnected;
    SOURCE_STATUS Status;
    KSPIN_LOCK StateLock;
    amtptp_source Core;
    VHFHANDLE Vhf;
    UCHAR ReceiveBuffer[SOURCE_MTU];
} SOURCE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SOURCE_CONTEXT, SourceGetContext);

NTSTATUS SourceBluetoothPrepare(SOURCE_CONTEXT *Context);
VOID SourceBluetoothRelease(SOURCE_CONTEXT *Context);
NTSTATUS SourceBluetoothStart(SOURCE_CONTEXT *Context);
VOID SourceBluetoothStop(SOURCE_CONTEXT *Context);
NTSTATUS SourceVhfCreate(SOURCE_CONTEXT *Context);
VOID SourceVhfDelete(SOURCE_CONTEXT *Context);
VOID SourceVhfInput(SOURCE_CONTEXT *Context, const UCHAR *Buffer, ULONG Length);
VOID SourceVhfReleaseTouches(SOURCE_CONTEXT *Context, BOOLEAN OnlyIfPending);
