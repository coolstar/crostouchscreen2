#define DESCRIPTOR_DEF
#include <atmel.h>

static ULONG AtmelDebugLevel = 100;
static ULONG AtmelDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
DriverEntry(
__in PDRIVER_OBJECT  DriverObject,
__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	AtmelPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, AtmelEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
		);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

static size_t mxt_obj_size(const struct mxt_object *obj)
{
	return obj->size_minus_one + 1;
}

static size_t mxt_obj_instances(const struct mxt_object *obj)
{
	return obj->instances_minus_one + 1;
}

static
struct mxt_object *
	mxt_findobject(struct mxt_rollup *core, int type)
{
	int i;

	for (i = 0; i < core->nobjs; ++i) {
		if (core->objs[i].type == type)
			return(&core->objs[i]);
	}
	return NULL;
}

static NTSTATUS
mxt_read_reg(PATMEL_CONTEXT  devContext, uint16_t reg, void *rbuf, int bytes)
{
	uint8_t wreg[2];
	wreg[0] = reg & 255;
	wreg[1] = reg >> 8;

	uint16_t nreg = ((uint16_t *)wreg)[0];

	NTSTATUS error = SpbReadDataSynchronously16(&devContext->I2CContext, nreg, rbuf, bytes);

	return error;
}

static NTSTATUS
mxt_write_reg_buf(PATMEL_CONTEXT  devContext, uint16_t reg, void *xbuf, int bytes)
{
	uint8_t wreg[2];
	wreg[0] = reg & 255;
	wreg[1] = reg >> 8;

	uint16_t nreg = ((uint16_t *)wreg)[0];
	return SpbWriteDataSynchronously16(&devContext->I2CContext, nreg, xbuf, bytes);
}

static NTSTATUS
mxt_write_reg(PATMEL_CONTEXT  devContext, uint16_t reg, uint8_t val)
{
	return mxt_write_reg_buf(devContext, reg, &val, 1);
}

static NTSTATUS
mxt_write_object_off(PATMEL_CONTEXT  devContext, struct mxt_object *obj,
	int offset, uint8_t val)
{
	uint16_t reg = obj->start_address;

	reg += offset;
	return mxt_write_reg(devContext, reg, val);
}

static
void
atmel_reset_device(PATMEL_CONTEXT  devContext)
{
	mxt_write_object_off(devContext, devContext->cmdprocobj, MXT_CMDPROC_RESET_OFF, 1);
}

static NTSTATUS mxt_read_t9_resolution(PATMEL_CONTEXT devContext)
{
	struct t9_range range;
	unsigned char orient;

	mxt_rollup core = devContext->core;
	mxt_object *resolutionobject = mxt_findobject(&core, MXT_TOUCH_MULTI_T9);

	mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_RANGE, &range, sizeof(range));

	mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_ORIENT, &orient, 1);

	/* Handle default values */
	if (range.x == 0)
		range.x = 1023;

	if (range.y == 0)
		range.y = 1023;

	if (orient & MXT_T9_ORIENT_SWITCH) {
		devContext->max_x = range.y+1;
		devContext->max_y = range.x+1;
	}
	else {
		devContext->max_x = range.x+1;
		devContext->max_y = range.y+1;
	}
	AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Screen Size: X: %d Y: %d\n", devContext->max_x, devContext->max_y);
	return STATUS_SUCCESS;
}

