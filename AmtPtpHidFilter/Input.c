// Input.c: Input handler routines

#include <Driver.h>
#include "Input.tmh"

static
VOID
PtpFilterInputIssueTransportRequestForGeneration(
	_In_ WDFDEVICE Device,
	_In_ ULONG Generation
);

static
VOID
PtpFilterInputQueueTransportWork(
	_In_ PDEVICE_CONTEXT DeviceContext
)
{
	BOOLEAN enqueueWorkItem = FALSE;

	WdfSpinLockAcquire(DeviceContext->TransportStateLock);
	if (DeviceContext->InD0 &&
		DeviceContext->DeviceConfigured &&
		!DeviceContext->LowerReadInFlight) {
		DeviceContext->TransportWorkPending = TRUE;
		DeviceContext->TransportWorkGeneration =
			DeviceContext->TransportGeneration;

		if (!DeviceContext->TransportWorkItemRunning) {
			DeviceContext->TransportWorkItemRunning = TRUE;
			enqueueWorkItem = TRUE;
		}
	}

	// Enqueue under the state lock so D0Exit cannot flush between publishing
	// the pending flag and putting the reusable work item on the system queue.
	if (enqueueWorkItem) {
		WdfWorkItemEnqueue(DeviceContext->HidTransportRecoveryWorkItem);
	}
	WdfSpinLockRelease(DeviceContext->TransportStateLock);
}

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


	// All lower reads are created and sent by the single per-device work item.
	PtpFilterInputIssueTransportRequest(Device);
}

VOID
PtpFilterWorkItemCallback(
	_In_ WDFWORKITEM WorkItem
)
{
	WDFDEVICE Device = WdfWorkItemGetParentObject(WorkItem);
	PDEVICE_CONTEXT deviceContext = PtpFilterGetContext(Device);

	for (;;) {
		ULONG generation;
		BOOLEAN issueRequest;

		WdfSpinLockAcquire(deviceContext->TransportStateLock);
		if (!deviceContext->TransportWorkPending) {
			deviceContext->TransportWorkItemRunning = FALSE;
			WdfSpinLockRelease(deviceContext->TransportStateLock);
			break;
		}

		generation = deviceContext->TransportWorkGeneration;
		deviceContext->TransportWorkPending = FALSE;
		issueRequest = deviceContext->InD0 &&
			deviceContext->DeviceConfigured &&
			deviceContext->TransportGeneration == generation &&
			!deviceContext->LowerReadInFlight;
		WdfSpinLockRelease(deviceContext->TransportStateLock);

		if (issueRequest) {
			PtpFilterInputIssueTransportRequestForGeneration(
				Device,
				generation);
		}
	}
}

VOID
PtpFilterInputIssueTransportRequest(
	_In_ WDFDEVICE Device
)
{
	PDEVICE_CONTEXT deviceContext;

	deviceContext = PtpFilterGetContext(Device);
	PtpFilterInputQueueTransportWork(deviceContext);
}

