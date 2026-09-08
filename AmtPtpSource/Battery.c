#include "Source.h"

#define BATTERY_SECOND 10000000ULL
#define BATTERY_INTERVAL (60ULL * BATTERY_SECOND)
#define BATTERY_STALE_SECONDS 180ULL

/* Windows Bluetooth battery property. Verified on this classic Bluetooth HID
 * service devnode: Settings aggregates it into the physical device card.
 * It is distinct from the Shell's PKEY_Devices_BatteryLife (49CD1F76..., 10).
 * A failed property update must never tear down a working touch connection. */
static const DEVPROPKEY BluetoothBattery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2
};

static VOID Publish(SOURCE_CONTEXT *Context, LONG Percent, BOOLEAN Force)
{
    SOURCE_BATTERY_IO *io = &Context->Battery;
    WDF_DEVICE_PROPERTY_DATA property;
    NTSTATUS status;
    UCHAR value = Percent >= 0 ? (UCHAR)Percent : 0;
    ULONGLONG now = KeQueryInterruptTime();
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    NT_ASSERT(Percent >= -1 && Percent <= 100);
    if (!Force && ((io->PropertyInitialized && Context->Status.BatteryPropertyPercent == Percent) ||
        now < io->PropertyRetryAt)) return;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &BluetoothBattery);
    /* WDF documents persistent assignment. Explicitly delete at startup,
     * expiration and orderly disconnect/suspend so old readings do not linger. */
    property.Flags = PLUGPLAY_PROPERTY_PERSISTENT;
    status = WdfDeviceAssignProperty(Context->Device, &property,
        Percent >= 0 ? DEVPROP_TYPE_BYTE : DEVPROP_TYPE_EMPTY,
        Percent >= 0 ? sizeof(value) : 0, Percent >= 0 ? &value : NULL);
    if (Percent < 0 && (status == STATUS_NOT_FOUND || status == STATUS_OBJECT_NAME_NOT_FOUND))
        status = STATUS_SUCCESS;
    InterlockedExchange(&Context->Status.BatteryPropertyStatus, status);
    InterlockedIncrement(&Context->Status.BatteryPropertyUpdates);
    io->PropertyInitialized = NT_SUCCESS(status);
    io->PropertyRetryAt = NT_SUCCESS(status) ? 0 : now + BATTERY_INTERVAL;
    if (NT_SUCCESS(status)) InterlockedExchange(&Context->Status.BatteryPropertyPercent, Percent);
}

EVT_WDF_REQUEST_COMPLETION_ROUTINE SourceBatteryComplete;

VOID SourceBatteryComplete(WDFREQUEST Request, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Parameters, WDFCONTEXT CompletionContext)
{
    SOURCE_CONTEXT *context = CompletionContext;
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    if (Request == context->Battery.WriteRequest) {
        context->Battery.WriteStatus = Parameters->IoStatus.Status;
        KeSetEvent(&context->Battery.WriteCompleted, IO_NO_INCREMENT, FALSE);
    } else {
        context->Battery.CompletionStatus = Parameters->IoStatus.Status;
        KeSetEvent(&context->Battery.Completed, IO_NO_INCREMENT, FALSE);
    }
}

NTSTATUS SourceBatteryCreate(SOURCE_CONTEXT *Context)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    KeInitializeEvent(&Context->Battery.Completed, NotificationEvent, FALSE);
    KeInitializeEvent(&Context->Battery.WriteCompleted, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Context->Device;
    status = WdfRequestCreate(&attributes, Context->Target, &Context->Battery.Request);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRequestAllocateTimer(Context->Battery.Request);
    if (!NT_SUCCESS(status)) return status;
    status = WdfMemoryCreatePreallocated(&attributes, &Context->Battery.Brb,
        sizeof(Context->Battery.Brb), &Context->Battery.Memory);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRequestCreate(&attributes, Context->Target, &Context->Battery.WriteRequest);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRequestAllocateTimer(Context->Battery.WriteRequest);
    if (!NT_SUCCESS(status)) return status;
    status = WdfMemoryCreatePreallocated(&attributes, &Context->Battery.WriteBrb,
        sizeof(Context->Battery.WriteBrb), &Context->Battery.WriteMemory);
    if (NT_SUCCESS(status)) Publish(Context, -1, TRUE);
    return status;
}

