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
#include <tchar.h>
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

typedef struct _MONITOR_PIPE {
    HANDLE                  Pipe;
    HANDLE                  Event;
    HANDLE                  Thread;
    LIST_ENTRY              ListEntry;
} MONITOR_PIPE, *PMONITOR_PIPE;

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HKEY                    ParametersKey;
    HANDLE                  EventLog;
    HANDLE                  StopEvent;
    HANDLE                  AddEvent;
    HANDLE                  RemoveEvent;
    PTCHAR                  Executable;
    HDEVNOTIFY              InterfaceNotification;
    PTCHAR                  DevicePath;
    HDEVNOTIFY              DeviceNotification;
    HANDLE                  Device;
    HANDLE                  MonitorEvent;
    HANDLE                  MonitorThread;
    HANDLE                  DeviceEvent;
    HANDLE                  DeviceThread;
    HANDLE                  ServerEvent;
    HANDLE                  ServerThread;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

MONITOR_CONTEXT MonitorContext;

#define PIPE_NAME TEXT("\\\\.\\pipe\\xencons")

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
    const TCHAR         *Strings[1];
#endif
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer,
                              MAXIMUM_BUFFER_SIZE,
                              Format,
                              Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
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
        ReportEvent(Context->EventLog,
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
    __Log(TEXT(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  HRESULT Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

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
    IN  DWORD           WaitHint)
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
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
MonitorGetPath(
    IN  const GUID  *Guid,
    OUT PTCHAR      *Path
    )
{
    HDEVINFO                            DeviceInfoSet;
    SP_DEVICE_INTERFACE_DATA            DeviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA    DeviceInterfaceDetail;
    DWORD                               Size;
    HRESULT                             Error;
    BOOL                                Success;

    Log("====>");

    DeviceInfoSet = SetupDiGetClassDevs(Guid,
                                        NULL,
                                        NULL,
                                        DIGCF_PRESENT |
                                        DIGCF_DEVICEINTERFACE);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        goto fail1;

    DeviceInterfaceData.cbSize = sizeof (SP_DEVICE_INTERFACE_DATA);

    Success = SetupDiEnumDeviceInterfaces(DeviceInfoSet,
                                          NULL,
                                          Guid,
                                          0,
                                          &DeviceInterfaceData);
    if (!Success)
        goto fail2;

    Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
                                              &DeviceInterfaceData,
                                              NULL,
                                              0,
                                              &Size,
                                              NULL);
    if (!Success && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        goto fail3;

    DeviceInterfaceDetail = calloc(1, Size);
    if (DeviceInterfaceDetail == NULL)
        goto fail4;

    DeviceInterfaceDetail->cbSize =
        sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA);

    Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
                                              &DeviceInterfaceData,
                                              DeviceInterfaceDetail,
                                              Size,
                                              NULL,
                                              NULL);
    if (!Success)
        goto fail5;

    *Path = _tcsdup(DeviceInterfaceDetail->DevicePath);

    if (*Path == NULL)
        goto fail6;

    Log("%s", *Path);

    free(DeviceInterfaceDetail);

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    Log("<====");

    return TRUE;

fail6:
    Log("fail6");

fail5:
    Log("fail5");

    free(DeviceInterfaceDetail);

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

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

DWORD WINAPI
PipeThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_PIPE       Pipe = (PMONITOR_PIPE)Argument;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    OVERLAPPED          Overlapped;
    HANDLE              Handle[2];
    DWORD               Length;
    DWORD               Object;
    HRESULT             Error;

    Log("====>");

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handle[0] = Pipe->Event;
    Handle[1] = Overlapped.hEvent;

    EnterCriticalSection(&Context->CriticalSection);
    __InsertTailList(&Context->ListHead, &Pipe->ListEntry);
    ++Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    for (;;) {
        (VOID) ReadFile(Pipe->Pipe,
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

        if (!GetOverlappedResult(Pipe->Pipe,
                                 &Overlapped,
                                 &Length,
                                 FALSE))
            break;

        ResetEvent(Overlapped.hEvent);

        PutString(Context->Device,
                  Buffer,
                  Length);
    }

    EnterCriticalSection(&Context->CriticalSection);
    __RemoveEntryList(&Pipe->ListEntry);
    --Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    CloseHandle(Overlapped.hEvent);

    CloseHandle(Pipe->Pipe);
    CloseHandle(Pipe->Thread);
    free(Pipe);

    Log("<====");

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
    PMONITOR_CONTEXT    Context = &MonitorContext;
    OVERLAPPED          Overlapped;
    HANDLE              Handle[2];
    HANDLE              Pipe;
    DWORD               Object;
    PMONITOR_PIPE       Instance;
    HRESULT             Error;

    UNREFERENCED_PARAMETER(Argument);

    Log("====>");

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handle[0] = Context->ServerEvent;
    Handle[1] = Overlapped.hEvent;

    for (;;) {
        Pipe = CreateNamedPipe(PIPE_NAME,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                               PIPE_UNLIMITED_INSTANCES,
                               MAXIMUM_BUFFER_SIZE,
                               MAXIMUM_BUFFER_SIZE,
                               0,
                               NULL);
        if (Pipe == INVALID_HANDLE_VALUE)
            goto fail2;

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

        Instance = (PMONITOR_PIPE)malloc(sizeof(MONITOR_PIPE));
        if (Instance == NULL)
            goto fail3;

        __InitializeListHead(&Instance->ListEntry);
        Instance->Pipe = Pipe;
        Instance->Event = Context->ServerEvent;
        Instance->Thread = CreateThread(NULL,
                                        0,
                                        PipeThread,
                                        Instance,
                                        0,
                                        NULL);
        if (Instance->Thread == INVALID_HANDLE_VALUE)
            goto fail4;
    }

    CloseHandle(Overlapped.hEvent);

    Log("<====");

    return 0;

fail4:
    Log("fail4");

    free(Instance);

fail3:
    Log("fail3");

    CloseHandle(Pipe);

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

    return 1;
}

DWORD WINAPI
MonitorThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFO         StartupInfo;
    BOOL                Success;
    HANDLE              Handle[2];
    DWORD               Object;
    HRESULT             Error;

    UNREFERENCED_PARAMETER(Argument);

    Log("====>");

    // If there is no executable, this thread can finish now.
    if (Context->Executable == NULL)
        goto done;

again:
    ZeroMemory(&ProcessInfo, sizeof (ProcessInfo));
    ZeroMemory(&StartupInfo, sizeof (StartupInfo));
    StartupInfo.cb = sizeof (StartupInfo);

    Log("Executing: %s", Context->Executable);

#pragma warning(suppress:6053) // CommandLine might not be NUL-terminated
    Success = CreateProcess(NULL,
                            Context->Executable,
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

    Handle[0] = Context->MonitorEvent;
    Handle[1] = ProcessInfo.hProcess;

    Object = WaitForMultipleObjects(ARRAYSIZE(Handle),
                                   Handle,
                                   FALSE,
                                   INFINITE);

#define WAIT_OBJECT_1 (WAIT_OBJECT_0 + 1)

    switch (Object) {
    case WAIT_OBJECT_0:
        ResetEvent(Context->MonitorEvent);

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

done:
    Log("<====");

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
DeviceThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    OVERLAPPED          Overlapped;
    HANDLE              Device;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    DWORD               Length;
    DWORD               Wait;
    HANDLE              Handles[2];
    DWORD               Error;

    UNREFERENCED_PARAMETER(Argument);

    Log("====>");

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Context->DeviceEvent;
    Handles[1] = Overlapped.hEvent;

    Device = CreateFile(Context->DevicePath,
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

        EnterCriticalSection(&Context->CriticalSection);

        for (ListEntry = Context->ListHead.Flink;
             ListEntry != &Context->ListHead;
             ListEntry = ListEntry->Flink) {
            PMONITOR_PIPE   Instance;

            Instance = CONTAINING_RECORD(ListEntry, MONITOR_PIPE, ListEntry);

            PutString(Instance->Pipe,
                      Buffer,
                      Length);
        }
        LeaveCriticalSection(&Context->CriticalSection);
    }

    CloseHandle(Device);

    CloseHandle(Overlapped.hEvent);

    Log("<====");

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

#define ECHO(_Handle, _Buffer) \
    PutString((_Handle), (PUCHAR)TEXT(_Buffer), (DWORD)_tcslen((_Buffer)) * sizeof(TCHAR))

static VOID
MonitorAdd(
    VOID
    )
{
    PMONITOR_CONTEXT        Context = &MonitorContext;
    PTCHAR                  Path;
    DEV_BROADCAST_HANDLE    Handle;
    HRESULT                 Error;
    BOOL                    Success;

    if (Context->Device != INVALID_HANDLE_VALUE)
        return;

    Log("====>");

    Success = MonitorGetPath(&GUID_XENCONS_DEVICE, &Path);

    if (!Success)
        goto fail1;

    Context->Device = CreateFile(Path,
                                 GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);

    if (Context->Device == INVALID_HANDLE_VALUE)
        goto fail2;

    ECHO(Context->Device, "\r\n[ATTACHED]\r\n");

    ZeroMemory(&Handle, sizeof (Handle));
    Handle.dbch_size = sizeof (Handle);
    Handle.dbch_devicetype = DBT_DEVTYP_HANDLE;
    Handle.dbch_handle = Context->Device;

    Context->DeviceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Handle,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Context->DeviceNotification == NULL)
        goto fail3;

    Context->DevicePath = Path;
    __InitializeListHead(&Context->ListHead);
    InitializeCriticalSection(&Context->CriticalSection);

    Context->MonitorEvent = CreateEvent(NULL,
                                        TRUE,
                                        FALSE,
                                        NULL);

    if (Context->MonitorEvent == NULL)
        goto fail4;

    Context->MonitorThread = CreateThread(NULL,
                                          0,
                                          MonitorThread,
                                          NULL,
                                          0,
                                          NULL);

    if (Context->MonitorThread == INVALID_HANDLE_VALUE)
        goto fail5;

    Context->DeviceEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);

    if (Context->DeviceEvent == NULL)
        goto fail6;

    Context->DeviceThread = CreateThread(NULL,
                                         0,
                                         DeviceThread,
                                         NULL,
                                         0,
                                         NULL);

    if (Context->DeviceThread == INVALID_HANDLE_VALUE)
        goto fail7;

    Context->ServerEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);
    if (Context->ServerEvent == NULL)
        goto fail8;

    Context->ServerThread = CreateThread(NULL,
                                         0,
                                         ServerThread,
                                         NULL,
                                         0,
                                         NULL);
    if (Context->ServerThread == INVALID_HANDLE_VALUE)
        goto fail9;

    Log("<====");

    return;

fail9:
    Log("fail9");

    CloseHandle(Context->ServerEvent);
    Context->ServerEvent = NULL;

fail8:
    Log("fail8");

    SetEvent(Context->DeviceEvent);
    WaitForSingleObject(Context->DeviceThread, INFINITE);

fail7:
    Log("fail7\n");

    CloseHandle(Context->DeviceEvent);
    Context->DeviceEvent = NULL;

fail6:
    Log("fail6\n");

    SetEvent(Context->MonitorThread);
    WaitForSingleObject(Context->MonitorThread, INFINITE);

fail5:
    Log("fail5");

    CloseHandle(Context->MonitorEvent);
    Context->MonitorEvent = NULL;

fail4:
    Log("fail4");

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    free(Context->DevicePath);
    Context->DevicePath = NULL;

    UnregisterDeviceNotification(Context->DeviceNotification);
    Context->DeviceNotification = NULL;

fail3:
    Log("fail3");

    CloseHandle(Context->Device);
    Context->Device = INVALID_HANDLE_VALUE;

fail2:
    Log("fail2");

    free(Path);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static VOID
MonitorWaitForPipeThreads(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HANDLE              *Handles;
    DWORD               Index;
    PLIST_ENTRY         ListEntry;

    EnterCriticalSection(&Context->CriticalSection);

    if (Context->ListCount == 0)
        goto fail1;

    Handles = (HANDLE*)malloc(sizeof(HANDLE) * Context->ListCount);
    if (Handles == NULL)
        goto fail2;

    Index = 0;
    for (ListEntry = Context->ListHead.Flink;
         ListEntry != &Context->ListHead && Index < Context->ListCount;
         ListEntry = ListEntry->Flink) {
        PMONITOR_PIPE Pipe = CONTAINING_RECORD(ListEntry, MONITOR_PIPE, ListEntry);
        Handles[Index++] = Pipe->Thread;
    }

    Context->ListCount = 0;

    LeaveCriticalSection(&Context->CriticalSection);

#pragma warning(suppress:6385) // Reading invalid data from 'Handles'...
    WaitForMultipleObjects(Index,
                           Handles,
                           TRUE,
                           INFINITE);
    free(Handles);
    return;

fail2:
    Log("fail2");

fail1:
    Log("fail1");

    LeaveCriticalSection(&Context->CriticalSection);

    return;
}

static VOID
MonitorRemove(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;

    if (Context->Device == INVALID_HANDLE_VALUE)
        return;

    Log("====>");

    SetEvent(Context->ServerEvent);
    MonitorWaitForPipeThreads();
    WaitForSingleObject(Context->ServerThread, INFINITE);

    CloseHandle(Context->ServerEvent);
    Context->ServerEvent = NULL;

    SetEvent(Context->DeviceEvent);
    WaitForSingleObject(Context->DeviceThread, INFINITE);

    CloseHandle(Context->DeviceEvent);
    Context->DeviceEvent = NULL;

    SetEvent(Context->MonitorEvent);
    WaitForSingleObject(Context->MonitorThread, INFINITE);

    CloseHandle(Context->MonitorEvent);
    Context->MonitorEvent = NULL;

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    free(Context->DevicePath);
    Context->DevicePath = NULL;

    UnregisterDeviceNotification(Context->DeviceNotification);
    Context->DeviceNotification = NULL;

    ECHO(Context->Device, "\r\n[DETACHED]\r\n");

    CloseHandle(Context->Device);
    Context->Device = INVALID_HANDLE_VALUE;

    Log("<====");
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
                PDEV_BROADCAST_DEVICEINTERFACE  Interface = EventData;

                if (IsEqualGUID(&Interface->dbcc_classguid,
                               &GUID_XENCONS_DEVICE))
                    SetEvent(Context->AddEvent);
            }
            break;

        case DBT_DEVICEQUERYREMOVE:
            if (Header->dbch_devicetype == DBT_DEVTYP_HANDLE) {
                PDEV_BROADCAST_HANDLE Device = EventData;

                if (Device->dbch_handle == Context->Device)
                    SetEvent(Context->RemoveEvent);
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

static BOOL
GetExecutable(
    OUT PTCHAR          *Executable
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               MaxValueLength;
    DWORD               ExecutableLength;
    DWORD               Type;
    HRESULT             Error;

    Error = RegQueryInfoKey(Context->ParametersKey,
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
        goto fail1;
    }

    ExecutableLength = MaxValueLength + sizeof (TCHAR);

    *Executable = calloc(1, ExecutableLength);
    if (Executable == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->ParametersKey,
                            "Executable",
                            NULL,
                            &Type,
                            (LPBYTE)(*Executable),
                            &ExecutableLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    Log("%s", *Executable);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(*Executable);

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

VOID WINAPI
MonitorMain(
    _In_    DWORD                   argc,
    _In_    LPTSTR                  *argv
    )
{
    PMONITOR_CONTEXT                Context = &MonitorContext;
    DEV_BROADCAST_DEVICEINTERFACE   Interface;
    HRESULT                         Error;
    BOOL                            Success;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Log("====>");

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         PARAMETERS_KEY(__MODULE__),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Context->Service = RegisterServiceCtrlHandlerEx(MONITOR_NAME,
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail2;

    Context->EventLog = RegisterEventSource(NULL,
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

    Context->AddEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);

    if (Context->AddEvent == NULL)
        goto fail5;

    Context->RemoveEvent = CreateEvent(NULL,
                                       TRUE,
                                       FALSE,
                                       NULL);

    if (Context->RemoveEvent == NULL)
        goto fail6;

    Success = GetExecutable(&Context->Executable);
    if (!Success)
        Context->Executable = NULL;

    Context->Device = INVALID_HANDLE_VALUE;

    ZeroMemory(&Interface, sizeof (Interface));
    Interface.dbcc_size = sizeof (Interface);
    Interface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    Interface.dbcc_classguid = GUID_XENCONS_DEVICE;

    Context->InterfaceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Interface,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Context->InterfaceNotification == NULL)
        goto fail7;

    // The device may already by present
    SetEvent(Context->AddEvent);

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    for (;;) {
        HANDLE  Events[3];
        DWORD   Object;

        Events[0] = Context->StopEvent;
        Events[1] = Context->AddEvent;
        Events[2] = Context->RemoveEvent;

        Log("waiting (%u)...", ARRAYSIZE(Events));
        Object = WaitForMultipleObjects(ARRAYSIZE(Events),
                                        Events,
                                        FALSE,
                                        INFINITE);
        Log("awake");

#define WAIT_OBJECT_1 (WAIT_OBJECT_0 + 1)
#define WAIT_OBJECT_2 (WAIT_OBJECT_0 + 2)

        switch (Object) {
        case WAIT_OBJECT_0:
            ResetEvent(Context->StopEvent);
            goto done;

        case WAIT_OBJECT_1:
            ResetEvent(Context->AddEvent);
            MonitorAdd();
            break;

        case WAIT_OBJECT_2:
            ResetEvent(Context->RemoveEvent);
            MonitorRemove();

        default:
            break;
        }

#undef WAIT_OBJECT_1
#undef WAIT_OBJECT_2
    }

done:
    MonitorRemove();

    UnregisterDeviceNotification(Context->InterfaceNotification);

    free(Context->Executable);

    CloseHandle(Context->RemoveEvent);

    CloseHandle(Context->AddEvent);

    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    (VOID) DeregisterEventSource(Context->EventLog);

    CloseHandle(Context->ParametersKey);

    Log("<====");

    return;

fail7:
    Log("fail7");

    CloseHandle(Context->RemoveEvent);

fail6:
    Log("fail6");

    CloseHandle(Context->AddEvent);

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
    TCHAR       Path[MAX_PATH];
    HRESULT     Error;

    Log("====>");

    if(!GetModuleFileName(NULL, Path, MAX_PATH))
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
_tWinMain(
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
