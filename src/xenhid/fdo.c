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

#define INITGUID
#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <hidport.h>
#include <version.h>

#include <hid_interface.h>
#include <store_interface.h>
#include <suspend_interface.h>

#include "fdo.h"
#include "thread.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "names.h"
#include "string.h"

#define MAXNAMELEN  128

struct _XENHID_FDO {
    PDEVICE_OBJECT              DeviceObject;
    PDEVICE_OBJECT              LowerDeviceObject;
    PXENHID_THREAD              DevicePowerThread;
    PIRP                        DevicePowerIrp;
    DEVICE_POWER_STATE          DevicePowerState;
    BOOLEAN                     Enabled;
    XENHID_HID_INTERFACE        HidInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallback;
    IO_CSQ                      Queue;
    KSPIN_LOCK                  Lock;
    LIST_ENTRY                  List;
};

#define FDO_POOL_TAG 'ODF'

ULONG
FdoGetSize(
    VOID
    )
{
    return sizeof(XENHID_FDO);
}

IO_CSQ_INSERT_IRP FdoCsqInsertIrp;

VOID
FdoCsqInsertIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, Queue);

    InsertTailList(&Fdo->List, &Irp->Tail.Overlay.ListEntry);
}

IO_CSQ_REMOVE_IRP FdoCsqRemoveIrp;

VOID
FdoCsqRemoveIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    UNREFERENCED_PARAMETER(Csq);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

IO_CSQ_PEEK_NEXT_IRP FdoCsqPeekNextIrp;

PIRP
FdoCsqPeekNextIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp,
    IN  PVOID   Context
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, Queue);
    PLIST_ENTRY ListEntry;
    PIRP        NextIrp;

    UNREFERENCED_PARAMETER(Context);

    if (Irp == NULL)
        ListEntry = Fdo->List.Flink;
    else
        ListEntry = Irp->Tail.Overlay.ListEntry.Flink;

    // should walk through the list until a match against Context is found
    if (ListEntry != &Fdo->List)
        NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
    else
        NextIrp = NULL;

    return NextIrp;
}

#pragma warning(push)
#pragma warning(disable:28167) // function changes IRQL

IO_CSQ_ACQUIRE_LOCK FdoCsqAcquireLock;

VOID
FdoCsqAcquireLock(
    IN  PIO_CSQ Csq,
    OUT PKIRQL  Irql
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, Queue);

    KeAcquireSpinLock(&Fdo->Lock, Irql);
}

IO_CSQ_RELEASE_LOCK FdoCsqReleaseLock;

VOID
FdoCsqReleaseLock(
    IN  PIO_CSQ Csq,
    IN  KIRQL   Irql
    )
{
    PXENHID_FDO Fdo = CONTAINING_RECORD(Csq, XENHID_FDO, Queue);

    KeReleaseSpinLock(&Fdo->Lock, Irql);
}

#pragma warning(pop)

IO_CSQ_COMPLETE_CANCELED_IRP FdoCsqCompleteCanceledIrp;

VOID
FdoCsqCompleteCanceledIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    UNREFERENCED_PARAMETER(Csq);

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static DECLSPEC_NOINLINE BOOLEAN
FdoHidCallback(
    IN  PVOID       Argument,
    IN  PVOID       Buffer,
    IN  ULONG       Length
    )
{
    PXENHID_FDO     Fdo = Argument;
    BOOLEAN         Completed = FALSE;
    PIRP            Irp;

    Irp = IoCsqRemoveNextIrp(&Fdo->Queue, NULL);
    if (Irp == NULL)
        goto done;

    RtlCopyMemory(Irp->UserBuffer,
                  Buffer,
                  Length);
    Irp->IoStatus.Information = Length;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    Completed = TRUE;

done:
    return Completed;
}


static FORCEINLINE PVOID
__FdoAllocate(
    IN  ULONG   Length
    )
{
    PVOID       Buffer;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, FDO_POOL_TAG);
    if (Buffer)
        RtlZeroMemory(Buffer, Length);

    return Buffer;
}

