#define DESCRIPTOR_DEF
#include "atmel.h"

static ULONG AtmelDebugLevel = 100;
static ULONG AtmelDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;


int AtmelProcessMessagesUntilInvalid(PATMEL_CONTEXT pDevice);

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
NTSTATUS
atmel_reset_device(PATMEL_CONTEXT  devContext)
{
	return mxt_write_object_off(devContext, devContext->cmdprocobj, MXT_CMDPROC_RESET_OFF, 1);
}

static NTSTATUS mxt_read_t9_resolution(PATMEL_CONTEXT devContext)
{
	struct t9_range range;
	UINT8 xsize, ysize;
	unsigned char orient;
	NTSTATUS status;

	mxt_rollup core = devContext->core;
	mxt_object *resolutionobject = mxt_findobject(&core, MXT_TOUCH_MULTI_T9);

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_XSIZE, &xsize, sizeof(xsize));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_YSIZE, &ysize, sizeof(ysize));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_RANGE, &range, sizeof(range));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T9_ORIENT, &orient, 1);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	/* Handle default values */
	if (range.x == 0)
		range.x = 1023;

	if (range.y == 0)
		range.y = 1023;

	if (orient & MXT_T9_ORIENT_SWITCH) {
		devContext->max_x = range.y + 1;
		devContext->max_y = range.x + 1;
	}
	else {
		devContext->max_x = range.x + 1;
		devContext->max_y = range.y + 1;
	}
	AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Screen Size: X: %d Y: %d\n", devContext->max_x, devContext->max_y);
	return status;
}

static NTSTATUS mxt_read_t100_config(PATMEL_CONTEXT devContext)
{
	NTSTATUS status;
	uint16_t range_x, range_y;
	uint8_t cfg, tchaux;
	uint8_t aux;

	mxt_rollup core = devContext->core;
	mxt_object *resolutionobject = mxt_findobject(&core, MXT_TOUCH_MULTITOUCHSCREEN_T100);

	/* read touchscreen dimensions */
	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T100_XRANGE, &range_x, sizeof(range_x));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T100_YRANGE, &range_y, sizeof(range_y));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	/* read orientation config */
	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T100_CFG1, &cfg, 1);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (cfg & MXT_T100_CFG_SWITCHXY) {
		devContext->max_x = range_y + 1;
		devContext->max_y = range_x + 1;
	}
	else {
		devContext->max_x = range_x + 1;
		devContext->max_y = range_y + 1;
	}

	status = mxt_read_reg(devContext, resolutionobject->start_address + MXT_T100_TCHAUX, &tchaux, 1);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	aux = 6;

	if (tchaux & MXT_T100_TCHAUX_VECT)
		devContext->t100_aux_vect = aux++;

	if (tchaux & MXT_T100_TCHAUX_AMPL)
		devContext->t100_aux_ampl = aux++;

	if (tchaux & MXT_T100_TCHAUX_AREA)
		devContext->t100_aux_area = aux++;
	AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Screen Size T100: X: %d Y: %d\n", devContext->max_x, devContext->max_y);

	return status;
}

static NTSTATUS mxt_set_t7_power_cfg(PATMEL_CONTEXT  devContext, uint8_t sleep)
{
	struct t7_config *new_config;
	struct t7_config deepsleep;
	deepsleep.active = deepsleep.idle = 0;
	struct t7_config active;
	active.active = 20;
	active.idle = 100;

	if (sleep == MXT_POWER_CFG_DEEPSLEEP)
		new_config = &deepsleep;
	else {
		new_config = &active;
	}

	return mxt_write_reg_buf(devContext, devContext->T7_address,
		new_config, sizeof(devContext->t7_cfg));
}

VOID
AtmelBootWorkItem(
	IN WDFWORKITEM  WorkItem
)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	uint8_t test[8];
	mxt_read_reg(pDevice, pDevice->T44_address, test, 0x07);

	WdfObjectDelete(WorkItem);
}

void AtmelBootTimer(_In_ WDFTIMER hTimer) {
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, ATMEL_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, AtmelBootWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);
	WdfTimerStop(hTimer, FALSE);
}

