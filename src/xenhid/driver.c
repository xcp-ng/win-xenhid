/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <hidport.h>

#include <version.h>

#include "fdo.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENHID_DRIVER {
    PDRIVER_OBJECT      DriverObject;
} XENHID_DRIVER, *PXENHID_DRIVER;

static XENHID_DRIVER    Driver;

static FORCEINLINE VOID
__DriverSetDriverObject(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    Driver.DriverObject = DriverObject;
}

static FORCEINLINE PDRIVER_OBJECT
__DriverGetDriverObject(
    VOID
    )
{
    return Driver.DriverObject;
}

PDRIVER_OBJECT
DriverGetDriverObject(
    VOID
    )
{
    return __DriverGetDriverObject();
}

DRIVER_UNLOAD       DriverUnload;

VOID
DriverUnload(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    Info("XENHID %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENHID_DRIVER)));

    Trace("<====\n");
}

DRIVER_ADD_DEVICE   AddDevice;

NTSTATUS
AddDevice(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    PHID_DEVICE_EXTENSION   Hid;
    PXENHID_FDO             Fdo;
    PDEVICE_OBJECT          LowerDeviceObject;
    NTSTATUS                status;

    ASSERT3P(__DriverGetDriverObject(), ==, DriverObject);

    Hid = DeviceObject->DeviceExtension;
    Fdo = Hid->MiniDeviceExtension;
    LowerDeviceObject = Hid->NextDeviceObject;

    status = FdoCreate(Fdo,
                       DeviceObject,
                       LowerDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return status;

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

DRIVER_DISPATCH Dispatch;

NTSTATUS 
Dispatch(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    PHID_DEVICE_EXTENSION   Hid;
    PXENHID_FDO             Fdo;
    NTSTATUS                status;

    Hid = DeviceObject->DeviceExtension;
    Fdo = Hid->MiniDeviceExtension;

    status = FdoDispatch(Fdo, Irp);

    return status;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT          DriverObject,
    IN  PUNICODE_STRING         RegistryPath
    )
{
    HID_MINIDRIVER_REGISTRATION Minidriver;
    ULONG                       Index;
    NTSTATUS                    status;

    ASSERT3P(__DriverGetDriverObject(), ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WdmlibProcgrpInitialize();

    Trace("====>\n");

    __DriverSetDriverObject(DriverObject);

    Driver.DriverObject->DriverUnload = DriverUnload;

    Info("XENHID %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    DriverObject->DriverExtension->AddDevice = AddDevice;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
#pragma prefast(suppress:28169) // No __drv_dispatchType annotation
#pragma prefast(suppress:28168) // No matching __drv_dispatchType annotation for IRP_MJ_CREATE
        DriverObject->MajorFunction[Index] = Dispatch;
    }

    RtlZeroMemory(&Minidriver, sizeof(Minidriver));
    Minidriver.Revision             = HID_REVISION;
    Minidriver.DriverObject         = DriverObject;
    Minidriver.RegistryPath         = RegistryPath;
    Minidriver.DeviceExtensionSize  = FdoGetSize();
    Minidriver.DevicesArePolled     = FALSE;

    status = HidRegisterMinidriver(&Minidriver);
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENHID_DRIVER)));

    return status;
}