static FORCEINLINE VOID
__FdoFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, FDO_POOL_TAG);
}

static FORCEINLINE VOID
__FdoSetDevicePowerState(
    IN  PXENHID_FDO         Fdo,
    IN  DEVICE_POWER_STATE  State
)
{
    Fdo->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__FdoGetDevicePowerState(
    IN  PXENHID_FDO     Fdo
)
{
    return Fdo->DevicePowerState;
}

static FORCEINLINE PANSI_STRING
__FdoMultiSzToUpcaseAnsi(
    IN  PCHAR       Buffer
)
{
    PANSI_STRING    Ansi;
    LONG            Index;
    LONG            Count;
    NTSTATUS        status;

    Index = 0;
    Count = 0;
    for (;;) {
        if (Buffer[Index] == '\0') {
            Count++;
            Index++;

            // Check for double NUL
            if (Buffer[Index] == '\0')
                break;
        }
        else {
            Buffer[Index] = __toupper(Buffer[Index]);
            Index++;
        }
    }

    Ansi = __FdoAllocate(sizeof(ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    for (Index = 0; Index < Count; Index++) {
        ULONG   Length;

        Length = (ULONG)strlen(Buffer);
        Ansi[Index].MaximumLength = (USHORT)(Length + 1);
        Ansi[Index].Buffer = __FdoAllocate(Ansi[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi[Index].Buffer == NULL)
            goto fail2;

        RtlCopyMemory(Ansi[Index].Buffer, Buffer, Length);
        Ansi[Index].Length = (USHORT)Length;

        Buffer += Length + 1;
    }

    return Ansi;

fail2:
    Error("fail2\n");

    while (--Index >= 0)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

static FORCEINLINE VOID
__FdoFreeAnsi(
    IN  PANSI_STRING    Ansi
    )
{
    ULONG               Index;

    for (Index = 0; Ansi[Index].Buffer != NULL; Index++)
        __FdoFree(Ansi[Index].Buffer);

    __FdoFree(Ansi);
}

static FORCEINLINE BOOLEAN
__FdoMatchDistribution(
    IN  PXENHID_FDO     Fdo,
    IN  PCHAR           Buffer
)
{
    PCHAR               Vendor;
    PCHAR               Product;
    PCHAR               Context;
    const CHAR          *Text;
    BOOLEAN             Match;
    ULONG               Index;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(Fdo);

    status = STATUS_INVALID_PARAMETER;

    Vendor = __strtok_r(Buffer, " ", &Context);
    if (Vendor == NULL)
        goto fail1;

    Product = __strtok_r(NULL, " ", &Context);
    if (Product == NULL)
        goto fail2;

    Match = TRUE;

    Text = VENDOR_NAME_STR;

    for (Index = 0; Text[Index] != 0; Index++) {
        if (!isalnum((UCHAR)Text[Index])) {
            if (Vendor[Index] != '_') {
                Match = FALSE;
                break;
            }
        } else {
            if (Vendor[Index] != Text[Index]) {
                Match = FALSE;
                break;
            }
        }
    }

    Text = "XENHID";

    if (_stricmp(Product, Text) != 0)
        Match = FALSE;

    return Match;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return FALSE;
}

#define MAXIMUM_INDEX   255

static FORCEINLINE NTSTATUS
__FdoSetDistribution(
    IN  PXENHID_FDO     Fdo
    )
{
    ULONG               Index;
    CHAR                Distribution[MAXNAMELEN];
    CHAR                Vendor[MAXNAMELEN];
    STRING              String;
    const CHAR          *Product;
    NTSTATUS            status;

    Trace("====>\n");

    Index = 0;
    while (Index <= MAXIMUM_INDEX) {
        PCHAR   Buffer;

        String.Buffer = Distribution;
        String.MaximumLength = sizeof(Distribution);
        String.Length = 0;

        status = StringPrintf(&String,
                              "%u",
                              Index);
        ASSERT(NT_SUCCESS(status));

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "drivers",
                              Distribution,
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_OBJECT_NAME_NOT_FOUND)
                goto update;

            goto fail1;
        }

        XENBUS_STORE(Free,
            &Fdo->StoreInterface,
            Buffer);

        Index++;
    }

    status = STATUS_UNSUCCESSFUL;
    goto fail2;

update:
    String.Buffer = Vendor;
    String.MaximumLength = sizeof(Vendor);
    String.Length = 0;

    status = StringPrintf(&String,
                          "%s",
                          VENDOR_NAME_STR);
    ASSERT(NT_SUCCESS(status));

    for (Index = 0; Vendor[Index] != '\0'; Index++)
        if (!isalnum((UCHAR)Vendor[Index]))
            Vendor[Index] = '_';

    Product = "XENHID";

#if DBG
#define ATTRIBUTES   "(DEBUG)"
#else
#define ATTRIBUTES   ""
#endif

    (VOID)XENBUS_STORE(Printf,
                       &Fdo->StoreInterface,
                       NULL,
                       "drivers",
                       Distribution,
                       "%s %s %u.%u.%u.%u %s",
                       Vendor,
                       Product,
                       MAJOR_VERSION,
                       MINOR_VERSION,
                       MICRO_VERSION,
                       BUILD_NUMBER,
                       ATTRIBUTES
    );

#undef  ATTRIBUTES

    Trace("<====\n");
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__FdoClearDistribution(
    IN  PXENHID_FDO     Fdo
    )
{
    PCHAR               Buffer;
    PANSI_STRING        Distributions;
    ULONG               Index;
    NTSTATUS            status;

    Trace("====>\n");

    status = XENBUS_STORE(Directory,
                          &Fdo->StoreInterface,
                          NULL,
                          NULL,
                          "drivers",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Distributions = __FdoMultiSzToUpcaseAnsi(Buffer);

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);
    } else {
        Distributions = NULL;
    }

    if (Distributions == NULL)
        goto done;

    for (Index = 0; Distributions[Index].Buffer != NULL; Index++) {
        PANSI_STRING    Distribution = &Distributions[Index];

        status = XENBUS_STORE(Read,
                              &Fdo->StoreInterface,
                              NULL,
                              "drivers",
                              Distribution->Buffer,
                              &Buffer);
        if (!NT_SUCCESS(status))
            continue;

        if (__FdoMatchDistribution(Fdo, Buffer))
            (VOID)XENBUS_STORE(Remove,
                               &Fdo->StoreInterface,
                               NULL,
                               "drivers",
                               Distribution->Buffer);

        XENBUS_STORE(Free,
                     &Fdo->StoreInterface,
                     Buffer);
    }

    __FdoFreeAnsi(Distributions);

done:
    Trace("<====\n");
}

static DECLSPEC_NOINLINE VOID
FdoSuspendCallback(
    IN  PVOID       Argument
    )
{
    PXENHID_FDO     Fdo = Argument;

    (VOID)__FdoSetDistribution(Fdo);
}

static DECLSPEC_NOINLINE NTSTATUS
FdoSetDistribution(
    IN  PXENHID_FDO Fdo
    )
{
    NTSTATUS            status;

    Trace("====>\n");

    (VOID)__FdoSetDistribution(Fdo);

    status = XENBUS_SUSPEND(Register,
                            &Fdo->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FdoSuspendCallback,
                            Fdo,
                            &Fdo->SuspendCallback);
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    __FdoClearDistribution(Fdo);

    return status;
}

static DECLSPEC_NOINLINE VOID
FdoClearDistribution(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("====>\n");

    XENBUS_SUSPEND(Deregister,
                   &Fdo->SuspendInterface,
                   Fdo->SuspendCallback);
    Fdo->SuspendCallback = NULL;

    __FdoClearDistribution(Fdo);

    Trace("<====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
FdoD3ToD0(
    IN  PXENHID_FDO Fdo
    )
{
    NTSTATUS        status;

    ASSERT3U(__FdoGetDevicePowerState(Fdo), ==, PowerDeviceD3);

    Trace("=====>\n");

    if (Fdo->Enabled)
        goto done;

    status = XENBUS_STORE(Acquire,
                          &Fdo->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_SUSPEND(Acquire,
                            &Fdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = FdoSetDistribution(Fdo);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENHID_HID(Acquire,
                        &Fdo->HidInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENHID_HID(Enable,
                        &Fdo->HidInterface,
                        FdoHidCallback,
                        Fdo);
    if (!NT_SUCCESS(status))
        goto fail5;

    Fdo->Enabled = TRUE;
done:
    __FdoSetDevicePowerState(Fdo, PowerDeviceD0);
    Trace("<=====\n");
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    XENHID_HID(Release,
               &Fdo->HidInterface);

fail4:
    Error("fail4\n");

    FdoClearDistribution(Fdo);

fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Release,
                   &Fdo->SuspendInterface);

fail2:
    Error("fail2\n");

    XENBUS_STORE(Release,
                 &Fdo->StoreInterface);

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static DECLSPEC_NOINLINE VOID
FdoD0ToD3(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");

    __FdoSetDevicePowerState(Fdo, PowerDeviceD3);

    if (!Fdo->Enabled)
        goto done;

    XENHID_HID(Disable,
               &Fdo->HidInterface);

    XENHID_HID(Release,
               &Fdo->HidInterface);

    FdoClearDistribution(Fdo);

    XENBUS_SUSPEND(Release,
                   &Fdo->SuspendInterface);

    XENBUS_STORE(Release,
                 &Fdo->StoreInterface);

    Fdo->Enabled = FALSE;
done:
    Trace("<=====\n");
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchDefault(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
__FdoForwardIrpSynchronously(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
FdoForwardIrpSynchronously(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    KEVENT              Event;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,
                           __FdoForwardIrpSynchronously,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    Trace("%08x\n", status);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoStartDevice(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = FdoD3ToD0(Fdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoStopDevice(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    FdoD0ToD3(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoRemoveDevice(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    FdoD0ToD3(Fdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    FdoDestroy(Fdo);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPnp(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = FdoStartDevice(Fdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = FdoRemoveDevice(Fdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = FdoStopDevice(Fdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerUp(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
)
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    Trace("====>\n");

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, <, __FdoGetDevicePowerState(Fdo));

    status = FdoForwardIrpSynchronously(Fdo, Irp);
    if (!NT_SUCCESS(status))
        goto done;

    Info("%s -> %s\n",
        PowerDeviceStateName(__FdoGetDevicePowerState(Fdo)),
        PowerDeviceStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD0);
    status = FdoD3ToD0(Fdo);
    ASSERT(NT_SUCCESS(status));

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%08x)\n", status);
    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePowerDown(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
)
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;

    ASSERT3U(DeviceState, >, __FdoGetDevicePowerState(Fdo));

    Info("%s -> %s\n",
        PowerDeviceStateName(__FdoGetDevicePowerState(Fdo)),
        PowerDeviceStateName(DeviceState));

    ASSERT3U(DeviceState, ==, PowerDeviceD3);

    if (__FdoGetDevicePowerState(Fdo) == PowerDeviceD0)
        FdoD0ToD3(Fdo);

    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

    return status;
}

static FORCEINLINE NTSTATUS
__FdoSetDevicePower(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
)
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
        PowerDeviceStateName(DeviceState),
        PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (DeviceState == __FdoGetDevicePowerState(Fdo)) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    status = (DeviceState < __FdoGetDevicePowerState(Fdo)) ?
        __FdoSetDevicePowerUp(Fdo, Irp) :
        __FdoSetDevicePowerDown(Fdo, Irp);

done:
    Trace("<==== (%s:%s)(%08x)\n",
        PowerDeviceStateName(DeviceState),
        PowerActionName(PowerAction),
        status);
    return status;
}

static NTSTATUS
FdoDevicePower(
    IN  PXENHID_THREAD  Self,
    IN  PVOID           Context
)
{
    PXENHID_FDO         Fdo = (PXENHID_FDO)Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP                Irp;
        PIO_STACK_LOCATION  StackLocation;
        UCHAR               MinorFunction;

        if (Fdo->DevicePowerIrp == NULL) {
            (VOID)KeWaitForSingleObject(Event,
                Executive,
                KernelMode,
                FALSE,
                NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Fdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Fdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        MinorFunction = StackLocation->MinorFunction;

        switch (StackLocation->MinorFunction) {
        case IRP_MN_SET_POWER:
            (VOID)__FdoSetDevicePower(Fdo, Irp);
            break;

        default:
            ASSERT(FALSE);
            break;
        }
    }

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchPower(
    IN  PXENHID_FDO     Fdo,
    IN  PIRP            Irp
)
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    if (MinorFunction != IRP_MN_SET_POWER) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);

        goto done;
    }

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Fdo->DevicePowerIrp, ==, NULL);
        Fdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Fdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
        break;
    }

done:
    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
FdoDispatchInternal(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Type3Input;
    ULONG               IoControlCode;
    ULONG               InputLength;
    ULONG               OutputLength;
    PVOID               Buffer;
    ULONG               Returned;
    PHID_XFER_PACKET    Packet;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    IoControlCode = StackLocation->Parameters.DeviceIoControl.IoControlCode;
    Type3Input = (ULONG)(ULONG_PTR)StackLocation->Parameters.DeviceIoControl.Type3InputBuffer;
    InputLength = StackLocation->Parameters.DeviceIoControl.InputBufferLength;
    OutputLength = StackLocation->Parameters.DeviceIoControl.OutputBufferLength;
    Buffer = Irp->UserBuffer;
    Packet = Irp->UserBuffer;

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = XENHID_HID(GetDeviceAttributes,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = XENHID_HID(GetDeviceDescriptor,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = XENHID_HID(GetReportDescriptor,
                            &Fdo->HidInterface,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_STRING:
        status = XENHID_HID(GetString,
                            &Fdo->HidInterface,
                            Type3Input,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_GET_INDEXED_STRING:
        status = XENHID_HID(GetIndexedString,
                            &Fdo->HidInterface,
                            Type3Input,
                            Buffer,
                            OutputLength,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;

        break;

    case IOCTL_HID_GET_FEATURE:
        status = XENHID_HID(GetFeature,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_SET_FEATURE:
        status = XENHID_HID(SetFeature,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    case IOCTL_HID_GET_INPUT_REPORT:
        status = XENHID_HID(GetInputReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen,
                            &Returned);
        if (NT_SUCCESS(status))
            Irp->IoStatus.Information = (ULONG_PTR)Returned;
        break;

    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = XENHID_HID(SetOutputReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    case IOCTL_HID_READ_REPORT:
        status = STATUS_PENDING;
        IoCsqInsertIrp(&Fdo->Queue, Irp, NULL);
        XENHID_HID(ReadReport,
                   &Fdo->HidInterface);
        break;

    case IOCTL_HID_WRITE_REPORT:
        status = XENHID_HID(WriteReport,
                            &Fdo->HidInterface,
                            Packet->reportId,
                            Packet->reportBuffer,
                            Packet->reportBufferLen);
        break;

    // Other HID IOCTLs are failed as not supported
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (status != STATUS_PENDING) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return status;
}

NTSTATUS
FdoDispatch(
    IN  PXENHID_FDO Fdo,
    IN  PIRP        Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    switch (StackLocation->MajorFunction) {
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        status = FdoDispatchInternal(Fdo, Irp);
        break;

    case IRP_MJ_PNP:
        status = FdoDispatchPnp(Fdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = FdoDispatchPower(Fdo, Irp);
        break;

    default:
        status = FdoDispatchDefault(Fdo, Irp);
        break;
    }

    return status;
}

static FORCEINLINE NTSTATUS
FdoQueryInterface(
    IN  PXENHID_FDO     Fdo,
    IN  const GUID      *Guid,
    IN  ULONG           Version,
    OUT PINTERFACE      Interface,
    IN  ULONG           Size
    )
{
    KEVENT              Event;
    IO_STATUS_BLOCK     StatusBlock;
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&StatusBlock, sizeof(IO_STATUS_BLOCK));

#pragma prefast(suppress:28123)
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       Fdo->LowerDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &StatusBlock);

    status = STATUS_UNSUCCESSFUL;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);
    StackLocation->MinorFunction = IRP_MN_QUERY_INTERFACE;

    StackLocation->Parameters.QueryInterface.InterfaceType = Guid;
    StackLocation->Parameters.QueryInterface.Size = (USHORT)Size;
    StackLocation->Parameters.QueryInterface.Version = (USHORT)Version;
    StackLocation->Parameters.QueryInterface.Interface = Interface;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(Fdo->LowerDeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = StatusBlock.Status;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);
    return status;
}

NTSTATUS
FdoCreate(
    IN  PXENHID_FDO     Fdo,
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PDEVICE_OBJECT  LowerDeviceObject
    )
{
    NTSTATUS            status;

    Trace("=====>\n");

    Fdo->DeviceObject = DeviceObject;
    Fdo->LowerDeviceObject = LowerDeviceObject;
    Fdo->DevicePowerState = PowerDeviceD3;

    status = ThreadCreate(FdoDevicePower, Fdo, &Fdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail1;

    InitializeListHead(&Fdo->List);
    KeInitializeSpinLock(&Fdo->Lock);

    status = IoCsqInitialize(&Fdo->Queue,
                             FdoCsqInsertIrp,
                             FdoCsqRemoveIrp,
                             FdoCsqPeekNextIrp,
                             FdoCsqAcquireLock,
                             FdoCsqReleaseLock,
                             FdoCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = FdoQueryInterface(Fdo,
                               &GUID_XENBUS_SUSPEND_INTERFACE,
                               XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->SuspendInterface,
                               sizeof(XENBUS_SUSPEND_INTERFACE));
    if (!NT_SUCCESS(status))
        goto fail3;

    status = FdoQueryInterface(Fdo,
                               &GUID_XENBUS_STORE_INTERFACE,
                               XENBUS_STORE_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->StoreInterface,
                               sizeof(XENBUS_STORE_INTERFACE));
    if (!NT_SUCCESS(status))
        goto fail4;

    status = FdoQueryInterface(Fdo,
                               &GUID_XENHID_HID_INTERFACE,
                               XENHID_HID_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&Fdo->HidInterface,
                               sizeof(XENHID_HID_INTERFACE));
    if (!NT_SUCCESS(status))
        goto fail5;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    RtlZeroMemory(&Fdo->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

fail4:
    Error("fail4\n");

    RtlZeroMemory(&Fdo->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));

fail3:
    Error("fail3\n");

    RtlZeroMemory(&Fdo->Queue, sizeof(IO_CSQ));

fail2:
    Error("fail2 %08x\n", status);

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;

    RtlZeroMemory(&Fdo->List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Fdo->Lock, sizeof(KSPIN_LOCK));

fail1:
    Error("fail1 %08x\n", status);

    Fdo->DeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof(XENHID_FDO)));
    return status;
}

VOID
FdoDestroy(
    IN  PXENHID_FDO Fdo
    )
{
    Trace("=====>\n");

    RtlZeroMemory(&Fdo->HidInterface,
                  sizeof(XENHID_HID_INTERFACE));
    RtlZeroMemory(&Fdo->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));
    RtlZeroMemory(&Fdo->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    ThreadAlert(Fdo->DevicePowerThread);
    ThreadJoin(Fdo->DevicePowerThread);
    Fdo->DevicePowerThread = NULL;
    Fdo->DevicePowerIrp = NULL;
    Fdo->DevicePowerState = 0;

    RtlZeroMemory(&Fdo->Queue, sizeof(IO_CSQ));
    RtlZeroMemory(&Fdo->List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Fdo->Lock, sizeof(KSPIN_LOCK));

    Fdo->DeviceObject = NULL;
    Fdo->LowerDeviceObject = NULL;

    ASSERT(IsZeroMemory(Fdo, sizeof(XENHID_FDO)));
    Trace("<=====\n");
}
