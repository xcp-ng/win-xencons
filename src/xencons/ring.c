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

#include <ntddk.h>
#include <ntstrsafe.h>
#include <stdlib.h>

#include <xen.h>
#include <debug_interface.h>
#include <store_interface.h>
#include <gnttab_interface.h>
#include <evtchn_interface.h>

#include "frontend.h"
#include "ring.h"
#include "names.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENCONS_QUEUE {
    IO_CSQ                  Csq;
    LIST_ENTRY              List;
    KSPIN_LOCK           	Lock;
} XENCONS_QUEUE, *PXENCONS_QUEUE;

struct _XENCONS_RING {
    PXENCONS_FRONTEND           Frontend;
    BOOLEAN                     Connected;
    BOOLEAN                     Enabled;
    KSPIN_LOCK                  Lock;
    PXENBUS_GNTTAB_CACHE        GnttabCache;
    struct xencons_interface    *Shared;
    PMDL                        Mdl;
    PXENBUS_GNTTAB_ENTRY        Entry;
    KDPC                        Dpc;
    ULONG                       Dpcs;
    ULONG                       Events;
    PXENBUS_EVTCHN_CHANNEL      Channel;
    XENBUS_GNTTAB_INTERFACE     GnttabInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;
    XENBUS_EVTCHN_INTERFACE     EvtchnInterface;
    XENBUS_DEBUG_INTERFACE      DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    XENCONS_QUEUE               Read;
    XENCONS_QUEUE               Write;
    ULONG                       BytesRead;
    ULONG                       BytesWritten;
};

#define MAXNAMELEN          128
#define XENCONS_RING_TAG  'GNIR'

static FORCEINLINE PVOID
__RingAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENCONS_RING_TAG);
}

static FORCEINLINE VOID
__RingFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENCONS_RING_TAG);
}

IO_CSQ_INSERT_IRP_EX RingCsqInsertIrpEx;

NTSTATUS
RingCsqInsertIrpEx(
    _In_ PIO_CSQ        Csq,
    _In_ PIRP           Irp,
    _In_ PVOID          InsertContext OPTIONAL
    )
{
    BOOLEAN             ReInsert = (BOOLEAN)(ULONG_PTR)InsertContext;
    PXENCONS_QUEUE      Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_QUEUE, Csq);

    if (ReInsert) {
        // This only occurs if the DPC de-queued the IRP but
        // then found the console to be blocked.
        InsertHeadList(&Queue->List, &Irp->Tail.Overlay.ListEntry);
    } else {
        InsertTailList(&Queue->List, &Irp->Tail.Overlay.ListEntry);
    }

    return STATUS_PENDING;
}

IO_CSQ_REMOVE_IRP RingCsqRemoveIrp;

VOID
RingCsqRemoveIrp(
    _In_ PIO_CSQ    Csq,
    _In_ PIRP       Irp
    )
{
    UNREFERENCED_PARAMETER(Csq);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

_Function_class_(IO_CSQ_PEEK_NEXT_IRP)
PIRP
RingCsqPeekNextIrp(
    _In_ PIO_CSQ    Csq,
    _In_ PIRP       Irp,
    _In_opt_ PVOID  PeekContext
    )
{
    PXENCONS_QUEUE  Queue;
    PLIST_ENTRY     ListEntry;
    PIRP            NextIrp;

    Queue = CONTAINING_RECORD(Csq, XENCONS_QUEUE, Csq);

    ListEntry = (Irp == NULL) ?
        Queue->List.Flink :
        Irp->Tail.Overlay.ListEntry.Flink;

    while (ListEntry != &Queue->List) {
        PIO_STACK_LOCATION  StackLocation;

        NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
        if (PeekContext == NULL)
            return NextIrp;

        StackLocation = IoGetCurrentIrpStackLocation(NextIrp);
        if (StackLocation->FileObject == (PFILE_OBJECT)PeekContext)
            return NextIrp;

        ListEntry = ListEntry->Flink;
    }

    return NULL;
}

#pragma warning(push)
#pragma warning(disable:28167) // function changes IRQL

_Function_class_(IO_CSQ_ACQUIRE_LOCK)
_Requires_lock_not_held_(Csq)
_Acquires_lock_(Csq)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_raises_(DISPATCH_LEVEL)
VOID
RingCsqAcquireLock(
    _In_ PIO_CSQ                            Csq,
    _Out_ _At_(*Irql, _IRQL_saves_) PKIRQL  Irql
    )
{
    PXENCONS_QUEUE                          Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_QUEUE, Csq);

    KeAcquireSpinLock(&Queue->Lock, Irql);
}

