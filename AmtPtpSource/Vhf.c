#include "Source.h"

EVT_VHF_ASYNC_OPERATION SourceGetFeature;
EVT_VHF_ASYNC_OPERATION SourceSetFeature;

NTSTATUS SourceVhfCreate(SOURCE_CONTEXT *Context)
{
    VHF_CONFIG config;
    NTSTATUS status;
    VHF_CONFIG_INIT(&config, WdfDeviceWdmGetDeviceObject(Context->Device),
        (USHORT)amtptp_source_descriptor_size, (PUCHAR)amtptp_source_descriptor);
    config.VhfClientContext = Context;
    /* Project-local virtual identity, distinct from the physical Apple device. */
    config.VendorID = 0x8910;
    config.ProductID = 0x0324;
    config.VersionNumber = 0x0200;
    config.EvtVhfAsyncOperationGetFeature = SourceGetFeature;
    config.EvtVhfAsyncOperationSetFeature = SourceSetFeature;
    /* Default VHF buffering copies each submitted report before return. */
    status = VhfCreate(&config, &Context->Vhf);
    if (!NT_SUCCESS(status)) return status;
    return VhfStart(Context->Vhf);
}

VOID SourceVhfDelete(SOURCE_CONTEXT *Context)
{
    if (Context->Vhf != NULL) {
        VhfDelete(Context->Vhf, TRUE);
        Context->Vhf = NULL;
    }
}

VOID SourceGetFeature(PVOID Client, VHFOPERATIONHANDLE Operation,
    PVOID OperationContext, PHID_XFER_PACKET Packet)
{
    SOURCE_CONTEXT *context = Client;
    amtptp_size written = 0;
    int result;
    KIRQL irql;
    UNREFERENCED_PARAMETER(OperationContext);
    KeAcquireSpinLock(&context->StateLock, &irql);
    result = amtptp_source_get_feature(&context->Core, Packet->reportId,
        Packet->reportBuffer, Packet->reportBufferLen, &written);
    KeReleaseSpinLock(&context->StateLock, irql);
    if (result == AMTPTP_SOURCE_REPORT) Packet->reportBufferLen = (ULONG)written;
    (void)VhfAsyncOperationComplete(Operation, result == AMTPTP_SOURCE_REPORT ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER);
}

VOID SourceSetFeature(PVOID Client, VHFOPERATIONHANDLE Operation,
    PVOID OperationContext, PHID_XFER_PACKET Packet)
{
    SOURCE_CONTEXT *context = Client;
    int result;
    KIRQL irql;
    UNREFERENCED_PARAMETER(OperationContext);
    KeAcquireSpinLock(&context->StateLock, &irql);
    result = amtptp_source_set_feature(&context->Core, Packet->reportId,
        Packet->reportBuffer, Packet->reportBufferLen);
    KeReleaseSpinLock(&context->StateLock, irql);
    /* No Bluetooth I/O in callbacks (which can arrive at DISPATCH_LEVEL). */
    (void)VhfAsyncOperationComplete(Operation, result == AMTPTP_SOURCE_REPORT ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER);
}

static VOID SubmitBatch(SOURCE_CONTEXT *Context, amtptp_report_batch *Batch)
{
    UCHAR index;
    for (index = 0; index < Batch->count; ++index) {
        HID_XFER_PACKET packet;
        NTSTATUS status;
        packet.reportId = AMTPTP_PTP_REPORT_ID;
        packet.reportBuffer = Batch->reports[index];
        packet.reportBufferLen = AMTPTP_PTP_REPORT_SIZE;
        status = VhfReadReportSubmit(Context->Vhf, &packet);
        if (!NT_SUCCESS(status)) InterlockedExchange(&Context->Status.LastStatus, status);
        else InterlockedIncrement(&Context->Status.Reports);
    }
}

VOID SourceVhfReleaseTouches(SOURCE_CONTEXT *Context, BOOLEAN OnlyIfPending)
{
    amtptp_report_batch batch = {0};
    KIRQL irql;
    KeAcquireSpinLock(&Context->StateLock, &irql);
    if (!OnlyIfPending || Context->Core.release_pending) amtptp_source_release(&Context->Core, &batch);
    KeReleaseSpinLock(&Context->StateLock, irql);
    if (Context->Vhf != NULL) SubmitBatch(Context, &batch);
}

VOID SourceVhfInput(SOURCE_CONTEXT *Context, const UCHAR *Buffer, ULONG Length)
{
    amtptp_report_batch batch;
    KIRQL irql;
    int result;
    SourceVhfReleaseTouches(Context, TRUE);
    KeAcquireSpinLock(&Context->StateLock, &irql);
    result = amtptp_source_input(&Context->Core, Buffer, Length, &batch);
    KeReleaseSpinLock(&Context->StateLock, irql);
    if (result == AMTPTP_SOURCE_INVALID) InterlockedIncrement(&Context->Status.InvalidPackets);
    else if (result == AMTPTP_SOURCE_REPORT) SubmitBatch(Context, &batch);
}
