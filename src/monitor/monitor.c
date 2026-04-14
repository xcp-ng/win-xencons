/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
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
#include <tchar.h>
#include <winioctl.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <setupapi.h>
#include <sddl.h>
#include <malloc.h>
#include <assert.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>

#include <xencons_device.h>
#include <version.h>

#include "messages.h"

#define stringify_literal(_text) #_text
#define stringify(_text) stringify_literal(_text)
#define __MODULE__ stringify(PROJECT)

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HANDLE                  StopEvent;
    HKEY                    ParametersKey;
    HDEVNOTIFY              InterfaceNotification;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

typedef struct _MONITOR_CONSOLE {
    LIST_ENTRY              ListEntry;
    PTSTR                   DevicePath;
    HANDLE                  DeviceHandle;
    HDEVNOTIFY              DeviceNotification;
    PTSTR                   DeviceName; // protocol and instance?
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

#define PIPE_BASE_NAME "\\\\.\\pipe\\ProtectedPrefix\\Administrators\\xencons\\"
// FILE_GENERIC_ALL for SYSTEM and Builtin\Administrators, nothing for the rest
#define PIPE_SDDL "D:(A;;FA;;;SY)(A;;FA;;;BA)"

#define MAXIMUM_BUFFER_SIZE 1024

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## _Service

#define PARAMETERS_KEY(_Service) \
        SERVICE_KEY(_Service) ## "\\Parameters"

TRACELOGGING_DEFINE_PROVIDER(MonitorTraceLoggingProvider,
                             MONITOR_NAME,
                             // {F1D4F89A-D4FC-5C76-865B-27532946CA0A}
                             (0xF1D4F89A, 0xD4FC, 0x5C76, 0x86, 0x5B, 0x27, 0x53, 0x29, 0x46, 0xCA, 0x0A));

typedef enum {
    LOG_INFO,
    LOG_ERROR
} LOG_LEVEL;

#ifdef UNICODE
#define TraceLoggingStringT(_buf, _name)    TraceLoggingWideString(_buf, _name)
#else
#define TraceLoggingStringT(_buf, _name)    TraceLoggingString(_buf, _name)
#endif

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    _In_ LOG_LEVEL      Level,
    _In_ PCTSTR         Format,
    ...
    )
{
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    _Analysis_assume_(Length < MAXIMUM_BUFFER_SIZE);
    _Analysis_assume_(Length >= 2);
    Buffer[Length] = _T('\0');
    Buffer[Length - 1] = _T('\n');
    Buffer[Length - 2] = _T('\r');

    OutputDebugString(Buffer);

    switch (Level) {
    case LOG_INFO:
        TraceLoggingWrite(MonitorTraceLoggingProvider,
                          "Information",
                          TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                          TraceLoggingStringT(Buffer, "Info"));
        break;
    case LOG_ERROR:
        TraceLoggingWrite(MonitorTraceLoggingProvider,
                          "Error",
                          TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                          TraceLoggingStringT(Buffer, "Error"));
        break;
    default:
        break;
    }
}

