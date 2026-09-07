#include "Source.h"

static const GUID HidService = {0x00001124,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
EVT_WDF_REQUEST_COMPLETION_ROUTINE SourceBrbComplete;
KSTART_ROUTINE SourceThread;

static BOOLEAN Stopping(SOURCE_CONTEXT *Context) { return KeReadStateEvent(&Context->StopEvent) != 0; }

VOID SourceBrbComplete(WDFREQUEST Request, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Parameters, WDFCONTEXT CompletionContext)
{
    SOURCE_CONTEXT *context = CompletionContext;
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target);
    context->CompletionStatus = Parameters->IoStatus.Status;
    KeSetEvent(&context->CompletedEvent, IO_NO_INCREMENT, FALSE);
}

/* One worker owns the request, BRB, and buffer. Even on timeout/stop, await
 * completion after cancellation before reusing any of them. */
static NTSTATUS SendBrb(SOURCE_CONTEXT *Context, size_t Size, ULONG TimeoutMs, BOOLEAN StopSensitive)
{
    WDF_REQUEST_REUSE_PARAMS reuse;
    WDF_REQUEST_SEND_OPTIONS options;
    WDFMEMORY_OFFSET offset;
    PVOID events[2] = {&Context->CompletedEvent, &Context->StopEvent};
    NTSTATUS status;
    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_UNSUCCESSFUL);
    status = WdfRequestReuse(Context->Request, &reuse);
    if (!NT_SUCCESS(status)) return status;
    offset.BufferOffset = 0;
    offset.BufferLength = Size;
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(Context->Target, Context->Request,
        IOCTL_INTERNAL_BTH_SUBMIT_BRB, Context->BrbMemory, &offset, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    KeClearEvent(&Context->CompletedEvent);
    WdfRequestSetCompletionRoutine(Context->Request, SourceBrbComplete, Context);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(TimeoutMs));
    if (!WdfRequestSend(Context->Request, Context->Target, &options)) return WdfRequestGetStatus(Context->Request);
    status = KeWaitForMultipleObjects(StopSensitive ? 2 : 1, events, WaitAny,
        Executive, KernelMode, FALSE, NULL, NULL);
    if (status == STATUS_WAIT_1) {
        (void)WdfRequestCancelSentRequest(Context->Request);
        (void)KeWaitForSingleObject(&Context->CompletedEvent, Executive, KernelMode, FALSE, NULL);
    }
    return Context->CompletionStatus;
}

static VOID InitBrb(SOURCE_CONTEXT *Context, BRB_TYPE Type)
{
    RtlZeroMemory(&Context->Brb, sizeof(Context->Brb));
    Context->Profile.BthInitializeBrb(&Context->Brb, Type);
}

static VOID ChannelIndication(PVOID CallbackContext, INDICATION_CODE Indication, PINDICATION_PARAMETERS Parameters)
{
    SOURCE_CONTEXT *context = CallbackContext;
    UNREFERENCED_PARAMETER(Parameters);
    if (context == NULL) return;
    switch (Indication) {
    case IndicationAddReference: WdfObjectReference(context->Device); break;
    case IndicationReleaseReference: WdfObjectDereference(context->Device); break;
    case IndicationRemoteDisconnect: InterlockedExchange(&context->Disconnected, 1); break;
    default: break; /* No optional configuration buffers or server callback. */
    }
}

static NTSTATUS OpenChannel(SOURCE_CONTEXT *Context, USHORT Psm, L2CAP_CHANNEL_HANDLE *Handle)
{
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    NTSTATUS status;
    InitBrb(Context, BRB_L2CA_OPEN_CHANNEL);
    brb = &Context->Brb.BrbL2caOpenChannel;
    brb->BtAddress = Context->Address;
    brb->Psm = Psm;
    brb->ChannelFlags = CF_ROLE_EITHER | CF_LINK_ENCRYPTED;
    brb->ConfigOut.Flags = CFG_MTU;
    brb->ConfigOut.Mtu.Min = L2CAP_MIN_MTU;
    brb->ConfigOut.Mtu.Preferred = SOURCE_MTU;
    brb->ConfigOut.Mtu.Max = SOURCE_MTU;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu = brb->ConfigOut.Mtu;
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = ChannelIndication;
    brb->CallbackContext = Context;
    brb->ReferenceObject = WdfDeviceWdmGetDeviceObject(Context->Device);
    brb->IncomingQueueDepth = 10;
    status = SendBrb(Context, sizeof(*brb), 5000, TRUE);
    if (status == STATUS_SUCCESS) *Handle = brb->ChannelHandle;
    return status;
}