_Function_class_(IO_CSQ_RELEASE_LOCK)
_Requires_lock_held_(Csq)
_Releases_lock_(Csq)
_IRQL_requires_(DISPATCH_LEVEL)
VOID
RingCsqReleaseLock(
    _In_ PIO_CSQ                Csq,
    _In_ _IRQL_restores_ KIRQL  Irql
    )
{
    PXENCONS_QUEUE              Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_QUEUE, Csq);

    _Analysis_assume_lock_held_(Queue->Lock);
    KeReleaseSpinLock(&Queue->Lock, Irql);
}

#pragma warning(pop)

_Function_class_(IO_CSQ_COMPLETE_CANCELED_IRP)
VOID
RingCsqCompleteCanceledIrp(
    _In_ PIO_CSQ        Csq,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MajorFunction;

    UNREFERENCED_PARAMETER(Csq);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MajorFunction = StackLocation->MajorFunction;

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_CANCELLED;

    Trace("CANCELLED (%02x:%s)\n",
          MajorFunction,
          MajorFunctionName(MajorFunction));

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static FORCEINLINE VOID
__RingCancelRequests(
    _In_ PXENCONS_RING      Ring,
    _In_opt_ PFILE_OBJECT   FileObject
    )
{
    for (;;) {
        PIRP    Irp;

        Irp = IoCsqRemoveNextIrp(&Ring->Read.Csq, FileObject);
        if (Irp == NULL)
            break;

        RingCsqCompleteCanceledIrp(&Ring->Read.Csq, Irp);
    }
    for (;;) {
        PIRP    Irp;

        Irp = IoCsqRemoveNextIrp(&Ring->Write.Csq, FileObject);
        if (Irp == NULL)
            break;

        RingCsqCompleteCanceledIrp(&Ring->Write.Csq, Irp);
    }
}

_Requires_lock_not_held_(*Argument)
_Acquires_lock_(*Argument)
_IRQL_requires_min_(DISPATCH_LEVEL)
static VOID
RingAcquireLock(
    _In_ PVOID      Argument
    )
{
    PXENCONS_RING   Ring = Argument;

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
}

_Requires_lock_held_(*Argument)
_Releases_lock_(*Argument)
_IRQL_requires_min_(DISPATCH_LEVEL)
static VOID
RingReleaseLock(
    _In_ PVOID      Argument
    )
{
    PXENCONS_RING   Ring = Argument;

    _Analysis_assume_lock_held_(Ring->Lock);
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);
}

NTSTATUS
RingOpen(
    _In_ PXENCONS_RING  Ring,
    _In_ PFILE_OBJECT   FileObject
    )
{
    UNREFERENCED_PARAMETER(Ring);
    UNREFERENCED_PARAMETER(FileObject);
    return STATUS_SUCCESS;
}

NTSTATUS
RingClose(
    _In_ PXENCONS_RING  Ring,
    _In_ PFILE_OBJECT   FileObject
    )
{
    __RingCancelRequests(Ring, FileObject);
    return STATUS_SUCCESS;
}

NTSTATUS
RingPutQueue(
    _In_ PXENCONS_RING  Ring,
    _In_ PIRP           Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_READ:
        status = IoCsqInsertIrpEx(&Ring->Read.Csq,
                                  Irp,
                                  NULL,
                                  (PVOID)FALSE);
        break;

    case IRP_MJ_WRITE:
        status = IoCsqInsertIrpEx(&Ring->Write.Csq,
                                  Irp,
                                  NULL,
                                  (PVOID)FALSE);
        break;

    default:
        ASSERT(FALSE);
        status = STATUS_NOT_SUPPORTED; // Keep SDV happy
        break;
    }
    if (status != STATUS_PENDING)
        goto fail1;

    KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);
    return STATUS_PENDING;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static ULONG
RingCopyFromRead(
    _In_ PXENCONS_RING          Ring,
    _In_ PCHAR                  Data,
    _In_ ULONG                  Length
    )
{
    struct xencons_interface    *Shared;
    XENCONS_RING_IDX            cons;
    XENCONS_RING_IDX            prod;
    ULONG                       Offset;

    Shared = Ring->Shared;

    KeMemoryBarrier();

    cons = Shared->in_cons;
    prod = Shared->in_prod;

    KeMemoryBarrier();

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = prod - cons;

        if (Available == 0)
            break;

        Index = MASK_XENCONS_IDX(cons, Shared->in);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, sizeof(Shared->in) - Index);

        RtlCopyMemory(Data + Offset, &Shared->in[Index], CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;

        cons += CopyLength;
    }

    KeMemoryBarrier();

    Shared->in_cons = cons;

    KeMemoryBarrier();

    return Offset;
}