#define LogInfo(_Format, ...) \
        __Log(LOG_INFO, _T(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

#define LogError(_Format, ...) \
        __Log(LOG_ERROR, _T(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

static PTSTR
GetErrorMessage(
    _In_  HRESULT   Error
    )
{
    PTSTR           Message;
    ULONG           Index;

    if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       Error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPTSTR)&Message,
                       0,
                       NULL))
        return NULL;

    for (Index = 0; Message[Index] != _T('\0'); Index++) {
        if (Message[Index] == _T('\r') || Message[Index] == _T('\n')) {
            Message[Index] = _T('\0');
            break;
        }
    }

    return Message;
}

static PCTSTR
ServiceStateName(
    _In_ DWORD  State
    )
{
#define _STATE_NAME(_State) \
    case SERVICE_ ## _State: \
        return _T(#_State)

    switch (State) {
    _STATE_NAME(START_PENDING);
    _STATE_NAME(RUNNING);
    _STATE_NAME(STOP_PENDING);
    _STATE_NAME(STOPPED);
    default:
        break;
    }

    return _T("UNKNOWN");

#undef  _STATE_NAME
}

static VOID
ReportStatus(
    _In_ DWORD          CurrentState,
    _In_ DWORD          Win32ExitCode,
    _In_ DWORD          WaitHint
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    static DWORD        CheckPoint = 1;
    BOOL                Success;
    HRESULT             Error;

    LogInfo("====> (%s)", ServiceStateName(CurrentState));

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

    LogInfo("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static FORCEINLINE VOID
__InitializeListHead(
    _In_ PLIST_ENTRY    ListEntry
    )
{
    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static FORCEINLINE VOID
__InsertTailList(
    _In_ PLIST_ENTRY    ListHead,
    _In_ PLIST_ENTRY    ListEntry
    )
{
    ListEntry->Blink = ListHead->Blink;
    ListEntry->Flink = ListHead;
    ListHead->Blink->Flink = ListEntry;
    ListHead->Blink = ListEntry;
}

static FORCEINLINE VOID
__RemoveEntryList(
    _In_ PLIST_ENTRY    ListEntry
    )
{
    PLIST_ENTRY         Flink;
    PLIST_ENTRY         Blink;

    Flink = ListEntry->Flink;
    Blink = ListEntry->Blink;
    Flink->Blink = Blink;
    Blink->Flink = Flink;

    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static VOID
PutString(
    _In_ HANDLE     Handle,
    _In_ PUCHAR     Buffer,
    _In_ DWORD      Length
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
    _In_ LPVOID         Argument
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

    LogInfo("====> %s", Console->DeviceName);

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

    LogInfo("<==== %s", Console->DeviceName);

    return 0;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
ServerThread(
    _In_ LPVOID         Argument
    )
{
    PMONITOR_CONSOLE    Console = (PMONITOR_CONSOLE)Argument;
    TCHAR               PipeName[MAXIMUM_BUFFER_SIZE];
    OVERLAPPED          Overlapped;
    HANDLE              Handle[2];
    HANDLE              Pipe;
    DWORD               Object;
    PMONITOR_CONNECTION Connection;
    HRESULT             Error;
    SECURITY_ATTRIBUTES SecurityAttributes;

    LogInfo("====> %s", Console->DeviceName);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handle[0] = Console->ServerEvent;
    Handle[1] = Overlapped.hEvent;

    Error = StringCchPrintf(PipeName,
                            MAXIMUM_BUFFER_SIZE,
                            _T("%s%s"),
                            _T(PIPE_BASE_NAME),
                            Console->DeviceName);
    if (Error != S_OK && Error != STRSAFE_E_INSUFFICIENT_BUFFER)
        goto fail2;

    LogInfo("%s", PipeName);

    ZeroMemory(&SecurityAttributes, sizeof(SECURITY_ATTRIBUTES));
    SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    SecurityAttributes.bInheritHandle = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(_T(PIPE_SDDL),
                                                             SDDL_REVISION_1,
                                                             &SecurityAttributes.lpSecurityDescriptor,
                                                             NULL))
        goto fail3;

    for (;;) {
        Pipe = CreateNamedPipe(PipeName,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
                               PIPE_UNLIMITED_INSTANCES,
                               MAXIMUM_BUFFER_SIZE,
                               MAXIMUM_BUFFER_SIZE,
                               0,
                               &SecurityAttributes);
        if (Pipe == INVALID_HANDLE_VALUE)
            goto fail4;

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
            goto fail5;

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
            goto fail6;
    }

    LocalFree(&SecurityAttributes.lpSecurityDescriptor);

    CloseHandle(Overlapped.hEvent);

    LogInfo("<==== %s", Console->DeviceName);

    return 0;

fail6:
    LogError("fail6");

    free(Connection);

fail5:
    LogError("fail5");

    CloseHandle(Pipe);

fail4:
    LogError("fail4");

    LocalFree(&SecurityAttributes.lpSecurityDescriptor);

fail3:
    LogError("fail3");

fail2:
    LogError("fail2");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
DeviceThread(
    _In_ LPVOID         Argument
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

    LogInfo("====> %s", Console->DeviceName);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Console->DeviceEvent;
    Handles[1] = Overlapped.hEvent;

    Device = CreateFile(Console->DevicePath,
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

    LogInfo("<==== %s", Console->DeviceName);

    return 0;

fail2:
    LogError("fail2\n");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

_Success_(return != FALSE)
static BOOL
GetExecutable(
    _In_ PTSTR              DeviceName,
    _Outptr_result_z_ PTSTR *Executable
    )
{
    PMONITOR_CONTEXT        Context = &MonitorContext;
    HKEY                    Key;
    DWORD                   MaxValueLength;
    DWORD                   ExecutableLength;
    DWORD                   Type;
    HRESULT                 Error;

    Error = RegOpenKeyEx(Context->ParametersKey,
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

    ExecutableLength = (MaxValueLength + 1) * sizeof(TCHAR);

    *Executable = calloc(1, ExecutableLength);
    if (*Executable == NULL)
        goto fail3;

    Error = RegQueryValueEx(Key,
                            _T("Executable"),
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

    LogInfo("%s = %s", DeviceName, *Executable);

    RegCloseKey(Key);

    return TRUE;

fail5:
    LogError("fail5");

fail4:
    LogError("fail4");

    free(*Executable);

fail3:
    LogError("fail3");

fail2:
    LogError("fail2");

    RegCloseKey(Key);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

DWORD WINAPI
ExecutableThread(
    _In_ LPVOID         Argument
    )
{
    PMONITOR_CONSOLE    Console = (PMONITOR_CONSOLE)Argument;
    PTSTR               Executable;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFO         StartupInfo;
    BOOL                Success;
    HANDLE              Handle[2];
    DWORD               Object;
    HRESULT             Error;

    LogInfo("====> %s", Console->DeviceName);

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

    LogInfo("Executing: %s", Executable);

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
    LogInfo("<==== %s", Console->DeviceName);

    return 0;

fail1:
    Error = GetLastError();

    free(Executable);

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

static PMONITOR_CONSOLE
ConsoleCreate(
    _In_ PTSTR              DevicePath
    )
{
    PMONITOR_CONTEXT        Context = &MonitorContext;
    PMONITOR_CONSOLE        Console;
    DEV_BROADCAST_HANDLE    Handle;
    CHAR                    DeviceName[MAX_PATH];
#ifdef UNICODE
    TCHAR                   DeviceNameString[MAX_PATH];
    errno_t                 StringError;
#else
#define DeviceNameString    DeviceName
#endif
    DWORD                   Bytes;
    BOOL                    Success;
    HRESULT                 Error;

    LogInfo("====> %s", DevicePath);

    Console = malloc(sizeof(MONITOR_CONSOLE));
    if (Console == NULL)
        goto fail1;

    memset(Console, 0, sizeof(MONITOR_CONSOLE));
    __InitializeListHead(&Console->ListHead);
    __InitializeListHead(&Console->ListEntry);
    InitializeCriticalSection(&Console->CriticalSection);

    Console->DevicePath = _tcsdup(DevicePath);
    if (Console->DevicePath == NULL)
        goto fail2;

    Console->DeviceHandle = CreateFile(DevicePath,
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
#ifdef UNICODE
    StringError = mbstowcs_s(NULL,
                             DeviceNameString,
                             sizeof(DeviceNameString) / sizeof(TCHAR),
                             DeviceName,
                             strlen(DeviceName));
    if (StringError != 0)
        goto fail5;
#endif

    Console->DeviceName = _tcsdup(DeviceNameString);
    if (Console->DeviceName == NULL)
        goto fail6;

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
        goto fail7;

    Console->DeviceEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);
    if (Console->DeviceEvent == NULL)
        goto fail8;

    Console->DeviceThread = CreateThread(NULL,
                                         0,
                                         DeviceThread,
                                         Console,
                                         0,
                                         NULL);
    if (Console->DeviceThread == NULL)
        goto fail9;

    Console->ServerEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);
    if (Console->ServerEvent == NULL)
        goto fail10;

    Console->ServerThread = CreateThread(NULL,
                                         0,
                                         ServerThread,
                                         Console,
                                         0,
                                         NULL);
    if (Console->ServerThread == NULL)
        goto fail11;

    Console->ExecutableEvent = CreateEvent(NULL,
                                           TRUE,
                                           FALSE,
                                           NULL);
    if (Console->ExecutableEvent == NULL)
        goto fail12;

    Console->ExecutableThread = CreateThread(NULL,
                                             0,
                                             ExecutableThread,
                                             Console,
                                             0,
                                             NULL);
    if (Console->ExecutableThread == NULL)
        goto fail13;

    LogInfo("<==== %s", Console->DeviceName);

    return Console;

fail13:
    LogError("fail13");

    CloseHandle(Console->ExecutableEvent);
    Console->ExecutableEvent = NULL;

fail12:
    LogError("fail12");

    SetEvent(Console->ServerEvent);
    WaitForSingleObject(Console->ServerThread, INFINITE);

fail11:
    LogError("fail11");

    CloseHandle(Console->ServerEvent);
    Console->ServerEvent = NULL;

fail10:
    LogError("fail10");

    SetEvent(Console->DeviceEvent);
    WaitForSingleObject(Console->DeviceThread, INFINITE);

fail9:
    LogError("fail9");

    CloseHandle(Console->DeviceEvent);
    Console->DeviceEvent = NULL;

fail8:
    LogError("fail8");

    UnregisterDeviceNotification(Console->DeviceNotification);
    Console->DeviceNotification = NULL;

fail7:
    LogError("fail7");

    ECHO(Console->DeviceHandle, "\r\n[DETACHED]\r\n");

    free(Console->DevicePath);
    Console->DevicePath = NULL;

fail6:
    LogError("fail6");

#ifdef UNICODE
fail5:
    LogError("fail5");
#endif

fail4:
    LogError("fail4");

    CloseHandle(Console->DeviceHandle);
    Console->DeviceHandle = INVALID_HANDLE_VALUE;

fail3:
    LogError("fail3");

    free(Console->DevicePath);
    Console->DevicePath = NULL;

fail2:
    LogError("fail2");

    DeleteCriticalSection(&Console->CriticalSection);
    ZeroMemory(&Console->ListHead, sizeof(LIST_ENTRY));
    ZeroMemory(&Console->ListEntry, sizeof(LIST_ENTRY));

    free(Console);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return NULL;
}

static FORCEINLINE VOID
ConsoleWaitForPipes(
    _In_ PMONITOR_CONSOLE   Console
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
    _In_ PMONITOR_CONSOLE   Console
    )
{
    LogInfo("====> %s", Console->DeviceName);

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

    LogInfo("<====");
}

static BOOL
MonitorAdd(
    _In_ PTCHAR         DevicePath
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_CONSOLE    Console;

    LogInfo("=====> %s", DevicePath);

    Console = ConsoleCreate(DevicePath);
    if (Console == NULL)
        goto fail1;

    EnterCriticalSection(&Context->CriticalSection);
    __InsertTailList(&Context->ListHead, &Console->ListEntry);
    ++Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    LogInfo("<===== %s", Console->DeviceName);

    return TRUE;

fail1:
    LogError("fail1");

    return FALSE;
}

static BOOL
MonitorRemove(
    _In_ HANDLE         DeviceHandle
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_CONSOLE    Console;
    PLIST_ENTRY         ListEntry;

    LogInfo("=====> 0x%p", DeviceHandle);

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

    LogError("DeviceHandle 0x%p not found", DeviceHandle);

    return FALSE;

found:
    __RemoveEntryList(&Console->ListEntry);
    --Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    ConsoleDestroy(Console);

    LogInfo("<=====");

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
    PSP_DEVICE_INTERFACE_DETAIL_DATA    DeviceInterfaceDetail;
    PMONITOR_CONSOLE                    Console;
    DWORD                               Size;
    DWORD                               Index;
    HRESULT                             Error;
    BOOL                                Success;

    LogInfo("====>");

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

        Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
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
            sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA);

        Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
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
        LogError("fail5");
    fail4:
        LogError("fail4");

        free(DeviceInterfaceDetail);

    fail3:
        LogError("fail3");
    fail2:
        Error = GetLastError();

        {
            PTSTR   Message;
            Message = GetErrorMessage(Error);
            LogError("fail2 (%s)", Message);
            LocalFree(Message);
        }
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    LogInfo("<====");

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
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

    LogInfo("=====>");

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

    LogInfo("<=====");
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    _In_ DWORD          Ctrl,
    _In_ DWORD          EventType,
    _In_ LPVOID         EventData,
    _In_ LPVOID         Argument
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
                PDEV_BROADCAST_DEVICEINTERFACE  Interface = EventData;

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

    if (TraceLoggingRegister(MonitorTraceLoggingProvider) != ERROR_SUCCESS)
        LogInfo("TraceLoggingRegister failed");

    LogInfo("====>");

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         _T(PARAMETERS_KEY(__MODULE__)),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Context->Service = RegisterServiceCtrlHandlerEx(_T(MONITOR_NAME),
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail2;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail3;

    ZeroMemory(&Interface, sizeof (Interface));
    Interface.dbcc_size = sizeof (Interface);
    Interface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    Interface.dbcc_classguid = GUID_XENCONS_DEVICE;

    Context->InterfaceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Interface,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Context->InterfaceNotification == NULL)
        goto fail4;

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    __InitializeListHead(&Context->ListHead);
    InitializeCriticalSection(&Context->CriticalSection);

    MonitorEnumerate();

    LogInfo("Waiting...");
    WaitForSingleObject(Context->StopEvent, INFINITE);
    LogInfo("Wait Complete");

    MonitorRemoveAll();

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    UnregisterDeviceNotification(Context->InterfaceNotification);

    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    RegCloseKey(Context->ParametersKey);

    LogInfo("<====");

    TraceLoggingUnregister(MonitorTraceLoggingProvider);

    return;

fail4:
    LogError("fail4");

    CloseHandle(Context->StopEvent);

fail3:
    LogError("fail3");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

fail2:
    LogError("fail2");

    RegCloseKey(Context->ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    TraceLoggingUnregister(MonitorTraceLoggingProvider);
}

static BOOL
MonitorCreate(
    VOID
    )
{
    SC_HANDLE   SCManager;
    SC_HANDLE   Service;
    TCHAR       Path[MAX_PATH];
    HRESULT     Error;

    LogInfo("====>");

    if(!GetModuleFileName(NULL, Path, MAX_PATH))
        goto fail1;

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail2;

    Service = CreateService(SCManager,
                            _T(MONITOR_NAME),
                            _T(MONITOR_DISPLAYNAME),
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

    LogInfo("<====");

    return TRUE;

fail3:
    LogError("fail3");

    CloseServiceHandle(SCManager);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
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

    LogInfo("====>");

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail1;

    Service = OpenService(SCManager,
                          _T(MONITOR_NAME),
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

    LogInfo("<====");

    return TRUE;

fail4:
    LogError("fail4");

fail3:
    LogError("fail3");

    CloseServiceHandle(Service);

fail2:
    LogError("fail2");

    CloseServiceHandle(SCManager);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
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
        { _T(MONITOR_NAME), MonitorMain },
        { NULL, NULL }
    };
    HRESULT             Error;

    LogInfo("%s (%s) ====>",
            _T(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
            _T(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    if (!StartServiceCtrlDispatcher(Table))
        goto fail1;

    LogInfo("%s (%s) <====",
            _T(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
            _T(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

int CALLBACK
_tWinMain(
    _In_        HINSTANCE   Current,
    _In_opt_    HINSTANCE   Previous,
    _In_        LPTSTR      CmdLine,
    _In_        int         CmdShow
    )
{
    BOOL                    Success;

    UNREFERENCED_PARAMETER(Current);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdShow);

    if (_tcslen(CmdLine) != 0) {
         if (_tcsicmp(CmdLine, TEXT("create")) == 0)
             Success = MonitorCreate();
         else if (_tcsicmp(CmdLine, TEXT("delete")) == 0)
             Success = MonitorDelete();
         else
             Success = FALSE;
    } else
        Success = MonitorEntry();

    return Success ? 0 : 1;
}
