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

#include "fdo.h"
#include "stream.h"
#include "thread.h"
#include "names.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define STREAM_POOL 'ETRS'

struct _XENCONS_STREAM {
    PXENCONS_FDO            	Fdo;
    PXENCONS_THREAD         	Thread;
    IO_CSQ                  	Csq;
    LIST_ENTRY              	List;
    KSPIN_LOCK           	Lock;
    XENBUS_CONSOLE_INTERFACE 	ConsoleInterface;
};

static FORCEINLINE PVOID
__StreamAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, STREAM_POOL);
}

static FORCEINLINE VOID
__StreamFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, STREAM_POOL);
}

IO_CSQ_INSERT_IRP_EX StreamCsqInsertIrpEx;

NTSTATUS
StreamCsqInsertIrpEx(
    IN  PIO_CSQ         Csq,
    IN  PIRP            Irp,
    IN  PVOID           InsertContext OPTIONAL
    )
{
    BOOLEAN             ReInsert = (BOOLEAN)(ULONG_PTR)InsertContext;
    PXENCONS_STREAM     Stream;

    Stream = CONTAINING_RECORD(Csq, XENCONS_STREAM, Csq);

    if (ReInsert) {
        // This only occurs if the worker thread de-queued the IRP but
        // then found the console to be blocked.
        InsertHeadList(&Stream->List, &Irp->Tail.Overlay.ListEntry);
    } else {
        InsertTailList(&Stream->List, &Irp->Tail.Overlay.ListEntry);
        ThreadWake(Stream->Thread);
    }

    return STATUS_SUCCESS;
}

IO_CSQ_REMOVE_IRP StreamCsqRemoveIrp;

VOID
StreamCsqRemoveIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    UNREFERENCED_PARAMETER(Csq);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

IO_CSQ_PEEK_NEXT_IRP StreamCsqPeekNextIrp;

PIRP
StreamCsqPeekNextIrp(
    IN  PIO_CSQ     Csq,
    IN  PIRP        Irp,
    IN  PVOID       PeekContext OPTIONAL
    )
{
    PXENCONS_STREAM Stream;
    PLIST_ENTRY     ListEntry;
    PIRP            NextIrp;

    UNREFERENCED_PARAMETER(PeekContext);

    Stream = CONTAINING_RECORD(Csq, XENCONS_STREAM, Csq);

    ListEntry = (Irp == NULL) ?
                Stream->List.Flink :
                Irp->Tail.Overlay.ListEntry.Flink;

    if (ListEntry == &Stream->List)
        return NULL;

    NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);

    return NextIrp;
}

#pragma warning(push)
#pragma warning(disable:28167) // function changes IRQL

IO_CSQ_ACQUIRE_LOCK StreamCsqAcquireLock;

VOID
StreamCsqAcquireLock(
    IN  PIO_CSQ Csq,
    OUT PKIRQL  Irql
    )
{
    PXENCONS_STREAM Stream;

    Stream = CONTAINING_RECORD(Csq, XENCONS_STREAM, Csq);

    KeAcquireSpinLock(&Stream->Lock, Irql);
}

IO_CSQ_RELEASE_LOCK StreamCsqReleaseLock;

VOID
StreamCsqReleaseLock(
    IN  PIO_CSQ Csq,
    IN  KIRQL   Irql
    )
{
    PXENCONS_STREAM Stream;

    Stream = CONTAINING_RECORD(Csq, XENCONS_STREAM, Csq);

    KeReleaseSpinLock(&Stream->Lock, Irql);
}

#pragma warning(pop)

IO_CSQ_COMPLETE_CANCELED_IRP StreamCsqCompleteCanceledIrp;