static ULONG
RingCopyToWrite(
    _In_ PXENCONS_RING          Ring,
    _In_ PCHAR                  Data,
    _In_ ULONG                  Length
    )
{
    struct xencons_interface    *Shared;
    XENCONS_RING_IDX            cons;
    XENCONS_RING_IDX            prod;
    ULONG                       Offset;

    Shared = Ring->Shared;

    KeMemoryBarrier();

    prod = Shared->out_prod;
    cons = Shared->out_cons;

    KeMemoryBarrier();

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = cons + sizeof(Shared->out) - prod;

        if (Available == 0)
            break;

        Index = MASK_XENCONS_IDX(prod, Shared->out);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, sizeof(Shared->out) - Index);

        RtlCopyMemory(&Shared->out[Index], Data + Offset, CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;

        prod += CopyLength;
    }

    KeMemoryBarrier();

    Shared->out_prod = prod;

    KeMemoryBarrier();

    return Offset;
}

static BOOLEAN
RingPoll(
    _In_ PXENCONS_RING  Ring
    )
{
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Length;
    PCHAR               Buffer;
    NTSTATUS            status;

    for (;;) {
        ULONG           Read;

        Irp = IoCsqRemoveNextIrp(&Ring->Read.Csq, NULL);
        if (Irp == NULL)
            break;

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(StackLocation->MajorFunction == IRP_MJ_READ);

        Length = StackLocation->Parameters.Read.Length;
        Buffer = Irp->AssociatedIrp.SystemBuffer;

        Read = RingCopyFromRead(Ring,
                                Buffer,
                                Length);
        if (Read == 0) {
            status = IoCsqInsertIrpEx(&Ring->Read.Csq,
                                      Irp,
                                      NULL,
                                      (PVOID)TRUE);
            ASSERT(status == STATUS_PENDING);
            break;
        }

        Ring->BytesRead += Read;

        Irp->IoStatus.Information = Read;
        Irp->IoStatus.Status = STATUS_SUCCESS;

        Trace("COMPLETE (READ) (%u bytes)\n",
              Irp->IoStatus.Information);

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    for (;;) {
        ULONG           Written;

        Irp = IoCsqRemoveNextIrp(&Ring->Write.Csq, NULL);
        if (Irp == NULL)
            break;

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(StackLocation->MajorFunction == IRP_MJ_WRITE);

        Length = StackLocation->Parameters.Write.Length;
        Buffer = Irp->AssociatedIrp.SystemBuffer;

        Written = RingCopyToWrite(Ring,
                                  Buffer,
                                  Length);
        if (Written == 0) {
            status = IoCsqInsertIrpEx(&Ring->Write.Csq,
                                      Irp,
                                      NULL,
                                      (PVOID)TRUE);
            ASSERT(status == STATUS_PENDING);
            break;
        }

        Ring->BytesWritten += Written;

        Irp->IoStatus.Information = Written;
        Irp->IoStatus.Status = STATUS_SUCCESS;

        Trace("COMPLETE (WRITE) (%u bytes)\n",
              Irp->IoStatus.Information);

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return FALSE;
}

_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_same_
static VOID
RingDpc(
    _In_ PKDPC          Dpc,
    _In_ PVOID          Context,
    _In_ PVOID          Argument1,
    _In_ PVOID          Argument2
    )
{
    PXENCONS_RING       Ring = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Ring != NULL);

    for (;;) {
        BOOLEAN Enabled;
        BOOLEAN Retry;
        KIRQL   Irql;

        KeAcquireSpinLock(&Ring->Lock, &Irql);
        Enabled = Ring->Enabled;
        KeReleaseSpinLock(&Ring->Lock, Irql);

        if (!Enabled)
            break;

        KeRaiseIrql(DISPATCH_LEVEL, &Irql);
        Retry = RingPoll(Ring);
        KeLowerIrql(Irql);

        if (!Retry)
            break;
    }

    (VOID) XENBUS_EVTCHN(Unmask,
                         &Ring->EvtchnInterface,
                         Ring->Channel,
                         FALSE,
                         FALSE);
}

_Function_class_(KSERVICE_ROUTINE)
BOOLEAN
RingEvtchnCallback(
    _In_ PKINTERRUPT    InterruptObject,
    _In_ PVOID          Argument
    )
{
    PXENCONS_RING       Ring = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Ring != NULL);

    Ring->Events++;

    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;

    return TRUE;
}

