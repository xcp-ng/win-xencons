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

#ifndef _XENCONS_CONSOLE_ABI_H
#define _XENCONS_CONSOLE_ABI_H

#include <ntddk.h>

#include "driver.h"

typedef PVOID *PXENCONS_CONSOLE_ABI_CONTEXT;

typedef NTSTATUS
(*XENCONS_CONSOLE_ABI_ACQUIRE)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    );

typedef VOID
(*XENCONS_CONSOLE_ABI_RELEASE)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    );

typedef NTSTATUS
(*XENCONS_CONSOLE_ABI_D3TOD0)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    );

typedef VOID
(*XENCONS_CONSOLE_ABI_D0TOD3)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context
    );

typedef NTSTATUS
(*XENCONS_CONSOLE_ABI_OPEN)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PFILE_OBJECT                    FileObject
    );

typedef NTSTATUS
(*XENCONS_CONSOLE_ABI_CLOSE)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PFILE_OBJECT                    FileObject
    );

typedef NTSTATUS
(*XENCONS_CONSOLE_ABI_PUT_QUEUE)(
    IN  PXENCONS_CONSOLE_ABI_CONTEXT    Context,
    IN  PIRP                            Irp
    );

typedef struct _XENCONS_CONSOLE_ABI {
    PXENCONS_CONSOLE_ABI_CONTEXT            Context;
    XENCONS_CONSOLE_ABI_ACQUIRE             ConsoleAbiAcquire;
    XENCONS_CONSOLE_ABI_RELEASE             ConsoleAbiRelease;
    XENCONS_CONSOLE_ABI_D3TOD0              ConsoleAbiD3ToD0;
    XENCONS_CONSOLE_ABI_D0TOD3              ConsoleAbiD0ToD3;
    XENCONS_CONSOLE_ABI_OPEN                ConsoleAbiOpen;
    XENCONS_CONSOLE_ABI_CLOSE               ConsoleAbiClose;
    XENCONS_CONSOLE_ABI_PUT_QUEUE           ConsoleAbiPutQueue;
} XENCONS_CONSOLE_ABI, *PXENCONS_CONSOLE_ABI;

#define XENCONS_CONSOLE_ABI(_Method, _Abi, ...)   \
    (_Abi)->ConsoleAbi ## _Method((_Abi)->Context, __VA_ARGS__)

#endif  // _XENCONS_CONSOLE_ABI_H
