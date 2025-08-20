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

#include <xen.h>
#include <store_interface.h>
#include <suspend_interface.h>
#include <debug_interface.h>
#include <xencons_device.h>

#include "driver.h"
#include "frontend.h"
#include "ring.h"
#include "thread.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define FRONTEND_POOL 'TNRF'
#define DOMID_INVALID (0x7FFFU)

typedef enum _FRONTEND_STATE {
    FRONTEND_UNKNOWN,
    FRONTEND_CLOSED,
    FRONTEND_PREPARED,
    FRONTEND_CONNECTED,
    FRONTEND_ENABLED
} FRONTEND_STATE, *PFRONTEND_STATE;

struct _XENCONS_FRONTEND {
    LONG                        References;
    PXENCONS_PDO                Pdo;
    PSTR                        Path;
    FRONTEND_STATE              State;
    KSPIN_LOCK                  Lock;
    PXENCONS_THREAD             EjectThread;
    KEVENT                      EjectEvent;
    BOOLEAN                     Online;

    PSTR                        BackendPath;
    USHORT                      BackendDomain;
    PSTR                        Name;
    PSTR                        Protocol;

    XENBUS_DEBUG_INTERFACE      DebugInterface;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;

    PXENBUS_SUSPEND_CALLBACK    SuspendCallback;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    PXENBUS_STORE_WATCH         Watch;

    PXENCONS_RING               Ring;
};

static PCSTR
FrontendStateName(
    _In_ FRONTEND_STATE State
    )
{
#define _STATE_NAME(_State)     \
    case  FRONTEND_ ## _State:  \
        return #_State;

    switch (State) {
        _STATE_NAME(UNKNOWN);
        _STATE_NAME(CLOSED);
        _STATE_NAME(PREPARED);
        _STATE_NAME(CONNECTED);
        _STATE_NAME(ENABLED);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

static PCSTR
XenbusStateName(
    _In_ XenbusState    State
    )
{
#define _STATE_NAME(_State)         \
    case  XenbusState ## _State:    \
        return #_State;

    switch (State) {
        _STATE_NAME(Unknown);
        _STATE_NAME(Initialising);
        _STATE_NAME(InitWait);
        _STATE_NAME(Initialised);
        _STATE_NAME(Connected);
        _STATE_NAME(Closing);
        _STATE_NAME(Closed);
        _STATE_NAME(Reconfiguring);
        _STATE_NAME(Reconfigured);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

static FORCEINLINE PVOID
__FrontendAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, FRONTEND_POOL);
}

static FORCEINLINE VOID
__FrontendFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, FRONTEND_POOL);
}

static FORCEINLINE PXENCONS_PDO
__FrontendGetPdo(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return Frontend->Pdo;
}

PXENCONS_PDO
FrontendGetPdo(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return __FrontendGetPdo(Frontend);
}

static FORCEINLINE PSTR
__FrontendGetPath(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return Frontend->Path;
}

PSTR
FrontendGetPath(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return __FrontendGetPath(Frontend);
}

static FORCEINLINE PSTR
__FrontendGetBackendPath(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return Frontend->BackendPath;
}

PSTR
FrontendGetBackendPath(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return __FrontendGetBackendPath(Frontend);
}

static FORCEINLINE USHORT
__FrontendGetBackendDomain(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return Frontend->BackendDomain;
}

USHORT
FrontendGetBackendDomain(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return __FrontendGetBackendDomain(Frontend);
}

static BOOLEAN
FrontendIsOnline(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    return Frontend->Online;
}

