/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
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

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>

#include <xencons_device.h>

#include "driver.h"
#include "console.h"
#include "stream.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define CONSOLE_POOL 'SNOC'

typedef struct _CONSOLE_HANDLE {
    LIST_ENTRY      ListEntry;
    PFILE_OBJECT    FileObject;
    PXENCONS_STREAM Stream;
} CONSOLE_HANDLE, *PCONSOLE_HANDLE;

typedef struct _XENCONS_CONSOLE {
    LONG            References;
    PXENCONS_FDO    Fdo;
    LIST_ENTRY      List;
    KSPIN_LOCK      Lock;
} XENCONS_CONSOLE, *PXENCONS_CONSOLE;

static FORCEINLINE PVOID
__ConsoleAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, CONSOLE_POOL);
}

static FORCEINLINE VOID
__ConsoleFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, CONSOLE_POOL);
}

static FORCEINLINE NTSTATUS
__ConsoleCreateHandle(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject,
    OUT PCONSOLE_HANDLE     *Handle
    )
{
    NTSTATUS                status;

    *Handle = __ConsoleAllocate(sizeof(CONSOLE_HANDLE));

    status = STATUS_NO_MEMORY;
    if (*Handle == NULL)
        goto fail1;

    status = StreamCreate(Console->Fdo, &(*Handle)->Stream);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Handle)->FileObject = FileObject;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(Handle, sizeof(CONSOLE_HANDLE)));
    __ConsoleFree(Handle);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__ConsoleDestroyHandle(
    IN  PXENCONS_CONSOLE    Console,
    IN  PCONSOLE_HANDLE     Handle
    )
{
    UNREFERENCED_PARAMETER(Console);

    RtlZeroMemory(&Handle->ListEntry, sizeof(LIST_ENTRY));

    StreamDestroy(Handle->Stream);
    Handle->Stream = NULL;

    Handle->FileObject = NULL;

    ASSERT(IsZeroMemory(Handle, sizeof(CONSOLE_HANDLE)));
    __ConsoleFree(Handle);
}

static PCONSOLE_HANDLE
__ConsoleFindHandle(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    KIRQL                   Irql;
    PLIST_ENTRY             ListEntry;
    PCONSOLE_HANDLE         Handle;
    NTSTATUS                status;

    KeAcquireSpinLock(&Console->Lock, &Irql);

    for (ListEntry = Console->List.Flink;
         ListEntry != &Console->List;
         ListEntry = ListEntry->Flink) {
        Handle = CONTAINING_RECORD(ListEntry,
                                   CONSOLE_HANDLE,
                                   ListEntry);

        if (Handle->FileObject == FileObject)
            goto found;
    }

    status = STATUS_UNSUCCESSFUL;
    goto fail1;

found:
    KeReleaseSpinLock(&Console->Lock, Irql);

    return Handle;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Console->Lock, Irql);

    return NULL;
}