static
VOID
PtpFilterInputIssueTransportRequestForGeneration(
	_In_ WDFDEVICE Device,
	_In_ ULONG Generation
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDFREQUEST hidReadRequest = NULL;
	WDFMEMORY hidReadOutputMemory = NULL;
	PWORKER_REQUEST_CONTEXT requestContext;
	BOOLEAN requestSent;
	BOOLEAN publishRequest = FALSE;
	ULONG queuedRequests = 0;

	deviceContext = PtpFilterGetContext(Device);
	WdfIoQueueGetState(deviceContext->HidReadQueue, &queuedRequests, NULL);
	if (queuedRequests == 0) {
		return;
	}

	// Reserve the one per-device lower-read slot before allocating anything.
	WdfSpinLockAcquire(deviceContext->TransportStateLock);
	if (deviceContext->InD0 &&
		deviceContext->DeviceConfigured &&
		deviceContext->TransportGeneration == Generation &&
		!deviceContext->LowerReadInFlight) {
		deviceContext->LowerReadInFlight = TRUE;
		deviceContext->LowerReadGeneration = Generation;
		deviceContext->LowerReadRequest = NULL;
		publishRequest = TRUE;
	}
	WdfSpinLockRelease(deviceContext->TransportStateLock);

	if (!publishRequest) {
		return;
	}

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WORKER_REQUEST_CONTEXT);
	attributes.ParentObject = Device;
	status = WdfRequestCreate(&attributes, deviceContext->HidIoTarget, &hidReadRequest);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestCreate fails, status = %!STATUS!", status);
		goto failure;
	}

	status = WdfMemoryCreateFromLookaside(deviceContext->HidReadBufferLookaside, &hidReadOutputMemory);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!", status);
		goto failure;
	}

	// Assign context information
	// And format HID read request.
	requestContext = WorkerRequestGetContext(hidReadRequest);
	requestContext->DeviceContext = deviceContext;
	requestContext->RequestMemory = hidReadOutputMemory;
	requestContext->Generation = Generation;
	status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget, hidReadRequest,
		IOCTL_HID_READ_REPORT, NULL, 0, hidReadOutputMemory, 0);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!", status);
		goto failure;
	}

	WdfRequestSetCompletionRoutine(hidReadRequest, PtpFilterInputRequestCompletionCallback, requestContext);

	// Publish only after formatting and after one final generation check.
	WdfSpinLockAcquire(deviceContext->TransportStateLock);
	if (deviceContext->InD0 &&
		deviceContext->DeviceConfigured &&
		deviceContext->TransportGeneration == Generation &&
		deviceContext->LowerReadInFlight &&
		deviceContext->LowerReadGeneration == Generation &&
		deviceContext->LowerReadRequest == NULL) {
		deviceContext->LowerReadRequest = hidReadRequest;
		publishRequest = TRUE;
	}
	else {
		deviceContext->LowerReadInFlight = FALSE;
		deviceContext->LowerReadRequest = NULL;
		publishRequest = FALSE;
	}
	WdfSpinLockRelease(deviceContext->TransportStateLock);

	if (!publishRequest) {
		status = STATUS_CANCELLED;
		goto failure;
	}

	// A lower driver may complete inline.  The local reference keeps Request
	// valid until WdfRequestSend returns; after a successful send we only drop
	// this reference and never inspect the request again.
	WdfObjectReference(hidReadRequest);
	requestSent = WdfRequestSend(hidReadRequest, deviceContext->HidIoTarget, NULL);
	if (requestSent) {
		WdfObjectDereference(hidReadRequest);
		return;
	}

	status = WdfRequestGetStatus(hidReadRequest);
	TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
		"%!FUNC! WdfRequestSend failed, status = %!STATUS!", status);
	WdfObjectDereference(hidReadRequest);