static BOOLEAN
FrontendIsBackendOnline(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    PSTR                    Buffer;
    BOOLEAN                 Online;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Online = FALSE;
    } else {
        Online = (BOOLEAN)strtol(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return Online;
}

static DECLSPEC_NOINLINE NTSTATUS
FrontendEject(
    _In_ PXENCONS_THREAD    Self,
    _In_ PVOID              Context
    )
{
    PXENCONS_FRONTEND       Frontend = Context;
    PKEVENT                 Event;

    Trace("%s: ====>\n", __FrontendGetPath(Frontend));

    Event = ThreadGetEvent(Self);

    for (;;) {
        KIRQL       Irql;

        KeWaitForSingleObject(Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        KeAcquireSpinLock(&Frontend->Lock, &Irql);

        // It is not safe to use interfaces before this point
        if (Frontend->State == FRONTEND_UNKNOWN ||
            Frontend->State == FRONTEND_CLOSED)
            goto loop;

        if (!FrontendIsOnline(Frontend))
            goto loop;

        if (!FrontendIsBackendOnline(Frontend))
            PdoRequestEject(__FrontendGetPdo(Frontend));

    loop:
        KeReleaseSpinLock(&Frontend->Lock, Irql);

        KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);

    Trace("%s: <====\n", __FrontendGetPath(Frontend));

    return STATUS_SUCCESS;
}

VOID
FrontendEjectFailed(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    KIRQL                   Irql;
    ULONG                   Length;
    PSTR                    Path;
    NTSTATUS                status;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: device eject failed\n", __FrontendGetPath(Frontend));

    Length = sizeof("error/") + (ULONG)strlen(__FrontendGetPath(Frontend));
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path,
                                Length,
                                "error/%s",
                                __FrontendGetPath(Frontend));
    if (!NT_SUCCESS(status))
        goto fail2;

    (VOID)XENBUS_STORE(Printf,
                       &Frontend->StoreInterface,
                       NULL,
                       Path,
                       "error",
                       "UNPLUG FAILED: device is still in use");

    __FrontendFree(Path);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
    return;

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
}

static VOID
FrontendSetOnline(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = TRUE;

    Trace("<====\n");
}

static VOID
FrontendSetOffline(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = FALSE;
    PdoRequestEject(__FrontendGetPdo(Frontend));

    Trace("<====\n");
}

static VOID
FrontendSetXenbusState(
    _In_ PXENCONS_FRONTEND  Frontend,
    _In_ XenbusState        State
    )
{
    BOOLEAN                 Online;

    Trace("%s: ====> %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));

    ASSERT(FrontendIsOnline(Frontend));

    Online = FrontendIsBackendOnline(Frontend);

    (VOID)XENBUS_STORE(Printf,
                       &Frontend->StoreInterface,
                       NULL,
                       __FrontendGetPath(Frontend),
                       "state",
                       "%u",
                       State);

    if (State == XenbusStateClosed && !Online)
        FrontendSetOffline(Frontend);

    Trace("%s: <==== %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));
}

static VOID
FrontendWaitForBackendXenbusStateChange(
    _In_     PXENCONS_FRONTEND  Frontend,
    _Inout_  XenbusState        *State
    )
{
    KEVENT                      Event;
    PXENBUS_STORE_WATCH         Watch;
    LARGE_INTEGER               Start;
    ULONGLONG                   TimeDelta;
    LARGE_INTEGER               Timeout;
    XenbusState                 Old = *State;
    NTSTATUS                    status;

    Trace("%s: ====> %s\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));

    ASSERT(FrontendIsOnline(Frontend));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "state",
                          &Event,
                          &Watch);
    if (!NT_SUCCESS(status))
        Watch = NULL;

    KeQuerySystemTime(&Start);
    TimeDelta = 0;

    Timeout.QuadPart = 0;

    while (*State == Old && TimeDelta < 120000) {
        PSTR            Buffer;
        LARGE_INTEGER   Now;

        if (Watch != NULL) {
            ULONG   Attempt = 0;

            while (++Attempt < 1000) {
                status = KeWaitForSingleObject(&Event,
                                               Executive,
                                               KernelMode,
                                               FALSE,
                                               &Timeout);
                if (status != STATUS_TIMEOUT)
                    break;

                // We are waiting for a watch event at DISPATCH_LEVEL so
                // it is our responsibility to poll the store ring.
                XENBUS_STORE(Poll,
                             &Frontend->StoreInterface);

                KeStallExecutionProcessor(1000);   // 1ms
            }

            KeClearEvent(&Event);
        }

        status = XENBUS_STORE(Read,
                              &Frontend->StoreInterface,
                              NULL,
                              __FrontendGetBackendPath(Frontend),
                              "state",
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            *State = XenbusStateUnknown;
        } else {
            *State = (XenbusState)strtol(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Frontend->StoreInterface,
                         Buffer);
        }

        KeQuerySystemTime(&Now);

        TimeDelta = (Now.QuadPart - Start.QuadPart) / 10000ull;
    }

    if (Watch != NULL)
        (VOID) XENBUS_STORE(WatchRemove,
                            &Frontend->StoreInterface,
                            Watch);

    Trace("%s: <==== (%s)\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));
}

static NTSTATUS
FrontendAcquireBackend(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    PSTR                    Buffer;
    NTSTATUS                status;

    Trace("=====>\n");

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend",
                          &Buffer);
    if (!NT_SUCCESS(status))
        goto fail1;

    Frontend->BackendPath = Buffer;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend-id",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Frontend->BackendDomain = 0;
    } else {
        Frontend->BackendDomain = (USHORT)strtol(Buffer, NULL, 10);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static VOID
FrontendReleaseBackend(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    Trace("=====>\n");

    ASSERT(Frontend->BackendDomain != DOMID_INVALID);
    ASSERT(Frontend->BackendPath != NULL);

    Frontend->BackendDomain = DOMID_INVALID;

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Frontend->BackendPath);
    Frontend->BackendPath = NULL;

    Trace("<=====\n");
}

static VOID
FrontendClose(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    XenbusState             State;

    Trace("====>\n");

    ASSERT(Frontend->Watch != NULL);
    (VOID)XENBUS_STORE(WatchRemove,
                       &Frontend->StoreInterface,
                       Frontend->Watch);
    Frontend->Watch = NULL;

    State = XenbusStateUnknown;
    while (State != XenbusStateClosed) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);

        switch (State) {
        case XenbusStateClosing:
            FrontendSetXenbusState(Frontend,
                                    XenbusStateClosed);
            break;
        case XenbusStateClosed:
            break;
        default:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosing);
            break;
        }
    }

    FrontendReleaseBackend(Frontend);

    XENBUS_STORE(Release, &Frontend->StoreInterface);

    Trace("<====\n");
}

