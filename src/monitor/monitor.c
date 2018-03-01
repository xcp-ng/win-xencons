/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source 1and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the23
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

#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <setupapi.h>
#include <malloc.h>
#include <assert.h>

#include <xencons_device.h>
#include <version.h>

#include "messages.h"

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HANDLE                  EventLog;
    HANDLE                  StopEvent;
    HKEY                    ParametersKey;
    HDEVNOTIFY              InterfaceNotification;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

typedef struct _MONITOR_CONSOLE {
    LIST_ENTRY              ListEntry;
    PWCHAR                  DevicePath;
    HANDLE                  DeviceHandle;
    HDEVNOTIFY              DeviceNotification;
    PCHAR                   DeviceName; // protocol and instance?
    HANDLE                  ExecutableThread;
    HANDLE                  ExecutableEvent;
    HANDLE                  DeviceThread;
    HANDLE                  DeviceEvent;
    HANDLE                  ServerThread;
    HANDLE                  ServerEvent;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
} MONITOR_CONSOLE, *PMONITOR_CONSOLE;

typedef struct _MONITOR_CONNECTION {
    PMONITOR_CONSOLE        Console;
    LIST_ENTRY              ListEntry;
    HANDLE                  Pipe;
    HANDLE                  Thread;
} MONITOR_CONNECTION, *PMONITOR_CONNECTION;

static MONITOR_CONTEXT MonitorContext;

#define PIPE_BASE_NAME "\\\\.\\pipe\\xencons\\"

#define MAXIMUM_BUFFER_SIZE 1024

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## _Service

