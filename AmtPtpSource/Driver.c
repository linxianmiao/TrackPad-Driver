#include <initguid.h>
#include "Source.h"

/* This interface exposes only SOURCE_IOCTL_STATUS. */
DEFINE_GUID(GUID_DEVINTERFACE_AMTPTP_SOURCE,
    0x84f01477,0x84a8,0x46d6,0x89,0x8b,0x73,0xa4,0x90,0x7e,0xdd,0x46);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SourceDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SourcePrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE SourceRelease;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SourceStart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART SourceRestart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND SourceSuspend;
EVT_WDF_OBJECT_CONTEXT_CLEANUP SourceCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SourceDeviceControl;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SourceDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS SourceDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_IO_QUEUE_CONFIG queue;
    SOURCE_CONTEXT *context;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = SourcePrepare;
    callbacks.EvtDeviceReleaseHardware = SourceRelease;
    callbacks.EvtDeviceSelfManagedIoInit = SourceStart;
    callbacks.EvtDeviceSelfManagedIoRestart = SourceRestart;
    callbacks.EvtDeviceSelfManagedIoSuspend = SourceSuspend;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &callbacks);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SOURCE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    attributes.EvtCleanupCallback = SourceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    context = SourceGetContext(device);
    context->Device = device;
    context->Target = WdfDeviceGetIoTarget(device);
    KeInitializeSpinLock(&context->StateLock);
    KeInitializeEvent(&context->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&context->CompletedEvent, NotificationEvent, FALSE);
    amtptp_source_init(&context->Core);
    context->Status.Size = sizeof(SOURCE_STATUS);
    context->Status.Version = 5;
    context->Status.HandshakeCode = -1;
    context->Status.BatteryPercent = -1;
    context->Status.BatteryFlags = -1;
    context->Status.BatteryAgeSeconds = -1;
    context->Status.BatteryPropertyPercent = -1;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfRequestCreate(&attributes, context->Target, &context->Request);
    if (!NT_SUCCESS(status)) return status;
    status = WdfMemoryCreatePreallocated(&attributes, &context->Brb, sizeof(context->Brb), &context->BrbMemory);
    if (!NT_SUCCESS(status)) return status;
    status = SourceBatteryCreate(context);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = SourceDeviceControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_AMTPTP_SOURCE, NULL);
    if (!NT_SUCCESS(status)) return status;
    return SourceVhfCreate(context);
}

NTSTATUS SourcePrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw); UNREFERENCED_PARAMETER(Translated);
    return SourceBluetoothPrepare(SourceGetContext(Device));
}
NTSTATUS SourceRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    SourceBluetoothStop(SourceGetContext(Device));
    SourceBluetoothRelease(SourceGetContext(Device));
    return STATUS_SUCCESS;
}
NTSTATUS SourceStart(WDFDEVICE Device) { return SourceBluetoothStart(SourceGetContext(Device)); }
NTSTATUS SourceRestart(WDFDEVICE Device) { return SourceBluetoothStart(SourceGetContext(Device)); }
NTSTATUS SourceSuspend(WDFDEVICE Device)
{
    SourceBluetoothStop(SourceGetContext(Device));
    return STATUS_SUCCESS;
}
VOID SourceCleanup(WDFOBJECT Object)
{
    SOURCE_CONTEXT *context = SourceGetContext(Object);
    /* The producer and all request completions leave before VHF is deleted. */
    SourceBluetoothStop(context);
    SourceVhfDelete(context);
    SourceBluetoothRelease(context);
}
VOID SourceDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength,
    size_t InLength, ULONG Code)
{
    SOURCE_CONTEXT *context = SourceGetContext(WdfIoQueueGetDevice(Queue));
    SOURCE_STATUS *result;
    KIRQL irql;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(InLength);
    if (Code != SOURCE_IOCTL_STATUS) { WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return; }
    if (OutLength < sizeof(*result)) { WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return; }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*result), (PVOID *)&result, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    /* Individual counters are approximate, not a transaction snapshot. */
    *result = context->Status;
    KeAcquireSpinLock(&context->StateLock, &irql);
    result->InputMode = context->Core.input_mode;
    result->SurfaceEnabled = context->Core.surface_enabled;
    result->ButtonEnabled = context->Core.button_enabled;
    SourceBatterySnapshot(context, result);
    KeReleaseSpinLock(&context->StateLock, irql);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*result));
}