NTSTATUS BOOTTOUCHSCREEN(
	_In_  PATMEL_CONTEXT  devContext
	)
{
	int blksize;
	int totsize;
	uint32_t crc;
	mxt_rollup core = devContext->core;

	AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Initializing Touch Screen.\n");

	mxt_read_reg(devContext, 0, &core.info, sizeof(core.info));

	core.nobjs = core.info.num_objects;

	if (core.nobjs < 0 || core.nobjs > 1024) {
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "init_device nobjs (%d) out of bounds\n",
			core.nobjs);
	}

	blksize = sizeof(core.info) +
		core.nobjs * sizeof(struct mxt_object);
	totsize = blksize + sizeof(struct mxt_raw_crc);

	core.buf = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, totsize, ATMEL_POOL_TAG);

	mxt_read_reg(devContext, 0, core.buf, totsize);

	crc = obp_convert_crc((mxt_raw_crc *)((uint8_t *)core.buf + blksize));

	if (obp_crc24(core.buf, blksize) != crc) {
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"init_device: configuration space "
			"crc mismatch %08x/%08x\n",
			crc, obp_crc24(core.buf, blksize));
	}
	else {
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "CRC Matched!\n");
	}

	core.objs = (mxt_object *)((uint8_t *)core.buf +
		sizeof(core.info));

	devContext->msgprocobj = mxt_findobject(&core, MXT_GEN_MESSAGEPROCESSOR);
	devContext->cmdprocobj = mxt_findobject(&core, MXT_GEN_COMMANDPROCESSOR);

	devContext->core = core;

	int reportid = 1;
	for (int i = 0; i < core.nobjs; i++) {
		mxt_object *obj = &core.objs[i];
		uint8_t min_id, max_id;

		if (obj->num_report_ids) {
			min_id = reportid;
			reportid += obj->num_report_ids *
				mxt_obj_instances(obj);
			max_id = reportid - 1;
		}
		else {
			min_id = 0;
			max_id = 0;
		}

		switch (obj->type) {
		case MXT_GEN_MESSAGE_T5:
			if (devContext->info.family == 0x80 &&
				devContext->info.version < 0x20) {
				/*
				* On mXT224 firmware versions prior to V2.0
				* read and discard unused CRC byte otherwise
				* DMA reads are misaligned.
				*/
				devContext->T5_msg_size = mxt_obj_size(obj);
			}
			else {
				/* CRC not enabled, so skip last byte */
				devContext->T5_msg_size = mxt_obj_size(obj) - 1;
			}
			devContext->T5_address = obj->start_address;
			break;
		case MXT_GEN_COMMAND_T6:
			devContext->T6_reportid = min_id;
			devContext->T6_address = obj->start_address;
			break;
		case MXT_GEN_POWER_T7:
			devContext->T7_address = obj->start_address;
			break;
		case MXT_TOUCH_MULTI_T9:
			devContext->multitouch = MXT_TOUCH_MULTI_T9;
			devContext->T9_reportid_min = min_id;
			devContext->T9_reportid_max = max_id;
			devContext->num_touchids = obj->num_report_ids
				* mxt_obj_instances(obj);
			break;
		case MXT_SPT_MESSAGECOUNT_T44:
			devContext->T44_address = obj->start_address;
			break;
		case MXT_SPT_GPIOPWM_T19:
			devContext->T19_reportid = min_id;
			break;
		case MXT_TOUCH_MULTITOUCHSCREEN_T100:
			devContext->multitouch = MXT_TOUCH_MULTITOUCHSCREEN_T100;
			devContext->T100_reportid_min = min_id;
			devContext->T100_reportid_max = max_id;
			/* first two report IDs reserved */
			devContext->num_touchids = obj->num_report_ids - 2;
			break;
		}
		AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Obj Type: %d\n", obj->type);
	}

	if (devContext->multitouch == MXT_TOUCH_MULTI_T9)
		mxt_read_t9_resolution(devContext);

	if (devContext->multitouch == MXT_TOUCH_MULTI_T9 || devContext->multitouch == MXT_TOUCH_MULTITOUCHSCREEN_T100) {
		uint16_t max_x[] = { devContext->max_x };
		uint16_t max_y[] = { devContext->max_y };

		uint8_t *max_x8bit = (uint8_t *)max_x;
		uint8_t *max_y8bit = (uint8_t *)max_y;

		devContext->max_x_hid[0] = max_x8bit[0];
		devContext->max_x_hid[1] = max_x8bit[1];

		devContext->max_y_hid[0] = max_y8bit[0];
		devContext->max_y_hid[1] = max_y8bit[1];
	}

	atmel_reset_device(devContext);

	return STATUS_SUCCESS;
}

NTSTATUS
OnPrepareHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesRaw,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PATMEL_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	BOOTTOUCHSCREEN(pDevice);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PATMEL_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	ExFreePoolWithTag(pDevice->core.buf, ATMEL_POOL_TAG);

	pDevice->core.buf = NULL;

	pDevice->msgprocobj = NULL;
	pDevice->cmdprocobj = NULL;

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PATMEL_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	WdfTimerStart(pDevice->Timer, WDF_REL_TIMEOUT_IN_MS(10));

	for (int i = 0; i < 20; i++){
		pDevice->Flags[i] = 0;
	}

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

	return status;
}