VOID
StreamCsqCompleteCanceledIrp(
    IN  PIO_CSQ Csq,
    IN  PIRP    Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MajorFunction;

    UNREFERENCED_PARAMETER(Csq);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MajorFunction = StackLocation->MajorFunction;

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_CANCELLED;

    Trace("COMPLETE (%02x:%s)\n",
          MajorFunction,
          MajorFunctionName(MajorFunction));

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS
StreamWorker(
    IN  PXENCONS_THREAD     Self,
    IN  PVOID               Context
    )
{
    PXENCONS_STREAM         Stream = Context;
    PKEVENT                 Event;
    PXENBUS_CONSOLE_WAKEUP  Wakeup;
    NTSTATUS                status;

    Trace("====>\n");

    Event = ThreadGetEvent(Self);

    status = XENBUS_CONSOLE(Acquire,
                            &Stream->ConsoleInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_CONSOLE(WakeupAdd,
                            &Stream->ConsoleInterface,
                            Event,
                            &Wakeup);
    if (!NT_SUCCESS(status))
        goto fail2;

    for (;;) {
        PIRP    Irp;

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        for (Irp = IoCsqRemoveNextIrp(&Stream->Csq, NULL);
             Irp != NULL;
             Irp = IoCsqRemoveNextIrp(&Stream->Csq, NULL)) {
            PIO_STACK_LOCATION  StackLocation;
            UCHAR               MajorFunction;
            BOOLEAN             Blocked;

            StackLocation = IoGetCurrentIrpStackLocation(Irp);
            MajorFunction = StackLocation->MajorFunction;

            switch (MajorFunction) {
            case IRP_MJ_READ:
                Blocked = !XENBUS_CONSOLE(CanRead,
                                          &Stream->ConsoleInterface);
                break;

            case IRP_MJ_WRITE:
                Blocked = !XENBUS_CONSOLE(CanWrite,
                                          &Stream->ConsoleInterface);
                break;

            default:
                ASSERT(FALSE);

                Blocked = TRUE;
                break;
            }

            if (Blocked) {
                status = IoCsqInsertIrpEx(&Stream->Csq,
                                          Irp,
                                          NULL,
                                          (PVOID)TRUE);
                ASSERT(NT_SUCCESS(status));

                break;
            }

            switch (MajorFunction) {
            case IRP_MJ_READ: {
                ULONG   Length;
                PCHAR   Buffer;
                ULONG   Read;

                Length = StackLocation->Parameters.Read.Length;
                Buffer = Irp->AssociatedIrp.SystemBuffer;

                Read = XENBUS_CONSOLE(Read,
                                      &Stream->ConsoleInterface,
                                      Buffer,
                                      Length);

                Irp->IoStatus.Information = Read;
                Irp->IoStatus.Status = STATUS_SUCCESS;
                break;
            }
            case IRP_MJ_WRITE: {
                ULONG   Length;
                PCHAR   Buffer;
                ULONG   Written;

                Length = StackLocation->Parameters.Write.Length;
                Buffer = Irp->AssociatedIrp.SystemBuffer;

                Written = XENBUS_CONSOLE(Write,
                                         &Stream->ConsoleInterface,
                                         Buffer,
                                         Length);

                Irp->IoStatus.Information = Written;
                Irp->IoStatus.Status = STATUS_SUCCESS;
                break;
            }
            default:
                ASSERT(FALSE);

                Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
                break;
            }

            Trace("COMPLETE (%02x:%s) (%u bytes)\n",
                  MajorFunction,
                  MajorFunctionName(MajorFunction),
                  Irp->IoStatus.Information);

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }
    }

    XENBUS_CONSOLE(WakeupRemove,
                   &Stream->ConsoleInterface,
                   Wakeup);

    XENBUS_CONSOLE(Release, &Stream->ConsoleInterface);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    XENBUS_CONSOLE(Release, &Stream->ConsoleInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
StreamCreate(
    IN  PXENCONS_FDO    Fdo,
    OUT PXENCONS_STREAM *Stream
    )
{
    NTSTATUS            status;

    *Stream = __StreamAllocate(sizeof (XENCONS_STREAM));

    status = STATUS_NO_MEMORY;
    if (*Stream == NULL)
        goto fail1;

    FdoGetConsoleInterface(Fdo, &(*Stream)->ConsoleInterface);

    KeInitializeSpinLock(&(*Stream)->Lock);
    InitializeListHead(&(*Stream)->List);

    status = IoCsqInitializeEx(&(*Stream)->Csq,
                               StreamCsqInsertIrpEx,
                               StreamCsqRemoveIrp,
                               StreamCsqPeekNextIrp,
                               StreamCsqAcquireLock,
                               StreamCsqReleaseLock,
                               StreamCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = ThreadCreate(StreamWorker,
                          *Stream,
                          &(*Stream)->Thread);
    if (!NT_SUCCESS(status))
        goto fail3;

    (*Stream)->Fdo = Fdo;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    RtlZeroMemory(&(*Stream)->Csq, sizeof (IO_CSQ));

fail2:
    Error("fail2\n");

    RtlZeroMemory(&(*Stream)->List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&(*Stream)->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&(*Stream)->ConsoleInterface,
                  sizeof (XENBUS_CONSOLE_INTERFACE));

    ASSERT(IsZeroMemory(*Stream, sizeof (XENCONS_STREAM)));
    __StreamFree(*Stream);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
StreamDestroy(
    IN  PXENCONS_STREAM Stream
    )
{
    Stream->Fdo = NULL;

    ThreadAlert(Stream->Thread);
    ThreadJoin(Stream->Thread);
    Stream->Thread = NULL;

    RtlZeroMemory(&Stream->Csq, sizeof (IO_CSQ));

    RtlZeroMemory(&Stream->List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Stream->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Stream->ConsoleInterface,
                  sizeof (XENBUS_CONSOLE_INTERFACE));

    ASSERT(IsZeroMemory(Stream, sizeof (XENCONS_STREAM)));
    __StreamFree(Stream);
}

NTSTATUS
StreamPutQueue(
    IN  PXENCONS_STREAM Stream,
    IN  PIRP            Irp
    )
{
    return IoCsqInsertIrpEx(&Stream->Csq, Irp, NULL, (PVOID)FALSE);
}
