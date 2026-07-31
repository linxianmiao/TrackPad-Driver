// Input.c: Input handler routines

#include <Driver.h>
#include "Input.tmh"

VOID
PtpFilterInputProcessRequest(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	deviceContext = PtpFilterGetContext(Device);
	status = WdfRequestForwardToIoQueue(Request, deviceContext->HidReadQueue);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! WdfRequestForwardToIoQueue fails, status = %!STATUS!", status);
		WdfRequestComplete(Request, status);
		return;
	}

	// Only issue request when fully configured.
	// Otherwise we will let power recovery process to triage it
	if (deviceContext->DeviceConfigured == TRUE) {
		PtpFilterInputIssueTransportRequest(Device);
	}
}

VOID
PtpFilterWorkItemCallback(
	_In_ WDFWORKITEM WorkItem
)
{
	WDFDEVICE Device = WdfWorkItemGetParentObject(WorkItem);
	PtpFilterInputIssueTransportRequest(Device);
}

VOID
PtpFilterInputIssueTransportRequest(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDFREQUEST hidReadRequest;
	WDFMEMORY hidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT requestContext;
	BOOLEAN requestStatus = FALSE;

	deviceContext = PtpFilterGetContext(Device);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WORKER_REQUEST_CONTEXT);
	attributes.ParentObject = Device;
	status = WdfRequestCreate(&attributes, deviceContext->HidIoTarget, &hidReadRequest);
	if (!NT_SUCCESS(status)) {
		// This can fail for Bluetooth devices. We will set up a 3 second timer for retry triage.
		// Typically this should not fail for USB transport.
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestCreate fails, status = %!STATUS!", status);
		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		return;
	}

	status = WdfMemoryCreateFromLookaside(deviceContext->HidReadBufferLookaside, &hidReadOutputMemory);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!", status);
		WdfObjectDelete(hidReadRequest);
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Assign context information
	// And format HID read request.
	requestContext = WorkerRequestGetContext(hidReadRequest);
	requestContext->DeviceContext = deviceContext;
	requestContext->RequestMemory = hidReadOutputMemory;
	status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget, hidReadRequest,
		IOCTL_HID_READ_REPORT, NULL, 0, hidReadOutputMemory, 0);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!", status);

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}

		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Set callback
	WdfRequestSetCompletionRoutine(hidReadRequest, PtpFilterInputRequestCompletionCallback, requestContext);

	requestStatus = WdfRequestSend(hidReadRequest, deviceContext->HidIoTarget, NULL);
	if (!requestStatus) {
		// Retry after 3 seconds, in case this is a transportation issue.
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! PtpFilterInputIssueTransportRequest request failed to sent");
		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}
	}
}

static
VOID
PtpFilterInputParseMT2Report(
	_In_ PUCHAR Buffer,
	_In_ SIZE_T BufferLength,
	_In_ PDEVICE_CONTEXT DeviceContext
)
{
	NTSTATUS status;
	WDFREQUEST ptpRequest;
	WDFMEMORY  ptpRequestMemory;
	PDRIVER_CONTEXT driverContext;
	WDFDRIVER driver;
	amtptp_raw_frame rawFrame;
	amtptp_frame ptpFrame;
	amtptp_options options;
	amtptp_status coreStatus;
	UCHAR ptpBytes[AMTPTP_PTP_REPORT_SIZE];
	SIZE_T ptpLength = 0;
	
	driver = WdfGetDriver();
	if (driver == NULL)
	{
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! driver == NULL");
		return;
	}
	driverContext = PtpFilterDriverGetContext(driver);

	coreStatus = amtptp_decode_mt2(Buffer, BufferLength, &rawFrame);
	if (coreStatus != AMTPTP_OK) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_INPUT,
			"%!FUNC! Malformed input received. Length = %llu, core status = %d. Attempt to reconfigure the device.",
			BufferLength,
			(INT)coreStatus);
		WdfTimerStart(DeviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		return;
	}

	amtptp_default_options(&options);
	options.x_min = (int16_t)DeviceContext->X.min;
	options.y_min = (int16_t)DeviceContext->Y.min;
	options.x_max = (uint16_t)(DeviceContext->X.max - DeviceContext->X.min);
	options.y_max = (uint16_t)(DeviceContext->Y.max - DeviceContext->Y.min);
	options.stop_pressure = driverContext->StopPressure;
	options.stop_size = driverContext->StopSize;
	options.button_disabled = driverContext->ButtonDisabled ? 1u : 0u;
	options.ignore_button_finger =
		driverContext->IgnoreButtonFinger ? 1u : 0u;
	options.ignore_near_fingers =
		driverContext->IgnoreNearFingers ? 1u : 0u;
	options.palm_rejection = driverContext->PalmRejection ? 1u : 0u;

	coreStatus = amtptp_convert_ptp(
		&DeviceContext->CoreSession,
		&options,
		&rawFrame,
		&ptpFrame);
	if (coreStatus != AMTPTP_OK) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_INPUT,
			"%!FUNC! Core conversion failed with status %d",
			(INT)coreStatus);
		return;
	}

	if (!DeviceContext->PtpReportTouch) {
		RtlZeroMemory(ptpFrame.contacts, sizeof(ptpFrame.contacts));
		ptpFrame.contact_count = 0u;
	}
	if (!DeviceContext->PtpReportButton) {
		ptpFrame.button = 0u;
	}

	TraceEvents(
		TRACE_LEVEL_VERBOSE,
		TRACE_INPUT,
		"%!FUNC! Converted contacts raw=%u ptp=%u scan=%u button=%u admitted=0x%04x suppressed=0x%04x",
		(UINT32)rawFrame.contact_count,
		(UINT32)ptpFrame.contact_count,
		(UINT32)ptpFrame.scan_time,
		(UINT32)ptpFrame.button,
		(UINT32)DeviceContext->CoreSession.admitted_mask,
		(UINT32)DeviceContext->CoreSession.suppressed_mask);

	coreStatus = amtptp_serialize_ptp(
		&ptpFrame,
		ptpBytes,
		sizeof(ptpBytes),
		&ptpLength);
	if (coreStatus != AMTPTP_OK || ptpLength != AMTPTP_PTP_REPORT_SIZE) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_INPUT,
			"%!FUNC! Core serialization failed with status %d",
			(INT)coreStatus);
		return;
	}

	// Retrieve a pending HID read only after the source report is validated.
	status = WdfIoQueueRetrieveNextRequest(
		DeviceContext->HidReadQueue,
		&ptpRequest);
	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_INPUT,
			"%!FUNC! WdfIoQueueRetrieveNextRequest failed with %!STATUS!",
			status);
		return;
	}

	status = WdfRequestRetrieveOutputMemory(ptpRequest, &ptpRequestMemory);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!", status);
		WdfRequestComplete(ptpRequest, status);
		return;
	}

	status = WdfMemoryCopyFromBuffer(
		ptpRequestMemory,
		0,
		ptpBytes,
		ptpLength);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!", status);
		WdfRequestComplete(ptpRequest, status);
		WdfDeviceSetFailed(DeviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	WdfRequestSetInformation(ptpRequest, ptpLength);
	WdfRequestComplete(ptpRequest, STATUS_SUCCESS);
}