BOOLEAN SourceBatteryInput(SOURCE_CONTEXT *Context, const UCHAR *Buffer, ULONG Length)
{
    UCHAR percent, flags;
    KIRQL irql;
    if (amtptp_source_battery(Buffer, Length, &percent, &flags) != AMTPTP_SOURCE_REPORT)
        return FALSE;
    KeAcquireSpinLock(&Context->StateLock, &irql);
    Context->Status.BatteryValid = 1;
    Context->Status.BatteryPercent = percent;
    Context->Status.BatteryFlags = flags;
    Context->Battery.UpdatedAt = KeQueryInterruptTime();
    KeReleaseSpinLock(&Context->StateLock, irql);
    InterlockedIncrement(&Context->Status.BatteryReports);
    InterlockedExchange(&Context->Status.BatteryLastStatus, STATUS_SUCCESS);
    /* Only the passive Bluetooth producer calls Input; never assign properties
     * under StateLock, from request completion, or from the read-only IOCTL. */
    Publish(Context, percent, FALSE);
    return TRUE;
}

/* Caller holds StateLock. Age includes suspended time; stale values cannot
 * reappear as fresh after sleep, or masquerade as an empty battery. */
VOID SourceBatterySnapshot(SOURCE_CONTEXT *Context, SOURCE_STATUS *Status)
{
    ULONGLONG age = (KeQueryInterruptTime() - Context->Battery.UpdatedAt) / BATTERY_SECOND;
    Status->BatteryValid = Context->Status.BatteryValid && Status->Connected && age < BATTERY_STALE_SECONDS;
    Status->BatteryPercent = Status->BatteryValid ? Context->Status.BatteryPercent : -1;
    Status->BatteryFlags = Status->BatteryValid ? Context->Status.BatteryFlags : -1;
    Status->BatteryAgeSeconds = Context->Status.BatteryValid ? (LONG)min(age, MAXLONG) : -1;
}

static BOOLEAN Submit(SOURCE_CONTEXT *Context, BOOLEAN Write)
{
    SOURCE_BATTERY_IO *io = &Context->Battery;
    BRB *storage = Write ? &io->WriteBrb : &io->Brb;
    struct _BRB_L2CA_ACL_TRANSFER *brb = &storage->BrbL2caAclTransfer;
    WDFREQUEST request = Write ? io->WriteRequest : io->Request;
    WDFMEMORY memory = Write ? io->WriteMemory : io->Memory;
    BOOLEAN *pending = Write ? &io->WritePending : &io->Pending;
    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_SEND_OPTIONS options;
    WDFMEMORY_OFFSET offset = {0, sizeof(*brb)};
    NTSTATUS status;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_UNSUCCESSFUL);
    status = WdfRequestReuse(request, &reuse);
    if (!NT_SUCCESS(status)) goto Failed;
    RtlZeroMemory(storage, sizeof(*storage));
    Context->Profile.BthInitializeBrb(storage, BRB_L2CA_ACL_TRANSFER);
    brb->BtAddress = Context->Address;
    brb->ChannelHandle = Context->ControlChannel;
    brb->Buffer = Write ? io->Command : io->Buffer;
    brb->BufferSize = Write ? sizeof(io->Command) : sizeof(io->Buffer);
    brb->TransferFlags = Write ? ACL_TRANSFER_DIRECTION_OUT :
        ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK | ACL_TRANSFER_TIMEOUT;
    brb->Timeout = 5000; /* BTH ACL timeouts are milliseconds. */
    if (Write) {
        io->Command[0] = 0x41; /* HIDP GET_REPORT, Input, no size parameter. */
        io->Command[1] = 0x90;
    } else RtlZeroMemory(io->Buffer, sizeof(io->Buffer));
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(Context->Target, request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, memory, &offset, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) goto Failed;
    *pending = TRUE;
    KeClearEvent(Write ? &io->WriteCompleted : &io->Completed);
    WdfRequestSetCompletionRoutine(request, SourceBatteryComplete, Context);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(Write ? 2000 : 6000));
    if (WdfRequestSend(request, Context->Target, &options)) return TRUE;
    status = WdfRequestGetStatus(request);
    *pending = FALSE;
Failed:
    InterlockedExchange(&Context->Status.BatteryLastStatus, status);
    InterlockedExchange(Write ? &Context->Status.BatteryWriteStatus : &Context->Status.BatteryReadStatus, status);
    return FALSE;
}

/* Only the Bluetooth producer calls Pump/Stop. Queries are asynchronous on
 * control PSM 0x11 while touch input continues independently on PSM 0x13. */