failure:
	WdfSpinLockAcquire(deviceContext->TransportStateLock);
	if (deviceContext->LowerReadInFlight &&
		deviceContext->LowerReadGeneration == Generation &&
		(deviceContext->LowerReadRequest == NULL ||
		 deviceContext->LowerReadRequest == hidReadRequest)) {
		deviceContext->LowerReadInFlight = FALSE;
		deviceContext->LowerReadRequest = NULL;
	}
	WdfSpinLockRelease(deviceContext->TransportStateLock);

	if (hidReadOutputMemory != NULL) {
		WdfObjectDelete(hidReadOutputMemory);
	}
	if (hidReadRequest != NULL) {
		WdfObjectDelete(hidReadRequest);
	}

	PtpFilterScheduleTransportRecovery(deviceContext, Generation, 3);
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
	BOOLEAN reportTouch;
	BOOLEAN reportButton;
	amtptp_u16 admittedMask;
	amtptp_u16 suppressedMask;
	
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
			"%!FUNC! Dropping malformed input. Length = %llu, core status = %d.",
			BufferLength,
			(INT)coreStatus);
		return;
	}

	amtptp_default_options(&options);
	options.x_min = (amtptp_i16)DeviceContext->X.min;
	options.y_min = (amtptp_i16)DeviceContext->Y.min;
	options.x_max = (amtptp_u16)(DeviceContext->X.max - DeviceContext->X.min);
	options.y_max = (amtptp_u16)(DeviceContext->Y.max - DeviceContext->Y.min);
	options.stop_pressure = driverContext->StopPressure;
	options.stop_size = driverContext->StopSize;
	options.button_disabled = driverContext->ButtonDisabled ? 1u : 0u;
	options.ignore_button_finger =
		driverContext->IgnoreButtonFinger ? 1u : 0u;
	options.ignore_near_fingers =
		driverContext->IgnoreNearFingers ? 1u : 0u;
	options.palm_rejection = driverContext->PalmRejection ? 1u : 0u;

	// Feature callbacks can reset the session or selective-reporting state.
	// Serialize that small state transition with conversion, then release the
	// spin lock before touching WDF queues or emitting traces.
	WdfSpinLockAcquire(DeviceContext->TransportStateLock);
	coreStatus = amtptp_convert_ptp(
		&DeviceContext->CoreSession,
		&options,
		&rawFrame,
		&ptpFrame);
	reportTouch = DeviceContext->PtpReportTouch;
	reportButton = DeviceContext->PtpReportButton;
	admittedMask = DeviceContext->CoreSession.admitted_mask;
	suppressedMask = DeviceContext->CoreSession.suppressed_mask;
	WdfSpinLockRelease(DeviceContext->TransportStateLock);
	if (coreStatus != AMTPTP_OK) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_INPUT,
			"%!FUNC! Core conversion failed with status %d",
			(INT)coreStatus);
		return;
	}

	if (!reportTouch) {
		RtlZeroMemory(ptpFrame.contacts, sizeof(ptpFrame.contacts));
		ptpFrame.contact_count = 0u;
	}
	if (!reportButton) {
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
		(UINT32)admittedMask,
		(UINT32)suppressedMask);

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
		// Close the producer gate before requesting restart.  The lower-read
		// completion rechecks this state before it can enqueue another request.
		WdfSpinLockAcquire(DeviceContext->TransportStateLock);
		DeviceContext->DeviceConfigured = FALSE;
		WdfSpinLockRelease(DeviceContext->TransportStateLock);
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
	UCHAR reportId;

	if (Buffer == NULL || BufferLength == 0) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
			"%!FUNC! Dropping empty input packet");
		return;
	}

	reportId = Buffer[0];

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
				"%!FUNC! Dropping standalone mouse packet");
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
			"%!FUNC! Dropping unhandled packet (Report ID: 0x%x)", reportId);
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
	WDFMEMORY requestMemory;
	ULONG generation;
	NTSTATUS ioStatus;
	SIZE_T responseLength;
	SIZE_T bufferCapacity = 0;
	PUCHAR responseBuffer = NULL;
	BOOLEAN trackedRequest = FALSE;
	BOOLEAN currentGeneration = FALSE;
	BOOLEAN scheduleRecovery = FALSE;
	BOOLEAN issueNextRequest = FALSE;
	BOOLEAN failDevice = FALSE;

	UNREFERENCED_PARAMETER(Target);

	// Capture every request-context value before deleting either object.
	requestContext = (PWORKER_REQUEST_CONTEXT)Context;
	deviceContext = requestContext->DeviceContext;
	requestMemory = requestContext->RequestMemory;
	generation = requestContext->Generation;
	ioStatus = Params->IoStatus.Status;
	responseLength = (SIZE_T)Params->IoStatus.Information;

	WdfSpinLockAcquire(deviceContext->TransportStateLock);
	if (deviceContext->LowerReadInFlight &&
		deviceContext->LowerReadGeneration == generation &&
		deviceContext->LowerReadRequest == Request) {
		trackedRequest = TRUE;
		currentGeneration = deviceContext->InD0 &&
			deviceContext->DeviceConfigured &&
			deviceContext->TransportGeneration == generation;
	}
	WdfSpinLockRelease(deviceContext->TransportStateLock);

	if (!trackedRequest) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
			"%!FUNC! Completion did not match the published lower read, generation = %lu",
			generation);
		goto cleanup;
	}

	// A canceled/stale completion is expected during D0Exit.  Generation state
	// is checked before touching its buffer or scheduling any follow-up work.
	if (!currentGeneration) {
		goto cleanup;
	}

	if (!NT_SUCCESS(ioStatus)) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
			"%!FUNC! Lower HID read failed, status = %!STATUS!",
			ioStatus);
		scheduleRecovery = TRUE;
		goto cleanup;
	}

	if (requestMemory == NULL || responseLength == 0) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
			"%!FUNC! Lower HID read returned no data");
		scheduleRecovery = TRUE;
		goto cleanup;
	}

	responseBuffer = WdfMemoryGetBuffer(requestMemory, &bufferCapacity);
	if (responseBuffer == NULL || responseLength > bufferCapacity) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT,
			"%!FUNC! Invalid lower HID read length = %llu, capacity = %llu",
			responseLength,
			bufferCapacity);
		scheduleRecovery = TRUE;
		goto cleanup;
	}

	// Pre-flight check 0: Right now we only have Magic Trackpad 2 (BT and USB)
	if (deviceContext->VendorID != HID_VID_APPLE_USB && deviceContext->VendorID != HID_VID_APPLE_BT) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT, "%!FUNC! Unsupported device entered this routine");
		failDevice = TRUE;
		goto cleanup;
	}

	PtpFilterInputParseReport(responseBuffer, responseLength, deviceContext);
	WdfSpinLockAcquire(deviceContext->TransportStateLock);
	issueNextRequest = deviceContext->InD0 &&
		deviceContext->DeviceConfigured &&
		deviceContext->TransportGeneration == generation;
	WdfSpinLockRelease(deviceContext->TransportStateLock);