static
VOID
PtpFilterInputParseReport(
	_In_ PUCHAR Buffer,
	_In_ SIZE_T BufferLength,
	_In_ PDEVICE_CONTEXT DeviceContext
)
{
	UCHAR reportId = Buffer[0];

	switch (reportId) {
	case 0x2: // Mouse report
		// USB devices have mouse reports prepended, so skip over it to get to next input report
		if (BufferLength > sizeof(MOUSE_REPORT))
		{
			Buffer += sizeof(MOUSE_REPORT);
			BufferLength -= sizeof(MOUSE_REPORT);
			PtpFilterInputParseReport(Buffer, BufferLength, DeviceContext);
		}
		else
		{
			TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
				"%!FUNC! Mouse Packet - Setting Wellspring mode");
			WdfTimerStart(DeviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		}
		break;

	case 0x31: // MT2 report
		PtpFilterInputParseMT2Report(Buffer, BufferLength, DeviceContext);
		break;

	case 0xF7: // Two packets in one (0xF7, pkt 1 len, <pkt 1>, <pkt 2>)
	case 0xFC: // Part one of large packet
	case 0xFE: // Part two of large packet
	case 0x90: // Battery status
	default:
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
			"%!FUNC! Unhandled packet (Report ID: 0x%x)", reportId);
		WdfWorkItemEnqueue(DeviceContext->HidTransportRecoveryWorkItem);
		break;
	}
}

VOID
PtpFilterInputRequestCompletionCallback(
	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_ WDFCONTEXT Context
)
{
	PWORKER_REQUEST_CONTEXT requestContext;
	PDEVICE_CONTEXT deviceContext;

	size_t responseLength;
	PUCHAR responseBuffer;

	UNREFERENCED_PARAMETER(Target);
	
	requestContext = (PWORKER_REQUEST_CONTEXT)Context;
	deviceContext = requestContext->DeviceContext;
	responseLength = (size_t)(LONG)WdfRequestGetInformation(Request);
	responseBuffer = WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);

	// Pre-flight check 0: Right now we only have Magic Trackpad 2 (BT and USB)
	if (deviceContext->VendorID != HID_VID_APPLE_USB && deviceContext->VendorID != HID_VID_APPLE_BT) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! Unsupported device entered this routine");
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedNoRestart);
		goto cleanup;
	}

	// Pre-flight check 1: if size is 0, this is not something we need. Ignore the read, and issue next request.
	if (responseLength <= 0) {
		WdfWorkItemEnqueue(requestContext->DeviceContext->HidTransportRecoveryWorkItem);
		goto cleanup;
	}

	PtpFilterInputParseReport(responseBuffer, responseLength, deviceContext);

cleanup:
	// Cleanup
	WdfObjectDelete(Request);
	if (requestContext->RequestMemory != NULL) {
		WdfObjectDelete(requestContext->RequestMemory);
	}

	// We don't issue new request here (unless it's a spurious request - which is handled earlier) to
	// keep the request pipe go through one-way.
}
