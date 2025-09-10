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

#ifndef _XENCONS_FRONTEND_H
#define _XENCONS_FRONTEND_H

#include <ntddk.h>

#include "driver.h"
#include "console_abi.h"

typedef struct _XENCONS_FRONTEND XENCONS_FRONTEND, *PXENCONS_FRONTEND;

extern NTSTATUS
FrontendCreate(
    _In_ PXENCONS_PDO                   Pdo,
    _Out_ PXENCONS_CONSOLE_ABI_CONTEXT  *Context
    );

VOID
FrontendGetAbi(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context,
    _Out_ PXENCONS_CONSOLE_ABI          Abi
    );

VOID
FrontendDestroy(
    _In_ PXENCONS_CONSOLE_ABI_CONTEXT   Context
    );

extern PXENCONS_PDO
FrontendGetPdo(
    _In_ PXENCONS_FRONTEND  Frontend
    );

extern PCHAR
FrontendGetPath(
    _In_ PXENCONS_FRONTEND  Frontend
    );

extern PCHAR
FrontendGetBackendPath(
    _In_ PXENCONS_FRONTEND  Frontend
    );

extern USHORT
FrontendGetBackendDomain(
    _In_ PXENCONS_FRONTEND  Frontend
    );

#endif  // _XENCONS_FRONTEND_H