static NTSTATUS
ConsoleOpen(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    PCONSOLE_HANDLE         Handle;
    KIRQL                   Irql;
    NTSTATUS                status;

    status = __ConsoleCreateHandle(Console, FileObject, &Handle);
    if (!NT_SUCCESS(status))
        goto fail1;

    KeAcquireSpinLock(&Console->Lock, &Irql);
    InsertTailList(&Console->List, &Handle->ListEntry);
    KeReleaseSpinLock(&Console->Lock, Irql);

    Trace("%p\n", Handle->FileObject);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
ConsoleClose(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    PCONSOLE_HANDLE         Handle;
    KIRQL                   Irql;
    NTSTATUS                status;

    Handle = __ConsoleFindHandle(Console, FileObject);

    status = STATUS_UNSUCCESSFUL;
    if (Handle == NULL)
        goto fail1;

    Trace("%p\n", Handle->FileObject);

    KeAcquireSpinLock(&Console->Lock, &Irql);
    RemoveEntryList(&Handle->ListEntry);
    KeReleaseSpinLock(&Console->Lock, Irql);

    __ConsoleDestroyHandle(Console, Handle);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__ConsoleReadWrite(
    IN  PXENCONS_CONSOLE    Console,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PCONSOLE_HANDLE         Handle;
    NTSTATUS                status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    Handle = __ConsoleFindHandle(Console, StackLocation->FileObject);

    status = STATUS_UNSUCCESSFUL;
    if (Handle == NULL)
        goto fail1;

    status = StreamPutQueue(Handle->Stream, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_PENDING;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__ConsoleDeviceControl(
    IN  PXENCONS_CONSOLE    Console,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    ULONG                   IoControlCode;
    ULONG                   InputBufferLength;
    ULONG                   OutputBufferLength;
    PVOID                   Buffer;
    PCHAR                   Value;
    ULONG                   Length;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Console);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    IoControlCode = StackLocation->Parameters.DeviceIoControl.IoControlCode;
    InputBufferLength = StackLocation->Parameters.DeviceIoControl.InputBufferLength;
    OutputBufferLength = StackLocation->Parameters.DeviceIoControl.OutputBufferLength;
    Buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (IoControlCode) {
    case IOCTL_XENCONS_GET_INSTANCE:
        Value = "0";
        break;
    case IOCTL_XENCONS_GET_NAME:
        Value = "default";
        break;
    case IOCTL_XENCONS_GET_PROTOCOL:
        Value = "vt100";
        break;
    default:
        Value = NULL;
        break;
    }

    status = STATUS_NOT_SUPPORTED;
    if (Value == NULL)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (InputBufferLength != 0)
        goto fail2;

    Length = (ULONG)strlen(Value) + 1;
    Irp->IoStatus.Information = Length;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (OutputBufferLength == 0)
        goto fail3;

    RtlZeroMemory(Buffer, OutputBufferLength);
    if (OutputBufferLength < Length)
        goto fail4;

    RtlCopyMemory(Buffer, Value, Length);

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
ConsolePutQueue(
    IN  PXENCONS_CONSOLE    Console,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    NTSTATUS                status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
        status = __ConsoleReadWrite(Console, Irp);
        break;

    case IRP_MJ_DEVICE_CONTROL:
        status = __ConsoleDeviceControl(Console, Irp);
        break;

    default:
        ASSERT(FALSE);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}

static NTSTATUS
ConsoleD3ToD0(
    IN  PXENCONS_CONSOLE    Console
    )
{ 
    Trace("====>\n");

    UNREFERENCED_PARAMETER(Console);

    Trace("<====\n");

    return STATUS_SUCCESS;
}

static VOID
ConsoleD0ToD3(
    IN  PXENCONS_CONSOLE    Console
    )
{
    KIRQL                   Irql;
    LIST_ENTRY              List;
    PLIST_ENTRY             ListEntry;
    PCONSOLE_HANDLE         Handle;

    Trace("====>\n");

    InitializeListHead(&List);

    KeAcquireSpinLock(&Console->Lock, &Irql);

    ListEntry = Console->List.Flink;
    if (!IsListEmpty(&Console->List)) {
        RemoveEntryList(&Console->List);
        InitializeListHead(&Console->List);
        AppendTailList(&List, ListEntry);
    }

    KeReleaseSpinLock(&Console->Lock, Irql);

    while (!IsListEmpty(&List)) {
        ListEntry = RemoveHeadList(&List);
        ASSERT3P(ListEntry, != , &List);

        Handle = CONTAINING_RECORD(ListEntry,
                                   CONSOLE_HANDLE,
                                   ListEntry);

        __ConsoleDestroyHandle(Console, Handle);
    }

    Trace("<====\n");
}

static NTSTATUS
ConsoleAbiAcquire(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;
    KIRQL                               Irql;

    Trace("====>\n");

    KeAcquireSpinLock(&Console->Lock, &Irql);

    Console->References++;

    KeReleaseSpinLock(&Console->Lock, Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;
}

static VOID
ConsoleAbiRelease(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;
    KIRQL                               Irql;

    Trace("====>\n");

    KeAcquireSpinLock(&Console->Lock, &Irql);

    ASSERT(Console->References != 0);
    --Console->References;

    KeReleaseSpinLock(&Console->Lock, Irql);

    Trace("<====\n");
}

static NTSTATUS
ConsoleAbiD3ToD0(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;

    return ConsoleD3ToD0(Console);
}

static VOID
ConsoleAbiD0ToD3(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;

    ConsoleD0ToD3(Console);
}

static NTSTATUS
ConsoleAbiOpen(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PFILE_OBJECT                    FileObject
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;

    return ConsoleOpen(Console, FileObject);
}

static NTSTATUS
ConsoleAbiClose(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PFILE_OBJECT                    FileObject
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;

    return ConsoleClose(Console, FileObject);
}

static NTSTATUS
ConsoleAbiPutQueue(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PIRP                            Irp
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;

    return ConsolePutQueue(Console, Irp);
}

static XENCONS_CONSOLE_ABI ConsoleAbi = {
    NULL,
    ConsoleAbiAcquire,
    ConsoleAbiRelease,
    ConsoleAbiD3ToD0,
    ConsoleAbiD0ToD3,
    ConsoleAbiOpen,
    ConsoleAbiClose,
    ConsoleAbiPutQueue
};

NTSTATUS
ConsoleCreate(
    IN  PXENCONS_FDO                    Fdo,
    OUT PXENCONS_CONSOLE_ABI_CONTEXT    *Context
    )
{
    PXENCONS_CONSOLE                    Console;
    NTSTATUS                            status;

    Trace("====>\n");

    Console = __ConsoleAllocate(sizeof(XENCONS_CONSOLE));

    status = STATUS_NO_MEMORY;
    if (Console == NULL)
        goto fail1;

    InitializeListHead(&Console->List);
    KeInitializeSpinLock(&Console->Lock);

    Console->Fdo = Fdo;

    *Context = (PVOID)Console;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
ConsoleGetAbi(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    OUT PXENCONS_CONSOLE_ABI            Abi
    )
{
    *Abi = ConsoleAbi;

    Abi->Context = Context;
}

VOID
ConsoleDestroy(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    )
{
    PXENCONS_CONSOLE                    Console = (PXENCONS_CONSOLE)Context;
    
    Trace("====>\n");

    ASSERT(IsListEmpty(&Console->List));
    RtlZeroMemory(&Console->List, sizeof(LIST_ENTRY));

    RtlZeroMemory(&Console->Lock, sizeof(KSPIN_LOCK));

    Console->Fdo = NULL;

    ASSERT(IsZeroMemory(Console, sizeof(XENCONS_CONSOLE)));
    __ConsoleFree(Console);

    Trace("<====\n");
}