VOID SourceBatteryPump(SOURCE_CONTEXT *Context)
{
    SOURCE_BATTERY_IO *io = &Context->Battery;
    ULONGLONG now = KeQueryInterruptTime();
    SOURCE_STATUS snapshot;
    KIRQL irql;
    KeAcquireSpinLock(&Context->StateLock, &irql);
    snapshot = Context->Status;
    SourceBatterySnapshot(Context, &snapshot);
    KeReleaseSpinLock(&Context->StateLock, irql);
    Publish(Context, snapshot.BatteryPercent, FALSE);
    if (io->WritePending && KeReadStateEvent(&io->WriteCompleted)) {
        io->WritePending = FALSE;
        InterlockedExchange(&Context->Status.BatteryWriteStatus, io->WriteStatus);
        if (io->WriteStatus != STATUS_SUCCESS || io->WriteBrb.BrbL2caAclTransfer.BufferSize != sizeof(io->Command)) {
            InterlockedExchange(&Context->Status.BatteryLastStatus,
                io->WriteStatus != STATUS_SUCCESS ? io->WriteStatus : STATUS_BUFFER_OVERFLOW);
            if (io->Pending) (void)WdfRequestCancelSentRequest(io->Request);
        }
    }
    if (io->Pending) {
        ULONG length;
        if (!KeReadStateEvent(&io->Completed)) return;
        io->Pending = FALSE;
        InterlockedExchange(&Context->Status.BatteryReadStatus, io->CompletionStatus);
        InterlockedExchange(&Context->Status.BatteryReadLength, io->Brb.BrbL2caAclTransfer.BufferSize);
        InterlockedExchange(&Context->Status.BatteryReadRemaining, io->Brb.BrbL2caAclTransfer.RemainingBufferSize);
        /* Diagnostics only: a failed buffer is never accepted as a reading.
         * Filter the header so the status interface cannot expose touch data. */
        Context->Status.BatteryResponse = io->Buffer[0] == 0xa1 && io->Buffer[1] == 0x90 ?
            (ULONG)io->Buffer[0] | ((ULONG)io->Buffer[1] << 8) |
            ((ULONG)io->Buffer[2] << 16) | ((ULONG)io->Buffer[3] << 24) : 0;
        if (io->CompletionStatus != STATUS_SUCCESS) {
            InterlockedExchange(&Context->Status.BatteryLastStatus, io->CompletionStatus);
            return;
        }
        length = io->Brb.BrbL2caAclTransfer.BufferSize;
        if (length > sizeof(io->Buffer)) {
            InterlockedExchange(&Context->Status.BatteryLastStatus, STATUS_BUFFER_OVERFLOW);
            return;
        }
        if (SourceBatteryInput(Context, io->Buffer, length)) return;
        /* Drain a late mode handshake, but never parse it as battery data. */
        if (++io->Reads >= 4 || now >= io->Deadline) {
            InterlockedExchange(&Context->Status.BatteryLastStatus, STATUS_DEVICE_PROTOCOL_ERROR);
            return;
        }
        (void)Submit(Context, FALSE);
        return;
    }
    if (io->WritePending || now < io->NextQuery) return;
    io->NextQuery = now + BATTERY_INTERVAL;
    io->Deadline = now + 5 * BATTERY_SECOND;
    io->Reads = 0;
    InterlockedIncrement(&Context->Status.BatteryQueries);
    /* Arm reception before sending: the device can reply immediately. */
    if (Submit(Context, FALSE) && !Submit(Context, TRUE))
        (void)WdfRequestCancelSentRequest(io->Request);
}

VOID SourceBatteryStop(SOURCE_CONTEXT *Context)
{
    SOURCE_BATTERY_IO *io = &Context->Battery;
    KIRQL irql;
    if (io->WritePending) (void)WdfRequestCancelSentRequest(io->WriteRequest);
    if (io->Pending) (void)WdfRequestCancelSentRequest(io->Request);
    if (io->WritePending) {
        (void)KeWaitForSingleObject(&io->WriteCompleted, Executive, KernelMode, FALSE, NULL);
        io->WritePending = FALSE;
    }
    if (io->Pending) {
        (void)KeWaitForSingleObject(&io->Completed, Executive, KernelMode, FALSE, NULL);
        io->Pending = FALSE;
    }
    io->NextQuery = 0;
    KeAcquireSpinLock(&Context->StateLock, &irql);
    Context->Status.BatteryValid = 0;
    Context->Status.BatteryPercent = -1;
    Context->Status.BatteryFlags = -1;
    KeReleaseSpinLock(&Context->StateLock, irql);
    Publish(Context, -1, TRUE);
    RtlSecureZeroMemory(io->Buffer, sizeof(io->Buffer));
}