cleanup:
	// Close the producer gate before releasing the single-flight slot on error.
	// Otherwise a newly queued upper READ can publish another lower request in
	// the window before recovery disables DeviceConfigured.
	if (trackedRequest && scheduleRecovery) {
		PtpFilterScheduleTransportRecovery(deviceContext, generation, 3);
	}
	else if (trackedRequest && failDevice) {
		WdfSpinLockAcquire(deviceContext->TransportStateLock);
		if (deviceContext->TransportGeneration == generation) {
			deviceContext->DeviceConfigured = FALSE;
		}
		WdfSpinLockRelease(deviceContext->TransportStateLock);
	}

	// Keep the single-flight slot published through parsing so another upper
	// READ cannot start a second completion against CoreSession concurrently.
	if (trackedRequest) {
		WdfSpinLockAcquire(deviceContext->TransportStateLock);
		if (deviceContext->LowerReadInFlight &&
			deviceContext->LowerReadGeneration == generation &&
			deviceContext->LowerReadRequest == Request) {
			deviceContext->LowerReadInFlight = FALSE;
			deviceContext->LowerReadRequest = NULL;
		}
		WdfSpinLockRelease(deviceContext->TransportStateLock);
	}

	// The memory can be deleted once parsing is finished.  Delete Request last,
	// and never access requestContext after this point.
	if (requestMemory != NULL) {
		WdfObjectDelete(requestMemory);
	}
	WdfObjectDelete(Request);

	if (failDevice) {
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedNoRestart);
	}
	else if (!scheduleRecovery && issueNextRequest) {
		PtpFilterInputIssueTransportRequest(deviceContext->Device);
	}
}