NTSTATUS BOOTTOUCHSCREEN(
	_In_  PATMEL_CONTEXT  devContext
)
{
	NTSTATUS status = STATUS_SUCCESS;
	if (!devContext->TouchScreenBooted) {
		int blksize;
		int totsize;
		uint32_t crc;
		mxt_rollup *core = &devContext->core;

		AtmelPrint(DEBUG_LEVEL_INFO, DBG_PNP, "Initializing Touch Screen.\n");

		status = mxt_read_reg(devContext, 0, &core->info, sizeof(core->info));
		if (!NT_SUCCESS(status)) {
			return status;
		}

		core->nobjs = core->info.num_objects;

		if (core->nobjs < 0 || core->nobjs > 1024) {
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "init_device nobjs (%d) out of bounds\n",
				core->nobjs);
		}

		blksize = sizeof(core->info) +
			core->nobjs * sizeof(struct mxt_object);
		totsize = blksize + sizeof(struct mxt_raw_crc);

		core->buf = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, totsize, ATMEL_POOL_TAG);

		status = mxt_read_reg(devContext, 0, core->buf, totsize);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		crc = obp_convert_crc((mxt_raw_crc *)((uint8_t *)core->buf + blksize));

		if (obp_crc24(core->buf, blksize) != crc) {
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
				"init_device: configuration space "
				"crc mismatch %08x/%08x\n",
				crc, obp_crc24(core->buf, blksize));
		}
		else {
			AtmelPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "CRC Matched!\n");
		}

		core->objs = (mxt_object *)((uint8_t *)core->buf +
			sizeof(core->info));

		devContext->msgprocobj = mxt_findobject(core, MXT_GEN_MESSAGEPROCESSOR);
		devContext->cmdprocobj = mxt_findobject(core, MXT_GEN_COMMANDPROCESSOR);

		int reportid = 1;
		for (int i = 0; i < core->nobjs; i++) {
			mxt_object *obj = &core->objs[i];
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
		}

		devContext->max_reportid = reportid;

		AtmelProcessMessagesUntilInvalid(devContext);

		if (devContext->multitouch == MXT_TOUCH_MULTI_T9)
			mxt_read_t9_resolution(devContext);
		else if (devContext->multitouch == MXT_TOUCH_MULTITOUCHSCREEN_T100)
			mxt_read_t100_config(devContext);

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

		status = atmel_reset_device(devContext);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		WDF_TIMER_CONFIG              timerConfig;
		WDFTIMER                      hTimer;
		WDF_OBJECT_ATTRIBUTES         attributes;

		WDF_TIMER_CONFIG_INIT(&timerConfig, AtmelBootTimer);

		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = devContext->FxDevice;
		NTSTATUS status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);

		WdfTimerStart(hTimer, WDF_REL_TIMEOUT_IN_MS(200));

		devContext->TouchScreenBooted = true;

		return status;
	}
	else {
		if (devContext->multitouch == MXT_TOUCH_MULTITOUCHSCREEN_T100) {
			status = mxt_set_t7_power_cfg(devContext, MXT_POWER_CFG_RUN);
			if (!NT_SUCCESS(status)) {
				return status;
			}
		}
		else {
			struct mxt_object *obj = mxt_findobject(&devContext->core, MXT_TOUCH_MULTI_T9);
			status = mxt_write_object_off(devContext, obj, MXT_T9_CTRL, 0x83);
			if (!NT_SUCCESS(status)) {
				return status;
			}
		}

		status = atmel_reset_device(devContext);
		return status;
	}
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

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = BOOTTOUCHSCREEN(pDevice);

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

	if (pDevice->core.buf != NULL) {
		ExFreePoolWithTag(pDevice->core.buf, ATMEL_POOL_TAG);
	}

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

	atmel_reset_device(pDevice);

	for (int i = 0; i < 20; i++) {
		pDevice->Flags[i] = 0;
	}

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

	AtmelCompleteIdleIrp(pDevice);

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

	if (pDevice->multitouch == MXT_TOUCH_MULTITOUCHSCREEN_T100)
		mxt_set_t7_power_cfg(pDevice, MXT_POWER_CFG_DEEPSLEEP);
	else {
		struct mxt_object *obj = mxt_findobject(&pDevice->core, MXT_TOUCH_MULTI_T9);
		mxt_write_object_off(pDevice, obj, MXT_T9_CTRL, 0);
	}

	WdfTimerStop(pDevice->Timer, TRUE);

	pDevice->ConnectInterrupt = false;

	return STATUS_SUCCESS;
}