static NTSTATUS CloseChannel(SOURCE_CONTEXT *Context, L2CAP_CHANNEL_HANDLE *Handle)
{
    NTSTATUS status;
    struct _BRB_L2CA_CLOSE_CHANNEL *brb;
    if (*Handle == NULL) return STATUS_SUCCESS;
    InitBrb(Context, BRB_L2CA_CLOSE_CHANNEL);
    brb = &Context->Brb.BrbL2caCloseChannel;
    brb->BtAddress = Context->Address;
    brb->ChannelHandle = *Handle;
    /* A close is cleanup: it must not be cancelled merely because Stop is set. */
    status = SendBrb(Context, sizeof(*brb), 5000, FALSE);
    if (status == STATUS_SUCCESS || status == STATUS_INVALID_HANDLE ||
        status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_CONNECTION_DISCONNECTED) *Handle = NULL;
    return status;
}

static NTSTATUS Transfer(SOURCE_CONTEXT *Context, L2CAP_CHANNEL_HANDLE Handle,
    PVOID Buffer, ULONG *Length, BOOLEAN Read, ULONG TimeoutMs)
{
    struct _BRB_L2CA_ACL_TRANSFER *brb;
    NTSTATUS status;
    ULONG capacity = *Length;
    InitBrb(Context, BRB_L2CA_ACL_TRANSFER);
    brb = &Context->Brb.BrbL2caAclTransfer;
    brb->BtAddress = Context->Address;
    brb->ChannelHandle = Handle;
    brb->Buffer = Buffer;
    brb->BufferSize = capacity;
    brb->TransferFlags = Read ? ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK | ACL_TRANSFER_TIMEOUT : ACL_TRANSFER_DIRECTION_OUT;
    brb->Timeout = TimeoutMs;
    status = SendBrb(Context, sizeof(*brb), TimeoutMs + 1000, TRUE);
    if (status == STATUS_SUCCESS) {
        /* With ACL_SHORT_TRANSFER_OK, a successful read can be shorter than
         * the supplied buffer. BufferSize is the documented transferred size. */
        if (brb->BufferSize > capacity || (!Read && brb->BufferSize != capacity)) return STATUS_BUFFER_OVERFLOW;
        *Length = brb->BufferSize;
    }
    return status;
}

NTSTATUS SourceBluetoothPrepare(SOURCE_CONTEXT *Context)
{
    BTH_ENUMERATOR_INFO enumeration = {0};
    BTH_DEVICE_INFO device = {0};
    WDF_MEMORY_DESCRIPTOR output;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &enumeration, sizeof(enumeration));
    status = WdfIoTargetSendInternalIoctlSynchronously(Context->Target, NULL,
        IOCTL_INTERNAL_BTHENUM_GET_ENUMINFO, NULL, &output, &options, NULL);
    if (status != STATUS_SUCCESS) return status;
    /* A second binding check, independent of the exact device-specific INF. */
    if (enumeration.Vid != 0x004c || enumeration.Pid != 0x0324 || enumeration.VidType != 1 ||
        !IsEqualGUID(&enumeration.Guid, &HidService)) return STATUS_NOT_SUPPORTED;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &device, sizeof(device));
    status = WdfIoTargetSendInternalIoctlSynchronously(Context->Target, NULL,
        IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &output, &options, NULL);
    if (status != STATUS_SUCCESS || device.address == 0) return STATUS_DEVICE_NOT_CONNECTED;
    Context->Address = device.address;
    status = WdfFdoQueryForInterface(Context->Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
        (PINTERFACE)&Context->Profile, sizeof(Context->Profile), BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (NT_SUCCESS(status)) Context->InterfaceAcquired = TRUE;
    return status;
}

VOID SourceBluetoothRelease(SOURCE_CONTEXT *Context)
{
    if (Context->InterfaceAcquired) {
        Context->Profile.Interface.InterfaceDereference(Context->Profile.Interface.Context);
        Context->InterfaceAcquired = FALSE;
    }
}