NTSTATUS
OnD0Exit(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PATMEL_CONTEXT pDevice = GetDeviceContext(FxDevice);

	WdfTimerStop(pDevice->Timer, TRUE);

	pDevice->ConnectInterrupt = false;

	return STATUS_SUCCESS;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID){
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return true;

	mxt_message_t msg;
	SpbReadDataSynchronously(&pDevice->I2CContext, 0, &msg, sizeof(msg));

	int reportid = msg.any.reportid;

	if (reportid != 0xff){
		int rawx = (msg.touch.pos[0] << 4) |
			((msg.touch.pos[2] >> 4) & 0x0F);
		int rawy = ((msg.touch.pos[1] << 4) |
			((msg.touch.pos[2]) & 0x0F)) / 4;

		pDevice->Flags[reportid] = msg.touch.flags;
		pDevice->XValue[reportid] = rawx;
		pDevice->YValue[reportid] = rawy;
		pDevice->AREA[reportid] = msg.touch.area;
	}

	struct _ATMEL_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20){
		if (pDevice->Flags[i] != 0){
			report.Touch[count].ContactID = i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT){
				report.Touch[count].Status = MULTI_IN_RANGE_BIT | MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS){
				report.Touch[count].Status = MULTI_IN_RANGE_BIT | MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE){
				report.Touch[count].Status = 0;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = MULTI_IN_RANGE_BIT;

			count++;
		}
		i++;
	}

	report.ActualCount = count;

	if (count > 0){
		size_t bytesWritten;
		AtmelProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}

	pDevice->lastmsg = msg;
	pDevice->RegsSet = true;

	return true;
}


void CyapaTimerFunc(_In_ WDFTIMER hTimer){
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return;

	if (!pDevice->RegsSet)
		return;

	struct _ATMEL_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20){
		if (pDevice->Flags[i] != 0){
			report.Touch[count].ContactID = i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT){
				report.Touch[count].Status = MULTI_IN_RANGE_BIT | MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS){
				report.Touch[count].Status = MULTI_IN_RANGE_BIT | MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE){
				report.Touch[count].Status = 0;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = MULTI_IN_RANGE_BIT;

			count++;
		}
		i++;
	}

	report.ActualCount = count;

	if (count > 0){
		size_t bytesWritten;
		AtmelProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}

	return;
}

NTSTATUS
AtmelEvtDeviceAdd(
IN WDFDRIVER       Driver,
IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PATMEL_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"AtmelEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Because we are a virtual device the root enumerator would just put null values 
	// in response to IRP_MN_QUERY_ID. Lets override that.
	//

	minorFunction = IRP_MN_QUERY_ID;

	status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
		DeviceInit,
		AtmelEvtWdmPreprocessMnQueryId,
		IRP_MJ_PNP,
		&minorFunction,
		1
		);
	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceInitAssignWdmIrpPreprocessCallback failed Status 0x%x\n", status);

		return status;
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ATMEL_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = AtmelEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
		);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	WDF_TIMER_CONFIG              timerConfig;
	WDFTIMER                      hTimer;

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, CyapaTimerFunc, 10);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = device;
	status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);
	devContext->Timer = hTimer;
	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "(%!FUNC!) WdfTimerCreate failed status:%!STATUS!\n", status);
		return status;
	}

	//
	// Initialize DeviceMode
	//

	devContext->DeviceMode = DEVICE_MODE_MOUSE;

	return status;
}

