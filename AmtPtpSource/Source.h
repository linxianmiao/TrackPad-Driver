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
    /* v2 tail: protocol state only, never report payloads or device addresses. */
    LONG TransportStage, FailureStage, FailureBrbStatus, FailureBtStatus, FailureBrbType;
    LONG ControlOpens, InterruptOpens, FeatureWrites, HandshakeCode;
    /* v3: unknown/stale/disconnected percent is -1, never a fabricated 0%. */
    LONG BatteryValid, BatteryPercent, BatteryFlags, BatteryAgeSeconds;
    LONG BatteryReports, BatteryQueries, BatteryLastStatus;
    LONG BatteryWriteStatus, BatteryReadStatus, BatteryReadLength, BatteryReadRemaining;
    ULONG BatteryResponse; /* Only an A1 90 battery response, never touch data. */
    /* v5: native Bluetooth device property publishing, independent of I/O. */
    LONG BatteryPropertyStatus, BatteryPropertyPercent, BatteryPropertyUpdates;
} SOURCE_STATUS;
C_ASSERT(sizeof(SOURCE_STATUS) == 148);

typedef struct SOURCE_BATTERY_IO {
    WDFREQUEST Request;
    WDFMEMORY Memory;
    BRB Brb;
    WDFREQUEST WriteRequest;
    WDFMEMORY WriteMemory;
    BRB WriteBrb;
    KEVENT WriteCompleted;
    NTSTATUS WriteStatus;
    BOOLEAN WritePending;
    UCHAR Command[2];
    KEVENT Completed;
    NTSTATUS CompletionStatus;
    ULONG Reads;
    BOOLEAN Pending;
    ULONGLONG NextQuery, Deadline, UpdatedAt;
    ULONGLONG PropertyRetryAt;
    BOOLEAN PropertyInitialized;
    UCHAR Buffer[SOURCE_MTU];
} SOURCE_BATTERY_IO;

enum SOURCE_TRANSPORT_STAGE {
    SourceStageIdle, SourceStageControl, SourceStageInterrupt, SourceStageModeWrite,
    SourceStageHandshake, SourceStageInput, SourceStageClose, SourceStageRetry, SourceStageStopped
};

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
    SOURCE_BATTERY_IO Battery;
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
NTSTATUS SourceBatteryCreate(SOURCE_CONTEXT *Context);
VOID SourceBatteryPump(SOURCE_CONTEXT *Context);
VOID SourceBatteryStop(SOURCE_CONTEXT *Context);
BOOLEAN SourceBatteryInput(SOURCE_CONTEXT *Context, const UCHAR *Buffer, ULONG Length);
VOID SourceBatterySnapshot(SOURCE_CONTEXT *Context, SOURCE_STATUS *Status);