static NTSTATUS
FrontendPrepare(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("====>\n");

    status = XENBUS_STORE(Acquire, &Frontend->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    FrontendSetOnline(Frontend);

    status = FrontendAcquireBackend(Frontend);
    if (!NT_SUCCESS(status))
        goto fail2;

    State = XenbusStateUnknown;
    while (State != XenbusStateInitWait) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        switch (State) {
        case XenbusStateInitWait:
            break;
        case XenbusStateClosed:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateClosed);
            // There is currently a bug in the backend.
            // Once the backend reaches Closed, it will crash the
            // frontend attempts to make any state transition.
            // Avoid the bug by forcing the frontend offline and
            // failing FrontendPrepare
            FrontendSetOffline(Frontend);
            break;
        default:
            FrontendSetXenbusState(Frontend,
                                    XenbusStateInitialising);
            break;
        }
    }

    status = STATUS_UNSUCCESSFUL;
    if (State != XenbusStateInitWait)
        goto fail3;

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          ThreadGetEvent(Frontend->EjectThread),
                          &Frontend->Watch);
    if (!NT_SUCCESS(status))
        goto fail4;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    FrontendReleaseBackend(Frontend);

fail2:
    Error("fail2\n");

    FrontendSetOffline(Frontend);

    XENBUS_STORE(Release, &Frontend->StoreInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendDebugCallback(
    _In_ PVOID              Argument,
    _In_ BOOLEAN            Crashing
    )
{
    PXENCONS_FRONTEND       Frontend = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "PATH: %s\n",
                 __FrontendGetPath(Frontend));
    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "NAME: %s\n",
                 Frontend->Name);
    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "PROTOCOL: %s\n",
                 Frontend->Protocol);
}