NTSTATUS
AtmelEvtWdmPreprocessMnQueryId(
WDFDEVICE Device,
PIRP Irp
)
{
	NTSTATUS            status;
	PIO_STACK_LOCATION  IrpStack, previousSp;
	PDEVICE_OBJECT      DeviceObject;
	PWCHAR              buffer;

	PAGED_CODE();

	//
	// Get a pointer to the current location in the Irp
	//

	IrpStack = IoGetCurrentIrpStackLocation(Irp);

	//
	// Get the device object
	//
	DeviceObject = WdfDeviceWdmGetDeviceObject(Device);


	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_PNP,
		"AtmelEvtWdmPreprocessMnQueryId Entry\n");

	//
	// This check is required to filter out QUERY_IDs forwarded
	// by the HIDCLASS for the parent FDO. These IDs are sent
	// by PNP manager for the parent FDO if you root-enumerate this driver.
	//
	previousSp = ((PIO_STACK_LOCATION)((UCHAR *)(IrpStack)+
		sizeof(IO_STACK_LOCATION)));

	if (previousSp->DeviceObject == DeviceObject)
	{
		//
		// Filtering out this basically prevents the Found New Hardware
		// popup for the root-enumerated Atmel on reboot.
		//
		status = Irp->IoStatus.Status;
	}
	else
	{
		switch (IrpStack->Parameters.QueryId.IdType)
		{
		case BusQueryDeviceID:
		case BusQueryHardwareIDs:
			//
			// HIDClass is asking for child deviceid & hardwareids.
			// Let us just make up some id for our child device.
			//
			buffer = (PWCHAR)ExAllocatePoolWithTag(
				NonPagedPool,
				ATMEL_HARDWARE_IDS_LENGTH,
				ATMEL_POOL_TAG
				);

			if (buffer)
			{
				//
				// Do the copy, store the buffer in the Irp
				//
				RtlCopyMemory(buffer,
					ATMEL_HARDWARE_IDS,
					ATMEL_HARDWARE_IDS_LENGTH
					);

				Irp->IoStatus.Information = (ULONG_PTR)buffer;
				status = STATUS_SUCCESS;
			}
			else
			{
				//
				//  No memory
				//
				status = STATUS_INSUFFICIENT_RESOURCES;
			}

			Irp->IoStatus.Status = status;
			//
			// We don't need to forward this to our bus. This query
			// is for our child so we should complete it right here.
			// fallthru.
			//
			IoCompleteRequest(Irp, IO_NO_INCREMENT);

			break;

		default:
			status = Irp->IoStatus.Status;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			break;
		}
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelEvtWdmPreprocessMnQueryId Exit = 0x%x\n", status);

	return status;
}

VOID
AtmelEvtInternalDeviceControl(
IN WDFQUEUE     Queue,
IN WDFREQUEST   Request,
IN size_t       OutputBufferLength,
IN size_t       InputBufferLength,
IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PATMEL_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
		);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = AtmelGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = AtmelGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = AtmelGetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = AtmelGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = AtmelWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = AtmelReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = AtmelSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = AtmelGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}
	else
	{
		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}

	return;
}

NTSTATUS
AtmelGetHidDescriptor(
IN WDFDEVICE Device,
IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelGetReportDescriptor(
IN WDFDEVICE Device,
IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetReportDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)DefaultReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
AtmelGetDeviceAttributes(
IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		(PVOID *)&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = ATMEL_VID;
	deviceAttributes->ProductID = ATMEL_PID;
	deviceAttributes->VersionNumber = ATMEL_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelGetString(
IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"Atmel.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID)*sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"AtmelGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"AtmelGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelWriteReport(
IN PATMEL_CONTEXT DevContext,
IN WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	size_t bytesWritten = 0;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"AtmelWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"AtmelWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"AtmelWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
AtmelProcessVendorReport(
IN PATMEL_CONTEXT DevContext,
IN PVOID ReportBuffer,
IN ULONG ReportBufferLen,
OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"AtmelProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelReadReport(
IN PATMEL_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelSetFeature(
IN PATMEL_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	AtmelFeatureReport* pReport = NULL;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"AtmelSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"AtmelWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_FEATURE:

				if (transferPacket->reportBufferLen == sizeof(AtmelFeatureReport))
				{
					pReport = (AtmelFeatureReport*)transferPacket->reportBuffer;

					DevContext->DeviceMode = pReport->DeviceMode;

					AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"AtmelSetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"AtmelSetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(AtmelFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(AtmelFeatureReport));
				}

				break;

			default:

				AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"AtmelSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
AtmelGetFeature(
IN PATMEL_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"AtmelGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"AtmelGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_MTOUCH:
			{

				AtmelMaxCountReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(AtmelMaxCountReport))
				{
					pReport = (AtmelMaxCountReport*)transferPacket->reportBuffer;

					pReport->MaximumCount = MULTI_MAX_COUNT;

					AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"AtmelGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"AtmelGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(AtmelMaxCountReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(AtmelMaxCountReport));
				}

				break;
			}

			case REPORTID_FEATURE:
			{

				AtmelFeatureReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(AtmelFeatureReport))
				{
					pReport = (AtmelFeatureReport*)transferPacket->reportBuffer;

					pReport->DeviceMode = DevContext->DeviceMode;

					pReport->DeviceIdentifier = 0;

					AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"AtmelGetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"AtmelGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(AtmelFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(AtmelFeatureReport));
				}

				break;
			}

			default:

				AtmelPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"AtmelGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}