VOID SourceThread(PVOID StartContext)
{
    SOURCE_CONTEXT *context = StartContext;
    LARGE_INTEGER retry;
    NTSTATUS status = STATUS_SUCCESS, closeStatus;
    ULONG length;
    UCHAR command[sizeof(amtptp_enable_multitouch)];
    retry.QuadPart = -20000000LL; /* Two seconds, interruptible by StopEvent. */
    while (!Stopping(context)) {
        InterlockedExchange(&context->Disconnected, 0);
        status = OpenChannel(context, 0x11, &context->ControlChannel);
        if (status != STATUS_SUCCESS || Stopping(context)) goto Disconnect;
        status = OpenChannel(context, 0x13, &context->InterruptChannel);
        if (status != STATUS_SUCCESS || Stopping(context)) goto Disconnect;
        InterlockedExchange(&context->Status.Connected, 1);
        RtlCopyMemory(command, amtptp_enable_multitouch, sizeof(command));
        length = sizeof(command);
        status = Transfer(context, context->ControlChannel, command, &length, FALSE, 2000);
        if (status != STATUS_SUCCESS || Stopping(context)) goto Disconnect;
        length = sizeof(context->ReceiveBuffer);
        status = Transfer(context, context->ControlChannel, context->ReceiveBuffer, &length, TRUE, 2000);
        if (status != STATUS_SUCCESS) goto Disconnect;
        if (!amtptp_source_handshake(context->ReceiveBuffer, length)) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            goto Disconnect;
        }
        while (!Stopping(context) && !InterlockedCompareExchange(&context->Disconnected, 0, 0)) {
            amtptp_raw_frame validated;
            SourceVhfReleaseTouches(context, TRUE);
            length = sizeof(context->ReceiveBuffer);
            status = Transfer(context, context->InterruptChannel, context->ReceiveBuffer, &length, TRUE, 500);
            if (status == STATUS_TIMEOUT || status == STATUS_IO_TIMEOUT) continue;
            if (status != STATUS_SUCCESS) break;
            if (Stopping(context)) break;
            InterlockedIncrement(&context->Status.Packets);
            if (length >= 2 && context->ReceiveBuffer[0] == AMTPTP_HIDP_INPUT &&
                amtptp_decode_mt2(context->ReceiveBuffer + 1, length - 1, &validated) == AMTPTP_OK) {
                /* A successful write/handshake alone is not mode evidence. */
                InterlockedExchange(&context->Status.ModeEnabled, 1);
                InterlockedIncrement(&context->Status.TouchPackets);
            }
            SourceVhfInput(context, context->ReceiveBuffer, length);
        }
Disconnect:
        InterlockedExchange(&context->Status.LastStatus, status);
        SourceVhfReleaseTouches(context, FALSE);
        InterlockedExchange(&context->Status.ModeEnabled, 0);
        InterlockedExchange(&context->Status.Connected, 0);
        closeStatus = CloseChannel(context, &context->InterruptChannel);
        if (closeStatus != STATUS_SUCCESS) InterlockedExchange(&context->Status.LastStatus, closeStatus);
        closeStatus = CloseChannel(context, &context->ControlChannel);
        if (closeStatus != STATUS_SUCCESS) InterlockedExchange(&context->Status.LastStatus, closeStatus);
        /* Do not open additional channels if an old close failed. */
        if (context->ControlChannel != NULL || context->InterruptChannel != NULL) break;
        if (Stopping(context)) break;
        InterlockedIncrement(&context->Status.Reconnects);
        (void)KeWaitForSingleObject(&context->StopEvent, Executive, KernelMode, FALSE, &retry);
    }
    RtlSecureZeroMemory(context->ReceiveBuffer, sizeof(context->ReceiveBuffer));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS SourceBluetoothStart(SOURCE_CONTEXT *Context)
{
    OBJECT_ATTRIBUTES attributes;
    if (!Context->InterfaceAcquired) return STATUS_DEVICE_NOT_READY;
    if (Context->Thread != NULL) return STATUS_SUCCESS;
    if (Context->ControlChannel != NULL || Context->InterruptChannel != NULL) return STATUS_DEVICE_BUSY;
    KeClearEvent(&Context->StopEvent);
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    return PsCreateSystemThread(&Context->Thread, THREAD_ALL_ACCESS, &attributes,
        NULL, NULL, SourceThread, Context);
}

VOID SourceBluetoothStop(SOURCE_CONTEXT *Context)
{
    if (Context->Thread != NULL) {
        KeSetEvent(&Context->StopEvent, IO_NO_INCREMENT, FALSE);
        (void)ZwWaitForSingleObject(Context->Thread, FALSE, NULL);
        ZwClose(Context->Thread);
        Context->Thread = NULL;
    }
}