int AtmelProcessMessage(PATMEL_CONTEXT pDevice, uint8_t *message) {
	uint8_t report_id = message[0];

	if (report_id == 0xff)
		return 0;

	if (report_id == pDevice->T6_reportid) {
		uint8_t status = message[1];
		uint32_t crc = message[2] | (message[3] << 8) | (message[4] << 16);
	}
	else if (report_id >= pDevice->T9_reportid_min && report_id <= pDevice->T9_reportid_max) {
		uint8_t flags = message[1];

		int rawx = (message[2] << 4) | ((message[4] >> 4) & 0xf);
		int rawy = (message[3] << 4) | ((message[4] & 0xf));

		/* Handle 10/12 bit switching */
		if (pDevice->max_x < 1024)
			rawx >>= 2;
		if (pDevice->max_y < 1024)
			rawy >>= 2;

		uint8_t area = message[5];
		uint8_t ampl = message[6];

		pDevice->Flags[report_id] = flags;
		pDevice->XValue[report_id] = rawx;
		pDevice->YValue[report_id] = rawy;
		pDevice->AREA[report_id] = area;
	}
	else if (report_id >= pDevice->T100_reportid_min && report_id <= pDevice->T100_reportid_max) {
		int reportid = report_id - pDevice->T100_reportid_min - 2;

		uint8_t flags = message[1];

		uint8_t t9_flags = 0; //convert T100 flags to T9
		if (flags & MXT_T100_DETECT)
			t9_flags += MXT_T9_DETECT;
		else if (pDevice->Flags[reportid] & MXT_T100_DETECT)
			t9_flags += MXT_T9_RELEASE;

		int rawx = *((uint16_t *)&message[2]);
		int rawy = *((uint16_t *)&message[4]);

		if (reportid >= 0) {
			pDevice->Flags[reportid] = t9_flags;

			pDevice->XValue[reportid] = rawx;
			pDevice->YValue[reportid] = rawy;
			pDevice->AREA[reportid] = 10;
		}
	}

	struct _ATMEL_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ActualCount = count;

	if (count > 0) {
		size_t bytesWritten;
		AtmelProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}
	pDevice->RegsSet = true;
	return 1;
}

int AtmelReadAndProcessMessages(PATMEL_CONTEXT pDevice, uint8_t count) {
	uint8_t num_valid = 0;
	int i, ret;
	if (count > pDevice->max_reportid)
		return -1;

	uint8_t *msg_buf = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, pDevice->max_reportid * pDevice->T5_msg_size, ATMEL_POOL_TAG);

	for (int i = 0; i < pDevice->max_reportid * pDevice->T5_msg_size; i++) {
		msg_buf[i] = 0xff;
	}

	NTSTATUS status = mxt_read_reg(pDevice, pDevice->T5_address, msg_buf, pDevice->T5_msg_size * count);
	if (!NT_SUCCESS(status)) {
		ExFreePoolWithTag(msg_buf, ATMEL_POOL_TAG);
		return 0;
	}

	for (i = 0; i < count; i++) {
		ret = AtmelProcessMessage(pDevice,
			msg_buf + pDevice->T5_msg_size * i);

		if (ret == 1)
			num_valid++;
	}

	ExFreePoolWithTag(msg_buf, ATMEL_POOL_TAG);

	/* return number of messages read */
	return num_valid;
}

int AtmelProcessMessagesUntilInvalid(PATMEL_CONTEXT pDevice) {
	int count, read;
	uint8_t tries = 2;

	count = pDevice->max_reportid;
	do {
		read = AtmelReadAndProcessMessages(pDevice, count);
		if (read < count)
			return 0;
	} while (--tries);
	return -1;
}

bool AtmelDeviceReadT44(PATMEL_CONTEXT pDevice) {
	NTSTATUS status;
	int ret;
	uint8_t count, num_left;

	uint8_t *msg_buf = (uint8_t *)ExAllocatePoolWithTag(NonPagedPool, pDevice->T5_msg_size + 1, ATMEL_POOL_TAG);

	/* Read T44 and T5 together */
	status = mxt_read_reg(pDevice, pDevice->T44_address, msg_buf, pDevice->T5_msg_size);
	if (!NT_SUCCESS(status)) {
		goto end;
	}

	count = msg_buf[0];

	if (count == 0)
		goto end;

	if (count > pDevice->max_reportid) {
		count = pDevice->max_reportid;
	}

	ret = AtmelProcessMessage(pDevice, msg_buf + 1);
	if (ret < 0) {
		goto end;
	}

	num_left = count - 1;

	if (num_left) {
		ret = AtmelReadAndProcessMessages(pDevice, num_left);
		if (ret < 0)
			goto end;
		//else if (ret != num_left)
		///	DbgPrint("T44: Unexpected invalid message!\n");
	}

end:
	ExFreePoolWithTag(msg_buf, ATMEL_POOL_TAG);
	return true;
}