#define PARAMETERS_KEY(_Service) \
        SERVICE_KEY(_Service) ## "\\Parameters"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const CHAR      *Format,
    IN  ...
    )
{
#if DBG
    PMONITOR_CONTEXT    Context = &MonitorContext;
    const CHAR          *Strings[1];
#endif
    CHAR                Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintfA(Buffer,
                              MAXIMUM_BUFFER_SIZE,
                              Format,
                              Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLengthA(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);

#if DBG
    Strings[0] = Buffer;

    if (Context->EventLog != NULL)
        ReportEventA(Context->EventLog,
                    EVENTLOG_INFORMATION_TYPE,
                    0,
                    MONITOR_LOG,
                    NULL,
                    ARRAYSIZE(Strings),
                    0,
                    Strings,
                    NULL);
#endif
}

#define Log(_Format, ...) \
    __Log(__MODULE__ "|" __FUNCTION__ ": " _Format, __VA_ARGS__)

static PCHAR
GetErrorMessage(
    IN  HRESULT Error
    )
{
    PCHAR       Message;
    ULONG       Index;

    if (!FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       Error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPSTR)&Message,
                       0,
                       NULL))
        return NULL;

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static const CHAR *
ServiceStateName(
    IN  DWORD   State
    )
{
#define _STATE_NAME(_State) \
    case SERVICE_ ## _State: \
        return #_State

    switch (State) {
    _STATE_NAME(START_PENDING);
    _STATE_NAME(RUNNING);
    _STATE_NAME(STOP_PENDING);
    _STATE_NAME(STOPPED);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _STATE_NAME
}

static VOID
ReportStatus(
    IN  DWORD           CurrentState,
    IN  DWORD           Win32ExitCode,
    IN  DWORD           WaitHint
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    static DWORD        CheckPoint = 1;
    BOOL                Success;
    HRESULT             Error;

    Log("====> (%s)", ServiceStateName(CurrentState));

    Context->Status.dwCurrentState = CurrentState;
    Context->Status.dwWin32ExitCode = Win32ExitCode;
    Context->Status.dwWaitHint = WaitHint;

    if (CurrentState == SERVICE_START_PENDING)
        Context->Status.dwControlsAccepted = 0;
    else
        Context->Status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             SERVICE_ACCEPT_SESSIONCHANGE;

    if (CurrentState == SERVICE_RUNNING ||
        CurrentState == SERVICE_STOPPED )
        Context->Status.dwCheckPoint = 0;
    else
        Context->Status.dwCheckPoint = CheckPoint++;

    Success = SetServiceStatus(Context->Service, &Context->Status);

    if (!Success)
        goto fail1;

    Log("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static FORCEINLINE VOID
__InitializeListHead(
    IN  PLIST_ENTRY ListEntry
    )
{
    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static FORCEINLINE VOID
__InsertTailList(
    IN  PLIST_ENTRY ListHead,
    IN  PLIST_ENTRY ListEntry
    )
{
    ListEntry->Blink = ListHead->Blink;
    ListEntry->Flink = ListHead;
    ListHead->Blink->Flink = ListEntry;
    ListHead->Blink = ListEntry;
}

static FORCEINLINE VOID
__RemoveEntryList(
    IN  PLIST_ENTRY ListEntry
    )
{
    PLIST_ENTRY     Flink;
    PLIST_ENTRY     Blink;

    Flink = ListEntry->Flink;
    Blink = ListEntry->Blink;
    Flink->Blink = Blink;
    Blink->Flink = Flink;

    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static VOID
PutString(
    IN  HANDLE      Handle,
    IN  PUCHAR      Buffer,
    IN  DWORD       Length
    )
{
    DWORD           Offset;

    Offset = 0;
    while (Offset < Length) {
        DWORD   Written;
        BOOL    Success;

        Success = WriteFile(Handle,
                            &Buffer[Offset],
                            Length - Offset,
                            &Written,
                            NULL);
        if (!Success)
            break;

        Offset += Written;
    }
}

#define ECHO(_Handle, _Buffer) \
    PutString((_Handle), (PUCHAR)_Buffer, (DWORD)strlen((_Buffer)) * sizeof(CHAR))

DWORD WINAPI
ConnectionThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONNECTION Connection = (PMONITOR_CONNECTION)Argument;
    PMONITOR_CONSOLE    Console = Connection->Console;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    OVERLAPPED          Overlapped;
    HANDLE              Handle[2];
    DWORD               Length;
    DWORD               Object;
    HRESULT             Error;

    Log("====> %s", Console->DeviceName);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handle[0] = Console->ServerEvent;
    Handle[1] = Overlapped.hEvent;

    EnterCriticalSection(&Console->CriticalSection);
    __InsertTailList(&Console->ListHead, &Connection->ListEntry);
    ++Console->ListCount;
    LeaveCriticalSection(&Console->CriticalSection);

    for (;;) {
        (VOID) ReadFile(Connection->Pipe,
                        Buffer,
                        sizeof(Buffer),
                        NULL,
                        &Overlapped);

        Object = WaitForMultipleObjects(ARRAYSIZE(Handle),
                                        Handle,
                                        FALSE,
                                        INFINITE);
        if (Object == WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(Connection->Pipe,
                                 &Overlapped,
                                 &Length,
                                 FALSE))
            break;

        ResetEvent(Overlapped.hEvent);

        PutString(Console->DeviceHandle,
                  Buffer,
                  Length);
    }

    EnterCriticalSection(&Console->CriticalSection);
    __RemoveEntryList(&Connection->ListEntry);
    --Console->ListCount;
    LeaveCriticalSection(&Console->CriticalSection);

    CloseHandle(Overlapped.hEvent);

    FlushFileBuffers(Connection->Pipe);
    DisconnectNamedPipe(Connection->Pipe);
    CloseHandle(Connection->Pipe);
    CloseHandle(Connection->Thread);
    free(Connection);

    Log("<==== %s", Console->DeviceName);

    return 0;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
ServerThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONSOLE    Console = (PMONITOR_CONSOLE)Argument;
    CHAR                PipeName[MAXIMUM_BUFFER_SIZE];
    OVERLAPPED          Overlapped;
    HANDLE              Handle[2];
    HANDLE              Pipe;
    DWORD               Object;
    PMONITOR_CONNECTION Connection;
    HRESULT             Error;

    Log("====> %s", Console->DeviceName);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handle[0] = Console->ServerEvent;
    Handle[1] = Overlapped.hEvent;

    Error = StringCchPrintfA(PipeName,
                             MAXIMUM_BUFFER_SIZE,
                             "%s%s",
                             PIPE_BASE_NAME,
                             Console->DeviceName);
    if (Error != S_OK && Error != STRSAFE_E_INSUFFICIENT_BUFFER)
        goto fail2;

    Log("%s", PipeName);

    for (;;) {
        Pipe = CreateNamedPipe(PipeName,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                               PIPE_UNLIMITED_INSTANCES,
                               MAXIMUM_BUFFER_SIZE,
                               MAXIMUM_BUFFER_SIZE,
                               0,
                               NULL);
        if (Pipe == INVALID_HANDLE_VALUE)
            goto fail3;

        (VOID) ConnectNamedPipe(Pipe,
                                &Overlapped);

        Object = WaitForMultipleObjects(ARRAYSIZE(Handle),
                                        Handle,
                                        FALSE,
                                        INFINITE);
        if (Object == WAIT_OBJECT_0) {
            CloseHandle(Pipe);
            break;
        }

        ResetEvent(Overlapped.hEvent);

        Connection = (PMONITOR_CONNECTION)malloc(sizeof(MONITOR_CONNECTION));
        if (Connection == NULL)
            goto fail4;

        __InitializeListHead(&Connection->ListEntry);
        Connection->Console = Console;
        Connection->Pipe = Pipe;
        Connection->Thread = CreateThread(NULL,
                                          0,
                                          ConnectionThread,
                                          Connection,
                                          0,
                                          NULL);
        if (Connection->Thread == NULL)
            goto fail5;
    }

    CloseHandle(Overlapped.hEvent);

    Log("<==== %s", Console->DeviceName);

    return 0;

fail5:
    Log("fail5");

    free(Connection);

fail4:
    Log("fail4");

    CloseHandle(Pipe);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
DeviceThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONSOLE    Console = (PMONITOR_CONSOLE)Argument;
    OVERLAPPED          Overlapped;
    HANDLE              Device;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    DWORD               Length;
    DWORD               Wait;
    HANDLE              Handles[2];
    DWORD               Error;

    Log("====> %s", Console->DeviceName);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Console->DeviceEvent;
    Handles[1] = Overlapped.hEvent;

    Device = CreateFileW(Console->DevicePath,
                         GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    if (Device == INVALID_HANDLE_VALUE)
        goto fail2;

    for (;;) {
        PLIST_ENTRY     ListEntry;

        (VOID) ReadFile(Device,
                        Buffer,
                        sizeof(Buffer),
                        NULL,
                        &Overlapped);

        Wait = WaitForMultipleObjects(ARRAYSIZE(Handles),
                                      Handles,
                                      FALSE,
                                      INFINITE);
        if (Wait == WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(Device,
                                 &Overlapped,
                                 &Length,
                                 FALSE))
            break;

        ResetEvent(Overlapped.hEvent);

        EnterCriticalSection(&Console->CriticalSection);

        for (ListEntry = Console->ListHead.Flink;
                ListEntry != &Console->ListHead;
                ListEntry = ListEntry->Flink) {
            PMONITOR_CONNECTION Connection;

            Connection = CONTAINING_RECORD(ListEntry,
                                           MONITOR_CONNECTION,
                                           ListEntry);

            PutString(Connection->Pipe,
                      Buffer,
                      Length);
        }

        LeaveCriticalSection(&Console->CriticalSection);
    }

    CloseHandle(Device);

    CloseHandle(Overlapped.hEvent);

    Log("<==== %s", Console->DeviceName);

    return 0;

fail2:
    Log("fail2\n");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

static BOOL
GetExecutable(
    IN  PCHAR           DeviceName,
    OUT PCHAR           *Executable
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HKEY                Key;
    DWORD               MaxValueLength;
    DWORD               ExecutableLength;
    DWORD               Type;
    HRESULT             Error;

    Error = RegOpenKeyExA(Context->ParametersKey,
                          DeviceName,
                          0,
                          KEY_READ,
                          &Key);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegQueryInfoKey(Key,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    ExecutableLength = MaxValueLength;

    *Executable = calloc(1, ExecutableLength);
    if (Executable == NULL)
        goto fail3;

    Error = RegQueryValueExA(Key,
                             "Executable",
                             NULL,
                             &Type,
                             (LPBYTE)(*Executable),
                             &ExecutableLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail5;
    }

    Log("%s = %s", DeviceName, *Executable);

    RegCloseKey(Key);

    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(*Executable);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(Key);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

DWORD WINAPI
ExecutableThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONSOLE    Console = (PMONITOR_CONSOLE)Argument;
    PCHAR               Executable;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFO         StartupInfo;
    BOOL                Success;
    HANDLE              Handle[2];
    DWORD               Object;
    HRESULT             Error;

    Log("====> %s", Console->DeviceName);

    // If there is no executable, this thread can finish now.
    if (!GetExecutable(Console->DeviceName,
                       &Executable))
        goto done;
    if (Executable == NULL)
        goto done;

again:
    ZeroMemory(&ProcessInfo, sizeof (ProcessInfo));
    ZeroMemory(&StartupInfo, sizeof (StartupInfo));
    StartupInfo.cb = sizeof (StartupInfo);

    Log("Executing: %s", Executable);

#pragma warning(suppress:6053) // CommandLine might not be NUL-terminated
    Success = CreateProcess(NULL,
                            Executable,
                            NULL,
                            NULL,
                            FALSE,
                            CREATE_NO_WINDOW |
                            CREATE_NEW_PROCESS_GROUP,
                            NULL,
                            NULL,
                            &StartupInfo,
                            &ProcessInfo);
    if (!Success)
        goto fail1;

    Handle[0] = Console->ExecutableEvent;
    Handle[1] = ProcessInfo.hProcess;

    Object = WaitForMultipleObjects(ARRAYSIZE(Handle),
                                    Handle,
                                    FALSE,
                                    INFINITE);

#define WAIT_OBJECT_1 (WAIT_OBJECT_0 + 1)

    switch (Object) {
    case WAIT_OBJECT_0:
        ResetEvent(Console->ExecutableEvent);

        TerminateProcess(ProcessInfo.hProcess, 1);
        CloseHandle(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
        break;

    case WAIT_OBJECT_1:
        CloseHandle(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
        goto again;

    default:
        break;
    }

//#undef WAIT_OBJECT_1

    free(Executable);

done:
    Log("<==== %s", Console->DeviceName);

    return 0;

fail1:
    Error = GetLastError();

    free(Executable);

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

static PMONITOR_CONSOLE
ConsoleCreate(
    IN  PWCHAR              DevicePath
    )
{
    PMONITOR_CONTEXT        Context = &MonitorContext;
    PMONITOR_CONSOLE        Console;
    DEV_BROADCAST_HANDLE    Handle;
    CHAR                    DeviceName[MAX_PATH];
    DWORD                   Bytes;
    BOOL                    Success;
    HRESULT                 Error;

    Log("====> %ws", DevicePath);

    Console = malloc(sizeof(MONITOR_CONSOLE));
    if (Console == NULL)
        goto fail1;

    memset(Console, 0, sizeof(MONITOR_CONSOLE));
    __InitializeListHead(&Console->ListHead);
    __InitializeListHead(&Console->ListEntry);
    InitializeCriticalSection(&Console->CriticalSection);

    Console->DevicePath = _wcsdup(DevicePath);
    if (Console->DevicePath == NULL)
        goto fail2;

    Console->DeviceHandle = CreateFileW(DevicePath,
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        NULL,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        NULL);
    if (Console->DeviceHandle == INVALID_HANDLE_VALUE)
        goto fail3;

    Success = DeviceIoControl(Console->DeviceHandle,
                              IOCTL_XENCONS_GET_NAME,
                              NULL,
                              0,
                              DeviceName,
                              sizeof(DeviceName),
                              &Bytes,
                              NULL);
    if (!Success)
        goto fail4;

    DeviceName[MAX_PATH - 1] = '\0';

    Console->DeviceName = _strdup(DeviceName);
    if (Console->DeviceName == NULL)
        goto fail5;

    ECHO(Console->DeviceHandle, "\r\n[ATTACHED]\r\n");

    ZeroMemory(&Handle, sizeof (Handle));
    Handle.dbch_size = sizeof (Handle);
    Handle.dbch_devicetype = DBT_DEVTYP_HANDLE;
    Handle.dbch_handle = Console->DeviceHandle;

    Console->DeviceNotification =
        RegisterDeviceNotification(Context->Service,
                                    &Handle,
                                    DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Console->DeviceNotification == NULL)
        goto fail6;

    Console->DeviceEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);
    if (Console->DeviceEvent == NULL)
        goto fail7;

    Console->DeviceThread = CreateThread(NULL,
                                         0,
                                         DeviceThread,
                                         Console,
                                         0,
                                         NULL);
    if (Console->DeviceThread == NULL)
        goto fail8;

    Console->ServerEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);
    if (Console->ServerEvent == NULL)
        goto fail9;

    Console->ServerThread = CreateThread(NULL,
                                         0,
                                         ServerThread,
                                         Console,
                                         0,
                                         NULL);
    if (Console->ServerThread == NULL)
        goto fail10;

    Console->ExecutableEvent = CreateEvent(NULL,
                                           TRUE,
                                           FALSE,
                                           NULL);
    if (Console->ExecutableEvent == NULL)
        goto fail11;

    Console->ExecutableThread = CreateThread(NULL,
                                             0,
                                             ExecutableThread,
                                             Console,
                                             0,
                                             NULL);
    if (Console->ExecutableThread == NULL)
        goto fail12;

    Log("<==== %s", Console->DeviceName);

    return Console;

fail12:
    Log("fail12");

    CloseHandle(Console->ExecutableEvent);
    Console->ExecutableEvent = NULL;

fail11:
    Log("fail11");

    SetEvent(Console->ServerEvent);
    WaitForSingleObject(Console->ServerThread, INFINITE);

fail10:
    Log("fail10");

    CloseHandle(Console->ServerEvent);
    Console->ServerEvent = NULL;

fail9:
    Log("fail9");

    SetEvent(Console->DeviceEvent);
    WaitForSingleObject(Console->DeviceThread, INFINITE);

fail8:
    Log("fail8");

    CloseHandle(Console->DeviceEvent);
    Console->DeviceEvent = NULL;

fail7:
    Log("fail7");

    UnregisterDeviceNotification(Console->DeviceNotification);
    Console->DeviceNotification = NULL;

fail6:
    Log("fail6");

    ECHO(Console->DeviceHandle, "\r\n[DETACHED]\r\n");

    free(Console->DevicePath);
    Console->DevicePath = NULL;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    CloseHandle(Console->DeviceHandle);
    Console->DeviceHandle = INVALID_HANDLE_VALUE;

fail3:
    Log("fail3");

    free(Console->DevicePath);
    Console->DevicePath = NULL;

fail2:
    Log("fail2");

    DeleteCriticalSection(&Console->CriticalSection);
    ZeroMemory(&Console->ListHead, sizeof(LIST_ENTRY));
    ZeroMemory(&Console->ListEntry, sizeof(LIST_ENTRY));

    free(Console);

fail1:
    Error = GetLastError();

    {
        PCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return NULL;
}

static FORCEINLINE VOID
ConsoleWaitForPipes(
    IN  PMONITOR_CONSOLE    Console
    )
{
    PLIST_ENTRY             ListEntry;
    HANDLE                  *Events;
    DWORD                   Count;
    DWORD                   Index;

    EnterCriticalSection(&Console->CriticalSection);

    Count = Console->ListCount + 1;
    Events = malloc(Count * sizeof(HANDLE));
    if (Events == NULL)
        goto fail1;

    Index = 0;
    for (ListEntry = Console->ListHead.Flink;
         ListEntry != &Console->ListHead;
         ListEntry = ListEntry->Flink) {
        PMONITOR_CONNECTION Connection;

        Connection = CONTAINING_RECORD(ListEntry,
                                       MONITOR_CONNECTION,
                                       ListEntry);

#pragma warning(suppress: 6386) // Buffer overflow
        Events[Index] = Connection->Thread;
        ++Index;
    }
    Events[Count - 1] = Console->ServerThread;

    LeaveCriticalSection(&Console->CriticalSection);

    SetEvent(Console->ServerEvent);
    WaitForMultipleObjects(Count, Events, TRUE, INFINITE);

    return;

fail1:
    LeaveCriticalSection(&Console->CriticalSection);

    // set the event and wait for the server thread anyway
    SetEvent(Console->ServerEvent);
    WaitForSingleObject(Console->ServerThread, INFINITE);
}

static VOID
ConsoleDestroy(
    IN  PMONITOR_CONSOLE    Console
    )
{
    Log("====> %s", Console->DeviceName);

    SetEvent(Console->ExecutableEvent);
    WaitForSingleObject(Console->ExecutableThread, INFINITE);

    CloseHandle(Console->ExecutableEvent);
    Console->ExecutableEvent = NULL;

    ConsoleWaitForPipes(Console);

    CloseHandle(Console->ServerEvent);
    Console->ServerEvent = NULL;

    SetEvent(Console->DeviceEvent);
    WaitForSingleObject(Console->DeviceThread, INFINITE);

    CloseHandle(Console->DeviceEvent);
    Console->DeviceEvent = NULL;

    UnregisterDeviceNotification(Console->DeviceNotification);
    Console->DeviceNotification = NULL;

    ECHO(Console->DeviceHandle, "\r\n[DETACHED]\r\n");

    free(Console->DevicePath);
    Console->DevicePath = NULL;

    CloseHandle(Console->DeviceHandle);
    Console->DeviceHandle = INVALID_HANDLE_VALUE;

    free(Console->DevicePath);
    Console->DevicePath = NULL;

    DeleteCriticalSection(&Console->CriticalSection);
    ZeroMemory(&Console->ListHead, sizeof(LIST_ENTRY));
    ZeroMemory(&Console->ListEntry, sizeof(LIST_ENTRY));

    free(Console);

    Log("<====");
}

static BOOL
MonitorAdd(
    IN  PWCHAR          DevicePath
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_CONSOLE    Console;

    Log("=====> %ws", DevicePath);

    Console = ConsoleCreate(DevicePath);
    if (Console == NULL)
        goto fail1;

    EnterCriticalSection(&Context->CriticalSection);
    __InsertTailList(&Context->ListHead, &Console->ListEntry);
    ++Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    Log("<===== %s", Console->DeviceName);

    return TRUE;

fail1:
    Log("fail1");

    return FALSE;
}

static BOOL
MonitorRemove(
    IN  HANDLE          DeviceHandle
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_CONSOLE    Console;
    PLIST_ENTRY         ListEntry;

    Log("=====> 0x%p", DeviceHandle);

    EnterCriticalSection(&Context->CriticalSection);
    for (ListEntry = Context->ListHead.Flink;
         ListEntry != &Context->ListHead;
         ListEntry = ListEntry->Flink) {
        Console = CONTAINING_RECORD(ListEntry,
                                    MONITOR_CONSOLE,
                                    ListEntry);

        if (Console->DeviceHandle == DeviceHandle)
            goto found;
    }
    LeaveCriticalSection(&Context->CriticalSection);

    Log("DeviceHandle 0x%p not found", DeviceHandle);

    return FALSE;

found:
    __RemoveEntryList(&Console->ListEntry);
    --Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    ConsoleDestroy(Console);

    Log("<=====");

    return TRUE;
}

static BOOL
MonitorEnumerate(
    VOID
    )
{
    PMONITOR_CONTEXT                    Context = &MonitorContext;
    HDEVINFO                            DeviceInfoSet;
    SP_DEVICE_INTERFACE_DATA            DeviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W  DeviceInterfaceDetail;
    PMONITOR_CONSOLE                    Console;
    DWORD                               Size;
    DWORD                               Index;
    HRESULT                             Error;
    BOOL                                Success;

    Log("====>");

    DeviceInfoSet = SetupDiGetClassDevs(&GUID_XENCONS_DEVICE,
                                        NULL,
                                        NULL,
                                        DIGCF_PRESENT |
                                        DIGCF_DEVICEINTERFACE);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        goto fail1;

    DeviceInterfaceData.cbSize = sizeof (SP_DEVICE_INTERFACE_DATA);

    for (Index = 0; TRUE; ++Index) {
        Success = SetupDiEnumDeviceInterfaces(DeviceInfoSet,
                                              NULL,
                                              &GUID_XENCONS_DEVICE,
                                              Index,
                                              &DeviceInterfaceData);
        if (!Success)
            break;

        Success = SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet,
                                                  &DeviceInterfaceData,
                                                  NULL,
                                                  0,
                                                  &Size,
                                                  NULL);
        if (!Success && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail2;

        DeviceInterfaceDetail = calloc(1, Size);
        if (DeviceInterfaceDetail == NULL)
            goto fail3;

        DeviceInterfaceDetail->cbSize =
            sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        Success = SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet,
                                                   &DeviceInterfaceData,
                                                   DeviceInterfaceDetail,
                                                   Size,
                                                   NULL,
                                                   NULL);
        if (!Success)
            goto fail4;

        Console = ConsoleCreate(DeviceInterfaceDetail->DevicePath);
        if (Console == NULL)
            goto fail5;

        EnterCriticalSection(&Context->CriticalSection);
        __InsertTailList(&Context->ListHead, &Console->ListEntry);
        ++Context->ListCount;
        LeaveCriticalSection(&Context->CriticalSection);

        free(DeviceInterfaceDetail);

        continue;

    fail5:
        Log("fail5");
    fail4:
        Log("fail4");

        free(DeviceInterfaceDetail);

    fail3:
        Log("fail3");
    fail2:
        Error = GetLastError();

        {
            PCHAR  Message;
            Message = GetErrorMessage(Error);
            Log("fail2 (%s)", Message);
            LocalFree(Message);
        }
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    Log("<====");

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static VOID
MonitorRemoveAll(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_CONSOLE    Console;

    Log("=====>");

    for (;;) {
        EnterCriticalSection(&Context->CriticalSection);
        if (Context->ListHead.Flink == &Context->ListHead)
            break;

        Console = CONTAINING_RECORD(Context->ListHead.Flink,
                                    MONITOR_CONSOLE,
                                    ListEntry);

        __RemoveEntryList(&Console->ListEntry);
        --Context->ListCount;

        LeaveCriticalSection(&Context->CriticalSection);

        ConsoleDestroy(Console);
    }
    LeaveCriticalSection(&Context->CriticalSection);

    Log("<=====");
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    IN  DWORD           Ctrl,
    IN  DWORD           EventType,
    IN  LPVOID          EventData,
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;

    UNREFERENCED_PARAMETER(Argument);

    switch (Ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        SetEvent(Context->StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
        return NO_ERROR;

    case SERVICE_CONTROL_DEVICEEVENT: {
        PDEV_BROADCAST_HDR  Header = EventData;

        switch (EventType) {
        case DBT_DEVICEARRIVAL:
            if (Header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE_W Interface = EventData;

                if (IsEqualGUID(&Interface->dbcc_classguid,
                                &GUID_XENCONS_DEVICE))
                    MonitorAdd(Interface->dbcc_name);
            }
            break;

        case DBT_DEVICEQUERYREMOVE:
        case DBT_DEVICEREMOVEPENDING:
        case DBT_DEVICEREMOVECOMPLETE:
            if (Header->dbch_devicetype == DBT_DEVTYP_HANDLE) {
                PDEV_BROADCAST_HANDLE Device = EventData;

                MonitorRemove(Device->dbch_handle);
            }
            break;
        }

        return NO_ERROR;
    }
    default:
        break;
    }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

VOID WINAPI
MonitorMain(
    _In_    DWORD                   argc,
    _In_    LPTSTR                  *argv
    )
{
    PMONITOR_CONTEXT                Context = &MonitorContext;
    DEV_BROADCAST_DEVICEINTERFACE   Interface;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Log("====>");

    Error = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                         PARAMETERS_KEY(__MODULE__),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Context->Service = RegisterServiceCtrlHandlerExA(MONITOR_NAME,
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail2;

    Context->EventLog = RegisterEventSourceA(NULL,
                                            MONITOR_NAME);
    if (Context->EventLog == NULL)
        goto fail3;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail4;

    ZeroMemory(&Interface, sizeof (Interface));
    Interface.dbcc_size = sizeof (Interface);
    Interface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    Interface.dbcc_classguid = GUID_XENCONS_DEVICE;

    Context->InterfaceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Interface,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Context->InterfaceNotification == NULL)
        goto fail5;

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    __InitializeListHead(&Context->ListHead);
    InitializeCriticalSection(&Context->CriticalSection);

    MonitorEnumerate();

    Log("Waiting...");
    WaitForSingleObject(Context->StopEvent, INFINITE);
    Log("Wait Complete");

    MonitorRemoveAll();

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    UnregisterDeviceNotification(Context->InterfaceNotification);

    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    (VOID) DeregisterEventSource(Context->EventLog);

    CloseHandle(Context->ParametersKey);

    Log("<====");

    return;

fail5:
    Log("fail5");

    CloseHandle(Context->StopEvent);

fail4:
    Log("fail4");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

    (VOID) DeregisterEventSource(Context->EventLog);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    CloseHandle(Context->ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
MonitorCreate(
    VOID
    )
{
    SC_HANDLE   SCManager;
    SC_HANDLE   Service;
    CHAR        Path[MAX_PATH];
    HRESULT     Error;

    Log("====>");

    if(!GetModuleFileNameA(NULL, Path, MAX_PATH))
        goto fail1;

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail2;

    Service = CreateService(SCManager,
                            MONITOR_NAME,
                            MONITOR_DISPLAYNAME,
                            SERVICE_ALL_ACCESS,
                            SERVICE_WIN32_OWN_PROCESS,
                            SERVICE_AUTO_START,
                            SERVICE_ERROR_NORMAL,
                            Path,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);

    if (Service == NULL)
        goto fail3;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail3:
    Log("fail3");

    CloseServiceHandle(SCManager);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorDelete(
    VOID
    )
{
    SC_HANDLE           SCManager;
    SC_HANDLE           Service;
    BOOL                Success;
    SERVICE_STATUS      Status;
    HRESULT             Error;

    Log("====>");

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail1;

    Service = OpenService(SCManager,
                          MONITOR_NAME,
                          SERVICE_ALL_ACCESS);

    if (Service == NULL)
        goto fail2;

    Success = ControlService(Service,
                             SERVICE_CONTROL_STOP,
                             &Status);

    if (!Success)
        goto fail3;

    Success = DeleteService(Service);

    if (!Success)
        goto fail4;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    CloseServiceHandle(Service);

fail2:
    Log("fail2");

    CloseServiceHandle(SCManager);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorEntry(
    VOID
    )
{
    SERVICE_TABLE_ENTRY Table[] = {
        { MONITOR_NAME, MonitorMain },
        { NULL, NULL }
    };
    HRESULT             Error;

    Log("%s (%s) ====>",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (!StartServiceCtrlDispatcher(Table))
        goto fail1;

    Log("%s (%s) <====",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

int CALLBACK
WinMain(
    _In_        HINSTANCE   Current,
    _In_opt_    HINSTANCE   Previous,
    _In_        LPSTR       CmdLine,
    _In_        int         CmdShow
    )
{
    BOOL                    Success;

    UNREFERENCED_PARAMETER(Current);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdShow);

    if (strlen(CmdLine) != 0) {
         if (_stricmp(CmdLine, "create") == 0)
             Success = MonitorCreate();
         else if (_stricmp(CmdLine, "delete") == 0)
             Success = MonitorDelete();
         else
             Success = FALSE;
    } else
        Success = MonitorEntry();

    return Success ? 0 : 1;
}