static NTSTATUS
FrontendConnect(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    XenbusState             State;
    ULONG                   Attempt;
    PSTR                    Buffer;
    ULONG                   Length;
    NTSTATUS                status;

    Trace("====>\n");

    status = XENBUS_DEBUG(Acquire, &Frontend->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_DEBUG(Register,
                          &Frontend->DebugInterface,
                          __MODULE__ "|FRONTEND",
                          FrontendDebugCallback,
                          Frontend,
                          &Frontend->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RingConnect(Frontend->Ring);
    if (!NT_SUCCESS(status))
        goto fail3;

    Attempt = 0;
    do {
        PXENBUS_STORE_TRANSACTION   Transaction;

        status = XENBUS_STORE(TransactionStart,
                              &Frontend->StoreInterface,
                              &Transaction);
        if (!NT_SUCCESS(status))
            break;

        status = RingStoreWrite(Frontend->Ring,
                                Transaction);
        if (!NT_SUCCESS(status))
            goto abort;

        status = XENBUS_STORE(TransactionEnd,
                              &Frontend->StoreInterface,
                              Transaction,
                              TRUE);
        if (status != STATUS_RETRY || ++Attempt > 10)
            break;

        continue;

    abort:
        (VOID)XENBUS_STORE(TransactionEnd,
                           &Frontend->StoreInterface,
                           Transaction,
                           FALSE);
        break;
    } while (status == STATUS_RETRY);

    if (!NT_SUCCESS(status))
        goto fail4;

    State = XenbusStateUnknown;
    while (State != XenbusStateConnected) {
        if (!FrontendIsOnline(Frontend))
            break;

        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);

        switch (State) {
        case XenbusStateInitWait:
            FrontendSetXenbusState(Frontend,
                                   XenbusStateConnected);
            break;
        case XenbusStateConnected:
            break;
        case XenbusStateUnknown:
        case XenbusStateClosing:
        case XenbusStateClosed:
            FrontendSetOffline(Frontend);
            break;
        default:
            break;
        }
    }

    status = STATUS_UNSUCCESSFUL;
    if (State != XenbusStateConnected)
        goto fail5;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "name",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Length = (ULONG)strlen(Buffer);

        Frontend->Name = __FrontendAllocate(Length + 1);
        if (Frontend->Name)
            RtlCopyMemory(Frontend->Name, Buffer, Length);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "protocol",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Length = (ULONG)strlen(Buffer);

        Frontend->Protocol = __FrontendAllocate(Length + 1);
        if (Frontend->Protocol)
            RtlCopyMemory(Frontend->Protocol, Buffer, Length);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    Trace("<====\n");
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

    RingDisconnect(Frontend->Ring);

fail3:
    Error("fail3\n");

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendDisconnect(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    Trace("====>\n");

    __FrontendFree(Frontend->Protocol);
    Frontend->Protocol = NULL;

    __FrontendFree(Frontend->Name);
    Frontend->Name = NULL;

    RingDisconnect(Frontend->Ring);

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

    Trace("<====\n");
}

static NTSTATUS
FrontendEnable(
    _In_ PXENCONS_FRONTEND   Frontend
    )
{
    NTSTATUS                status;

    Trace("====>\n");

    status = RingEnable(Frontend->Ring);
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendDisable(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    Trace("====>\n");

    RingDisable(Frontend->Ring);

    Trace("<====\n");
}

static NTSTATUS
FrontendSetState(
    _In_ PXENCONS_FRONTEND  Frontend,
    _In_ FRONTEND_STATE     State
    )
{
    BOOLEAN                 Failed;
    KIRQL                   Irql;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: ====> '%s' -> '%s'\n",
            __FrontendGetPath(Frontend),
            FrontendStateName(Frontend->State),
            FrontendStateName(State));

    Failed = FALSE;
    while (Frontend->State != State && !Failed) {
        NTSTATUS    status;

        switch (Frontend->State) {
        case FRONTEND_UNKNOWN:
            switch (State) {
            case FRONTEND_CLOSED:
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CLOSED:
            switch (State) {
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            case FRONTEND_UNKNOWN:
                Frontend->State = FRONTEND_UNKNOWN;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_PREPARED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendConnect(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_CONNECTED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    Failed = TRUE;
                }
                break;

            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CONNECTED:
            switch (State) {
            case FRONTEND_ENABLED:
                status = FrontendEnable(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_ENABLED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    FrontendDisconnect(Frontend);
                    Failed = TRUE;
                }
                break;

            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;

                FrontendDisconnect(Frontend);
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_ENABLED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendDisable(Frontend);
                Frontend->State = FRONTEND_CONNECTED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        Info("%s in state '%s'\n",
                __FrontendGetPath(Frontend),
                FrontendStateName(Frontend->State));
    }

    KeReleaseSpinLock(&Frontend->Lock, Irql);

    Info("%s: <=====\n", __FrontendGetPath(Frontend));

    return (!Failed) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static FORCEINLINE VOID
__FrontendResume(
    _In_ PXENCONS_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    ASSERT3U(Frontend->State, == , FRONTEND_UNKNOWN);
    UNREFERENCED_PARAMETER(Frontend);
    // Current backends dont like re-opening after being closed
    //(VOID)FrontendSetState(Frontend, FRONTEND_CLOSED);
}

static FORCEINLINE VOID
__FrontendSuspend(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    UNREFERENCED_PARAMETER(Frontend);
    (VOID)FrontendSetState(Frontend, FRONTEND_UNKNOWN);
}

static DECLSPEC_NOINLINE VOID
FrontendSuspendCallback(
    _In_ PVOID          Argument
    )
{
    PXENCONS_FRONTEND   Frontend = Argument;

    __FrontendSuspend(Frontend);
    __FrontendResume(Frontend);
}

static NTSTATUS
FrontendResume(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Frontend->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FrontendResume(Frontend);

    status = XENBUS_SUSPEND(Register,
            &Frontend->SuspendInterface,
            SUSPEND_CALLBACK_LATE,
            FrontendSuspendCallback,
            Frontend,
            &Frontend->SuspendCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                Executive,
                KernelMode,
                FALSE,
                NULL);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

static VOID
FrontendSuspend(
    _In_ PXENCONS_FRONTEND  Frontend
    )
{
    KIRQL                   Irql;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallback);
    Frontend->SuspendCallback = NULL;

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                Executive,
                KernelMode,
                FALSE,
                NULL);

    Trace("<====\n");
}

static NTSTATUS
FrontendGetProperty(
    _In_ PXENCONS_FRONTEND  Frontend,
    _In_ PIRP               Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    ULONG                   IoControlCode;
    ULONG                   InputBufferLength;
    ULONG                   OutputBufferLength;
    PVOID                   Buffer;
    PSTR                    Value;
    ULONG                   Length;
    NTSTATUS                status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    IoControlCode = StackLocation->Parameters.DeviceIoControl.IoControlCode;
    InputBufferLength = StackLocation->Parameters.DeviceIoControl.InputBufferLength;
    OutputBufferLength = StackLocation->Parameters.DeviceIoControl.OutputBufferLength;
    Buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (IoControlCode) {
    case IOCTL_XENCONS_GET_INSTANCE:
        Value = PdoGetName(Frontend->Pdo);
        break;
    case IOCTL_XENCONS_GET_NAME:
        Value = Frontend->Name;
        break;
    case IOCTL_XENCONS_GET_PROTOCOL:
        Value = Frontend->Protocol;
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
FrontendAbiAcquire(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;
    KIRQL                               Irql;
    NTSTATUS                            status;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    if (Frontend->References++ != 0)
        goto done;

    status = XENBUS_SUSPEND(Acquire, &Frontend->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FrontendResume(Frontend);

    status = XENBUS_SUSPEND(Register,
                            &Frontend->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FrontendSuspendCallback,
                            Frontend,
                            &Frontend->SuspendCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

done:
    KeReleaseSpinLock(&Frontend->Lock, Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                                Executive,
                                KernelMode,
                                FALSE,
                                NULL);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    --Frontend->References;
    ASSERT3U(Frontend->References, ==, 0);
    KeReleaseSpinLock(&Frontend->Lock, Irql);

    return status;
}

static VOID
FrontendAbiRelease(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;
    KIRQL                               Irql;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    if (--Frontend->References != 0)
        goto done;

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallback);
    Frontend->SuspendCallback = NULL;

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

done:
    KeReleaseSpinLock(&Frontend->Lock, Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                                Executive,
                                KernelMode,
                                FALSE,
                                NULL);
}

static NTSTATUS
FrontendAbiD3ToD0(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;

    UNREFERENCED_PARAMETER(Frontend);
    //return FrontendSetState(Frontend, FRONTEND_ENABLED);

    return STATUS_SUCCESS;
}

static VOID
FrontendAbiD0ToD3(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;

    UNREFERENCED_PARAMETER(Frontend);
    //(VOID) FrontendSetState(Frontend, FRONTEND_CLOSED);
}

static NTSTATUS
FrontendAbiOpen(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context,
    _In_ PFILE_OBJECT                   FileObject
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;

    return RingOpen(Frontend->Ring, FileObject);
}

static NTSTATUS
FrontendAbiClose(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context,
    _In_ PFILE_OBJECT                   FileObject
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;

    return RingClose(Frontend->Ring, FileObject);
}

static NTSTATUS
FrontendAbiPutQueue(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context,
    _In_ PIRP                           Irp
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;
    PIO_STACK_LOCATION                  StackLocation;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
        return RingPutQueue(Frontend->Ring, Irp);

    case IRP_MJ_DEVICE_CONTROL:
        return FrontendGetProperty(Frontend, Irp);

    default:
        ASSERT(FALSE);
        return STATUS_NOT_SUPPORTED;
    }
}

static XENCONS_CONSOLE_ABI FrontendAbi = {
    NULL,
    FrontendAbiAcquire,
    FrontendAbiRelease,
    FrontendAbiD3ToD0,
    FrontendAbiD0ToD3,
    FrontendAbiOpen,
    FrontendAbiClose,
    FrontendAbiPutQueue
};

NTSTATUS
FrontendCreate(
    _In_ PXENCONS_PDO                   Pdo,
    _Out_ PXENCONS_CONSOLE_ABI_CONTEXT  *Context
    )
{
    PSTR                                Name;
    ULONG                               Length;
    PSTR                                Path;
    PXENCONS_FRONTEND                   Frontend;
    NTSTATUS                            status;

    Trace("====>\n");

    Name = PdoGetName(Pdo);

    Length = sizeof("devices/console/") + (ULONG)strlen(Name);

    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path,
                                Length,
                                "device/console/%s",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    Frontend = __FrontendAllocate(sizeof(XENCONS_FRONTEND));

    status = STATUS_NO_MEMORY;
    if (Frontend == NULL)
        goto fail3;

    Frontend->Pdo = Pdo;
    Frontend->Path = Path;
    Frontend->BackendDomain = DOMID_INVALID;

    KeInitializeSpinLock(&Frontend->Lock);

    FdoGetDebugInterface(PdoGetFdo(Pdo), &Frontend->DebugInterface);
    FdoGetSuspendInterface(PdoGetFdo(Pdo), &Frontend->SuspendInterface);
    FdoGetStoreInterface(PdoGetFdo(Pdo), &Frontend->StoreInterface);

    status = RingCreate(Frontend, &Frontend->Ring);
    if (!NT_SUCCESS(status))
        goto fail4;

    KeInitializeEvent(&Frontend->EjectEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FrontendEject, Frontend, &Frontend->EjectThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    *Context = (PVOID)Frontend;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    RtlZeroMemory(&Frontend->EjectEvent, sizeof(KEVENT));

    RingDestroy(Frontend->Ring);
    Frontend->Ring = NULL;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&Frontend->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Frontend->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Frontend->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    Frontend->Online = FALSE;

    RtlZeroMemory(&Frontend->Lock, sizeof(KSPIN_LOCK));

    Frontend->BackendDomain = 0;
    Frontend->Path = NULL;
    Frontend->Pdo = NULL;

    ASSERT(IsZeroMemory(Frontend, sizeof(XENCONS_FRONTEND)));

    __FrontendFree(Frontend);

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __FrontendFree(Path);
fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FrontendGetAbi(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context,
    _Out_ PXENCONS_CONSOLE_ABI          Abi
    )
{
    *Abi = FrontendAbi;

    Abi->Context = Context;
}

VOID
FrontendDestroy(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    )
{
    PXENCONS_FRONTEND                   Frontend = (PXENCONS_FRONTEND)Context;

    Trace("=====>\n");

    ASSERT3U(KeGetCurrentIrql(), == , PASSIVE_LEVEL);

    ASSERT(Frontend->State == FRONTEND_UNKNOWN);

    ThreadAlert(Frontend->EjectThread);
    ThreadJoin(Frontend->EjectThread);
    Frontend->EjectThread = NULL;

    RtlZeroMemory(&Frontend->EjectEvent, sizeof(KEVENT));

    RingDestroy(Frontend->Ring);
    Frontend->Ring = NULL;

    RtlZeroMemory(&Frontend->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Frontend->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Frontend->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    Frontend->Online = FALSE;

    RtlZeroMemory(&Frontend->Lock, sizeof(KSPIN_LOCK));

    Frontend->BackendDomain = 0;

    __FrontendFree(Frontend->Path);
    Frontend->Path = NULL;

    Frontend->Pdo = NULL;

    ASSERT(IsZeroMemory(Frontend, sizeof(XENCONS_FRONTEND)));
    __FrontendFree(Frontend);

    Trace("<=====\n");
}