bool AtmelDeviceRead(PATMEL_CONTEXT pDevice) {
	int total_handled, num_handled;
	uint8_t count = pDevice->last_message_count;

	if (count < 1 || count > pDevice->max_reportid)
		count = 1;

	/* include final invalid message */
	total_handled = AtmelReadAndProcessMessages(pDevice, count + 1);
	if (total_handled < 0)
		return false;
	else if (total_handled <= count)
		goto update_count;

	/* keep reading two msgs until one is invalid or reportid limit */
	do {
		num_handled = AtmelReadAndProcessMessages(pDevice, 2);
		if (num_handled < 0)
			return false;

		total_handled += num_handled;

		if (num_handled < 2)
			break;
	} while (total_handled < pDevice->num_touchids);

update_count:
	pDevice->last_message_count = total_handled;

	return true;
}

void AtmelProcessInput(PATMEL_CONTEXT pDevice) {
	struct _ATMEL_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ActualCount = count;

	if (count > 0) {
		size_t bytesWritten;
		AtmelProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}
}
BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return false;

	bool ret = true;

	if (pDevice->T44_address)
		ret = AtmelDeviceReadT44(pDevice);
	else
		ret = AtmelDeviceRead(pDevice);

	AtmelProcessInput(pDevice);

	return ret;
}

void AtmelTimerFunc(_In_ WDFTIMER hTimer) {
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PATMEL_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return;

	if (!pDevice->RegsSet)
		return;

	AtmelProcessInput(pDevice);
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

	devContext->TouchScreenBooted = false;

	devContext->FxDevice = device;

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
	// Create manual I/O queue to take care of idle power requests
	//

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->IdleQueue
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

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, AtmelTimerFunc, 10);

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

void
AtmelIdleIrpWorkItem
(
	IN WDFWORKITEM IdleWorkItem
)
{
	NTSTATUS status;
	PIDLE_WORKITEM_CONTEXT idleWorkItemContext;
	PATMEL_CONTEXT deviceContext;
	PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO idleCallbackInfo;

	idleWorkItemContext = GetIdleWorkItemContext(IdleWorkItem);
	NT_ASSERT(idleWorkItemContext != NULL);

	deviceContext = GetDeviceContext(idleWorkItemContext->FxDevice);
	NT_ASSERT(deviceContext != NULL);

	//
	// Get the idle callback info from the workitem context
	//
	PIRP irp = WdfRequestWdmGetIrp(idleWorkItemContext->FxRequest);
	PIO_STACK_LOCATION stackLocation = IoGetCurrentIrpStackLocation(irp);

	idleCallbackInfo = (PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)
		(stackLocation->Parameters.DeviceIoControl.Type3InputBuffer);

	//
	// idleCallbackInfo is validated already, so invoke idle callback
	//
	idleCallbackInfo->IdleCallback(idleCallbackInfo->IdleContext);

	//
	// Park this request in our IdleQueue and mark it as pending
	// This way if the IRP was cancelled, WDF will cancel it for us
	//
	status = WdfRequestForwardToIoQueue(
		idleWorkItemContext->FxRequest,
		deviceContext->IdleQueue);

	if (!NT_SUCCESS(status))
	{
		//
		// IdleQueue is a manual-dispatch, non-power-managed queue. This should
		// *never* fail.
		//

		NT_ASSERTMSG("WdfRequestForwardToIoQueue to IdleQueue failed!", FALSE);

		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Error forwarding idle notification Request:0x%p to IdleQueue:0x%p - %!STATUS!",
			idleWorkItemContext->FxRequest,
			deviceContext->IdleQueue,
			status);

		//
		// Complete the request if we couldnt forward to the Idle Queue
		//
		WdfRequestComplete(idleWorkItemContext->FxRequest, status);
	}
	else
	{
		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Forwarded idle notification Request:0x%p to IdleQueue:0x%p - %!STATUS!",
			idleWorkItemContext->FxRequest,
			deviceContext->IdleQueue,
			status);
	}

	//
	// Delete the workitem since we're done with it
	//
	WdfObjectDelete(IdleWorkItem);

	return;
}