static VOID
RingDebugCallback(
    _In_ PVOID      Argument,
    _In_ BOOLEAN    Crashing
    )
{
    PXENCONS_RING   Ring = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "0x%p [%s]\n",
                 Ring,
                 (Ring->Enabled) ? "ENABLED" : "DISABLED");

    // Dump shared ring
    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "SHARED: in_cons = %u in_prod = %u out_cons = %u out_prod = %u\n",
                 Ring->Shared->in_cons,
                 Ring->Shared->in_prod,
                 Ring->Shared->out_cons,
                 Ring->Shared->out_prod);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "BYTES: read = %u written = %u\n",
                 Ring->BytesRead,
                 Ring->BytesWritten);
}

NTSTATUS
RingEnable(
    _In_ PXENCONS_RING  Ring
    )
{
    Trace("====>\n");

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
    Ring->Enabled = TRUE;
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);

    (VOID)KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);

    Trace("<====\n");

    return STATUS_SUCCESS;
}

VOID
RingDisable(
    _In_ PXENCONS_RING  Ring
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
    Ring->Enabled = FALSE;
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);

    Trace("<====\n");
}

NTSTATUS
RingConnect(
    _In_ PXENCONS_RING  Ring
    )
{
    CHAR                Name[MAXNAMELEN];
    NTSTATUS            status;

    Trace("====>\n");

    ASSERT(!Ring->Connected);

    status = XENBUS_DEBUG(Acquire, &Ring->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_EVTCHN(Acquire, &Ring->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_GNTTAB(Acquire, &Ring->GnttabInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_STORE(Acquire, &Ring->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = RtlStringCbPrintfA(Name,
                                sizeof(Name),
                                "console_%s_gnttab",
                                PdoGetName(FrontendGetPdo(Ring->Frontend)));
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_GNTTAB(CreateCache,
                           &Ring->GnttabInterface,
                           Name,
                           0,
                           0,
                           RingAcquireLock,
                           RingReleaseLock,
                           Ring,
                           &Ring->GnttabCache);
    if (!NT_SUCCESS(status))
        goto fail6;

    Ring->Mdl = __AllocatePage();

    status = STATUS_NO_MEMORY;
    if (Ring->Mdl == NULL)
        goto fail7;

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    Ring->Shared = Ring->Mdl->MappedSystemVa;
    ASSERT(Ring->Shared != NULL);

    status = XENBUS_GNTTAB(PermitForeignAccess,
                           &Ring->GnttabInterface,
                           Ring->GnttabCache,
                           TRUE,
                           FrontendGetBackendDomain(Ring->Frontend),
                           MmGetMdlPfnArray(Ring->Mdl)[0],
                           FALSE,
                           &Ring->Entry);
    if (!NT_SUCCESS(status))
        goto fail8;

    Ring->Channel = XENBUS_EVTCHN(Open,
                                  &Ring->EvtchnInterface,
                                  XENBUS_EVTCHN_TYPE_UNBOUND,
                                  RingEvtchnCallback,
                                  Ring,
                                  FrontendGetBackendDomain(Ring->Frontend),
                                  TRUE);

    status = STATUS_UNSUCCESSFUL;
    if (Ring->Channel == NULL)
        goto fail9;

    (VOID)XENBUS_EVTCHN(Unmask,
                        &Ring->EvtchnInterface,
                        Ring->Channel,
                        FALSE,
                        TRUE);

    status = XENBUS_DEBUG(Register,
                          &Ring->DebugInterface,
                          __MODULE__ "|POLLER",
                          RingDebugCallback,
                          Ring,
                          &Ring->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail10;

    Ring->Connected = TRUE;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    Ring->Events = 0;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

fail9:
    Error("fail9\n");

    (VOID)XENBUS_GNTTAB(RevokeForeignAccess,
                        &Ring->GnttabInterface,
                        Ring->GnttabCache,
                        TRUE,
                        Ring->Entry);
    Ring->Entry = NULL;

fail8:
    Error("fail8\n");

    RtlZeroMemory(Ring->Shared, PAGE_SIZE);

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

fail7:
    Error("fail7\n");

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

    XENBUS_STORE(Release, &Ring->StoreInterface);

fail4:
    Error("fail4\n");

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

fail3:
    Error("fail3\n");

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
RingStoreWrite(
    _In_ PXENCONS_RING  Ring,
    _In_ PVOID          Transaction
    )
{
    ULONG               Port;
    ULONG               GrantRef;
    NTSTATUS            status;

    Port = XENBUS_EVTCHN(GetPort,
                         &Ring->EvtchnInterface,
                         Ring->Channel);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "port",
                          "%u",
                          Port);
    if (!NT_SUCCESS(status))
        goto fail1;

    GrantRef = XENBUS_GNTTAB(GetReference,
                             &Ring->GnttabInterface,
                             Ring->Entry);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "ring-ref",
                          "%u",
                          GrantRef);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RingDisconnect(
    _In_ PXENCONS_RING  Ring
    )
{
    Trace("====>\n");

    ASSERT(Ring->Connected);
    Ring->Connected = FALSE;

    XENBUS_DEBUG(Deregister,
                 &Ring->DebugInterface,
                 Ring->DebugCallback);
    Ring->DebugCallback = NULL;

    Ring->Dpcs = 0;
    Ring->Events = 0;
    Ring->BytesRead = 0;
    Ring->BytesWritten = 0;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    (VOID)XENBUS_GNTTAB(RevokeForeignAccess,
                        &Ring->GnttabInterface,
                        Ring->GnttabCache,
                        TRUE,
                        Ring->Entry);
    Ring->Entry = NULL;

    RtlZeroMemory(Ring->Shared, PAGE_SIZE);

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

    XENBUS_STORE(Release, &Ring->StoreInterface);

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

    Trace("<====\n");
}

NTSTATUS
RingCreate(
    _In_ PXENCONS_FRONTEND  Frontend,
    _Out_ PXENCONS_RING     *Ring
    )
{
    NTSTATUS                status;

    *Ring = __RingAllocate(sizeof(XENCONS_RING));

    status = STATUS_NO_MEMORY;
    if (*Ring == NULL)
        goto fail1;

    (*Ring)->Frontend = Frontend;

    FdoGetDebugInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->DebugInterface);

    FdoGetEvtchnInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->EvtchnInterface);

    FdoGetGnttabInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->GnttabInterface);

    FdoGetStoreInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->StoreInterface);

    KeInitializeSpinLock(&(*Ring)->Lock);

    KeInitializeThreadedDpc(&(*Ring)->Dpc, RingDpc, *Ring);

    KeInitializeSpinLock(&(*Ring)->Read.Lock);
    InitializeListHead(&(*Ring)->Read.List);

    status = IoCsqInitializeEx(&(*Ring)->Read.Csq,
                               RingCsqInsertIrpEx,
                               RingCsqRemoveIrp,
                               RingCsqPeekNextIrp,
                               RingCsqAcquireLock,
                               RingCsqReleaseLock,
                               RingCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeInitializeSpinLock(&(*Ring)->Write.Lock);
    InitializeListHead(&(*Ring)->Write.List);

    status = IoCsqInitializeEx(&(*Ring)->Write.Csq,
                               RingCsqInsertIrpEx,
                               RingCsqRemoveIrp,
                               RingCsqPeekNextIrp,
                               RingCsqAcquireLock,
                               RingCsqReleaseLock,
                               RingCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail3;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    RtlZeroMemory(&(*Ring)->Write.List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&(*Ring)->Write.Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&(*Ring)->Read.Csq, sizeof(IO_CSQ));

fail2:
    Error("fail2\n");

    RtlZeroMemory(&(*Ring)->Read.List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&(*Ring)->Read.Lock, sizeof(KSPIN_LOCK));

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RingDestroy(
    _In_ PXENCONS_RING  Ring
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , PASSIVE_LEVEL);

    // Cancel all outstanding IRPs
    __RingCancelRequests(Ring, NULL);

    ASSERT(IsListEmpty(&Ring->Read.List));
    ASSERT(IsListEmpty(&Ring->Write.List));

    RtlZeroMemory(&Ring->Write.Csq, sizeof(IO_CSQ));

    RtlZeroMemory(&Ring->Write.List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Ring->Write.Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&Ring->Read.Csq, sizeof(IO_CSQ));

    RtlZeroMemory(&Ring->Read.List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Ring->Read.Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&Ring->Dpc, sizeof(KDPC));

    RtlZeroMemory(&Ring->Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&Ring->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Ring->GnttabInterface,
                  sizeof(XENBUS_GNTTAB_INTERFACE));

    RtlZeroMemory(&Ring->EvtchnInterface,
                  sizeof(XENBUS_EVTCHN_INTERFACE));

    RtlZeroMemory(&Ring->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    Ring->Frontend = NULL;

    ASSERT(IsZeroMemory(Ring, sizeof(XENCONS_RING)));
    __RingFree(Ring);
}