NTSTATUS
AtmelProcessIdleRequest(
	IN PATMEL_CONTEXT pDevice,
	IN WDFREQUEST Request,
	OUT BOOLEAN* Complete
)
{
	PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO idleCallbackInfo;
	PIRP irp;
	PIO_STACK_LOCATION irpSp;
	NTSTATUS status;

	NT_ASSERT(Complete != NULL);
	*Complete = TRUE;

	//
	// Retrieve request parameters and validate
	//
	irp = WdfRequestWdmGetIrp(Request);
	irpSp = IoGetCurrentIrpStackLocation(irp);

	if (irpSp->Parameters.DeviceIoControl.InputBufferLength <
		sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO))
	{
		status = STATUS_INVALID_BUFFER_SIZE;

		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Error: Input buffer is too small to process idle request - %!STATUS!",
			status);

		goto exit;
	}

	//
	// Grab the callback
	//
	idleCallbackInfo = (PHID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)
		irpSp->Parameters.DeviceIoControl.Type3InputBuffer;

	NT_ASSERT(idleCallbackInfo != NULL);

	if (idleCallbackInfo == NULL || idleCallbackInfo->IdleCallback == NULL)
	{
		status = STATUS_NO_CALLBACK_ACTIVE;
		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Error: Idle Notification request %p has no idle callback info - %!STATUS!",
			Request,
			status);
		goto exit;
	}

	{
		//
		// Create a workitem for the idle callback
		//
		WDF_OBJECT_ATTRIBUTES workItemAttributes;
		WDF_WORKITEM_CONFIG workitemConfig;
		WDFWORKITEM idleWorkItem;
		PIDLE_WORKITEM_CONTEXT idleWorkItemContext;

		WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&workItemAttributes, IDLE_WORKITEM_CONTEXT);
		workItemAttributes.ParentObject = pDevice->FxDevice;

		WDF_WORKITEM_CONFIG_INIT(&workitemConfig, AtmelIdleIrpWorkItem);

		status = WdfWorkItemCreate(
			&workitemConfig,
			&workItemAttributes,
			&idleWorkItem
		);

		if (!NT_SUCCESS(status)) {
			AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"Error creating creating idle work item - %!STATUS!",
				status);
			goto exit;
		}

		//
		// Set the workitem context
		//
		idleWorkItemContext = GetIdleWorkItemContext(idleWorkItem);
		idleWorkItemContext->FxDevice = pDevice->FxDevice;
		idleWorkItemContext->FxRequest = Request;

		//
		// Enqueue a workitem for the idle callback
		//
		WdfWorkItemEnqueue(idleWorkItem);

		//
		// Mark the request as pending so that 
		// we can complete it when we come out of idle
		//
		*Complete = FALSE;
	}

exit:

	return status;
}

VOID
AtmelCompleteIdleIrp(
	IN PATMEL_CONTEXT FxDeviceContext
)
/*++

Routine Description:

	This is invoked when we enter D0.
	We simply complete the Idle Irp if it hasn't been cancelled already.

Arguments:

	FxDeviceContext -  Pointer to Device Context for the device

Return Value:



--*/
{
	NTSTATUS status;
	WDFREQUEST request = NULL;

	//
	// Lets try to retrieve the Idle IRP from the Idle queue
	//
	status = WdfIoQueueRetrieveNextRequest(
		FxDeviceContext->IdleQueue,
		&request);

	//
	// We did not find the Idle IRP, maybe it was cancelled
	// 
	if (!NT_SUCCESS(status) || (request == NULL))
	{
		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Error finding idle notification request in IdleQueue:0x%p - %!STATUS!",
			FxDeviceContext->IdleQueue,
			status);
	}
	else
	{
		//
		// Complete the Idle IRP
		//
		WdfRequestComplete(request, status);

		AtmelPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"Completed idle notification Request:0x%p from IdleQueue:0x%p - %!STATUS!",
			request,
			FxDeviceContext->IdleQueue,
			status);
	}

	return;
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

	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		//Handle HID Idle request
		status = AtmelProcessIdleRequest(devContext, Request, &completeRequest);
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

	PATMEL_CONTEXT devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(Device);

	AtmelPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"AtmelGetReportDescriptor Entry\n");

#define MT_TOUCH_COLLECTION												\
			MT_TOUCH_COLLECTION0 \
			0x26, devContext->max_x_hid[0], devContext->max_x_hid[1],                   /*       LOGICAL_MAXIMUM (WIDTH)    */ \
			MT_TOUCH_COLLECTION1 \
			0x26, devContext->max_y_hid[0], devContext->max_y_hid[1],                   /*       LOGICAL_MAXIMUM (HEIGHT)    */ \
			MT_TOUCH_COLLECTION2 \

	HID_REPORT_DESCRIPTOR ReportDescriptor[] = {
		//
		// Multitouch report starts here
		//
		0x05, 0x0d,                         // USAGE_PAGE (Digitizers)
		0x09, 0x04,                         // USAGE (Touch Screen)
		0xa1, 0x01,                         // COLLECTION (Application)
		0x85, REPORTID_MTOUCH,              //   REPORT_ID (Touch)
		0x09, 0x22,                         //   USAGE (Finger)
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		USAGE_PAGE
		0xc0,                               // END_COLLECTION
	};

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
		(PVOID)ReportDescriptor,
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

	lenID = pwstrID ? wcslen(pwstrID) * sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

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