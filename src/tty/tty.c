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

#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <userenv.h>

typedef struct _TTY_STREAM {
    HANDLE  Read;
    HANDLE  Write;
} TTY_STREAM, *PTTY_STREAM;

#define PIPE_NAME TEXT("\\\\.\\pipe\\xencons")
#define MAXIMUM_BUFFER_SIZE 1024

typedef struct _TTY_CONTEXT {
    TTY_STREAM          ChildStdIn;
    TTY_STREAM          ChildStdOut;
    TTY_STREAM          Device;
    TCHAR               UserName[MAXIMUM_BUFFER_SIZE];
    TCHAR               Password[MAXIMUM_BUFFER_SIZE];
    HANDLE              Token;
    PROCESS_INFORMATION ProcessInfo;
} TTY_CONTEXT, *PTTY_CONTEXT;

TTY_CONTEXT TtyContext;

static BOOL
CreateChild(
    VOID
    )
{
    PTTY_CONTEXT            Context = &TtyContext;
    TCHAR                   CommandLine[] = TEXT("c:\\windows\\system32\\cmd.exe /q /a");
    PVOID                   Environment;
    PROFILEINFO             ProfileInfo;
    DWORD                   Size;
    TCHAR                   ProfileDir[MAXIMUM_BUFFER_SIZE];
    STARTUPINFO             StartupInfo;
    BOOL                    Success;

    Success = CreateEnvironmentBlock(&Environment,
                                     Context->Token,
                                     FALSE);
    if (!Success)
        return FALSE;

    ZeroMemory(&ProfileInfo, sizeof (ProfileInfo));
    ProfileInfo.dwSize = sizeof (ProfileInfo);
    ProfileInfo.lpUserName = Context->UserName;

    Success = LoadUserProfile(Context->Token, &ProfileInfo);
    if (!Success)
        return FALSE;

    Size = sizeof (ProfileDir);

    Success = GetUserProfileDirectory(Context->Token,
                                      ProfileDir,
                                      &Size);
    if (!Success)
        return FALSE;

    Success = ImpersonateLoggedOnUser(Context->Token);
    if (!Success)
        return FALSE;

    ZeroMemory(&StartupInfo, sizeof (StartupInfo));
    StartupInfo.cb = sizeof (StartupInfo);

    StartupInfo.hStdInput = Context->ChildStdIn.Read;
    StartupInfo.hStdOutput = Context->ChildStdOut.Write;
    StartupInfo.hStdError = Context->ChildStdOut.Write;

    StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

#pragma warning(suppress:6335) // leaking handle information
    Success = CreateProcessAsUser(Context->Token,
                                  NULL,
                                  CommandLine,
                                  NULL,
                                  NULL,
                                  TRUE,
                                  CREATE_UNICODE_ENVIRONMENT,
                                  Environment,
                                  ProfileDir,
                                  &StartupInfo,
                                  &Context->ProcessInfo);

    DestroyEnvironmentBlock(Environment);

    if (!Success)
        return FALSE;

    SetConsoleCtrlHandler(NULL, TRUE);

    return TRUE;
}

static VOID
PutCharacter(
    IN  PTTY_STREAM Stream,
    IN  TCHAR       Character
    )
{
    WriteFile(Stream->Write,
              &Character,
              1,
              NULL,
              NULL);
}

static VOID
PutString(
    IN  PTTY_STREAM Stream,
    IN  PTCHAR      Buffer,
    IN  DWORD       Length
    )
{
    DWORD           Offset;

    Offset = 0;
    while (Offset < Length) {
        DWORD   Written;
        BOOL    Success;

        Success = WriteFile(Stream->Write,
                            &Buffer[Offset],
                            Length - Offset,
                            &Written,
                            NULL);
        if (!Success)
            break;

        Offset += Written;
    }
}

#define ECHO(_Stream, _Buffer) \
    PutString((_Stream), TEXT(_Buffer), (DWORD)_tcslen(_Buffer))

static BOOL
GetLine(
    IN  PTTY_STREAM Stream,
    IN  PTCHAR      Buffer,
    IN  DWORD       NumberOfBytesToRead,
    OUT LPDWORD     NumberOfBytesRead,
    IN  BOOL        NoEcho
    )
{
    DWORD           Offset;
    BOOL            Success = TRUE;

    Offset = 0;
    while (Offset < NumberOfBytesToRead) {
        TCHAR   Sequence[MAXIMUM_BUFFER_SIZE];
        PTCHAR  Character;
        DWORD   Read;

        Success = ReadFile(Stream->Read,
                           &Sequence,
                           sizeof (Sequence),
                           &Read,
                           NULL);
        if (!Success)
            break;

        Character = &Sequence[0];
        while (Read-- != 0) {
            Buffer[Offset] = *Character++;

            if (!iscntrl(Buffer[Offset])) {
                if (!NoEcho)
                    PutCharacter(Stream, Buffer[Offset]);
                Offset++;
            } else {
                if (Buffer[Offset] == 0x7F && // DEL
                    Offset != 0) {
                    --Offset;

                    if (Buffer[Offset] >= 0x00 &&
                        Buffer[Offset] < 0x20)
                        ECHO(Stream, "\b\b  \b\b");
                    else if (!NoEcho)
                        ECHO(Stream, "\b \b");
                } else if (Buffer[Offset] == 0x03 || // ^C
                           Buffer[Offset] == 0x0D) { // ^M
                    Offset++;
                    break;
                } else if (Buffer[Offset] >= 0x00 &&
                           Buffer[Offset] < 0x20) {
                    ECHO(Stream, "^");
                    PutCharacter(Stream, Buffer[Offset] + 0x40);
                    Offset++;
                }
            }

            if (Offset >= NumberOfBytesToRead)
                break;
        }

        if (Offset == 0)
            continue;

        if (Buffer[Offset - 1] == 0x03 || // ^C
            Buffer[Offset - 1] == 0x0D)   // ^M
            break;
    }

    ECHO(Stream, "\r\n");

    *NumberOfBytesRead = Offset;

    return Success;
}

static BOOL
GetCredentials(
    VOID
    )
{
    PTTY_CONTEXT    Context = &TtyContext;
    TCHAR           ComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    PTCHAR          End;
    DWORD           Size;
    BOOL            Success;

    ZeroMemory(ComputerName, sizeof (ComputerName));
    Size = sizeof (ComputerName);

    Success = GetComputerName(ComputerName, &Size);
    if (!Success)
        return FALSE;

    ECHO(&Context->Device, "\r\n");
    PutString(&Context->Device, ComputerName, Size);
    ECHO(&Context->Device, " login: ");

    ZeroMemory(Context->UserName, sizeof (Context->UserName));

    Success = GetLine(&Context->Device,
                      Context->UserName,
                      sizeof (Context->UserName),
                      &Size,
                      FALSE);
    if (!Success)
        return FALSE;

    End = _tcschr(Context->UserName, TEXT('\r'));
    if (End == NULL)
        return FALSE;

    *End = TEXT('\0');

    if (_tcslen(Context->UserName) == 0)
        return FALSE;

    ECHO(&Context->Device, "Password: ");

    ZeroMemory(Context->Password, sizeof (Context->Password));

    Success = GetLine(&Context->Device,
                      Context->Password,
                      sizeof (Context->Password),
                      &Size,
                      TRUE);
    if (!Success)
        return FALSE;

    End = _tcschr(Context->Password, TEXT('\r'));
    if (End == NULL)
        return FALSE;

    *End = TEXT('\0');

    return TRUE;
}

static DWORD WINAPI
TtyIn(
    IN  LPVOID      Argument
    )
{
    PTTY_CONTEXT    Context = &TtyContext;

    UNREFERENCED_PARAMETER(Argument);

    for (;;) {
        DWORD       Read;
        CHAR        Buffer[MAXIMUM_BUFFER_SIZE];
        BOOL        Success;

        Success = GetLine(&Context->Device,
                          Buffer,
                          sizeof (Buffer) - 1,
                          &Read,
                          FALSE);
        if (!Success)
            break;

        if (Read == 0)
            continue;

        if (Buffer[Read - 1] == 0x03) { // ^C
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
            continue;
        } else if (Buffer[Read - 1] == 0x0D) { // ^M
            Buffer[Read++] = '\n';

            Success = WriteFile(Context->ChildStdIn.Write,
                                Buffer,
                                Read,
                                NULL,
                                NULL);
            if (!Success)
                break;
        }
    }

    return 0;
}

static DWORD WINAPI
TtyOut(
    IN  LPVOID      Argument
    )
{
    PTTY_CONTEXT    Context = &TtyContext;

    UNREFERENCED_PARAMETER(Argument);

    for (;;) {
        DWORD       Read;
        DWORD       Written;
        CHAR        Buffer[MAXIMUM_BUFFER_SIZE];
        BOOL        Success;

        Success = ReadFile(Context->ChildStdOut.Read,
                           Buffer,
                           sizeof (Buffer),
                           &Read,
                           NULL);
        if (!Success)
            break;

        if (Read == 0)
            continue;

        Success = WriteFile(Context->Device.Write,
                            Buffer,
                            Read,
                            &Written,
                            NULL);
        if (!Success)
            break;
    }

    return 0;
}

void __cdecl
_tmain(
    IN  int             argc,
    IN  TCHAR           *argv[]
    )
{
    PTTY_CONTEXT        Context = &TtyContext;
    SECURITY_ATTRIBUTES Attributes;
    HANDLE              Handle[3];
    DWORD               Index;
    BOOL                Success;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Context->Device.Read = CreateFile(PIPE_NAME,
                                      GENERIC_READ,
                                      FILE_SHARE_WRITE,
                                      NULL,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      NULL);

    if (Context->Device.Read == INVALID_HANDLE_VALUE)
        ExitProcess(1);

    Context->Device.Write = CreateFile(PIPE_NAME,
                                       GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       NULL);

    if (Context->Device.Write == INVALID_HANDLE_VALUE)
        ExitProcess(1);

    Success = GetCredentials();
    if (!Success)
        ExitProcess(1);

    Success = LogonUser(Context->UserName,
                        NULL,
                        Context->Password,
                        LOGON32_LOGON_INTERACTIVE,
                        LOGON32_PROVIDER_DEFAULT,
                        &Context->Token);
    if (!Success)
        ExitProcess(1);

    Attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    Attributes.bInheritHandle = TRUE;
    Attributes.lpSecurityDescriptor = NULL;

    Success = CreatePipe(&Context->ChildStdOut.Read,
                         &Context->ChildStdOut.Write,
                         &Attributes,
                         0);
    if (!Success)
        ExitProcess(1);

    Success = SetHandleInformation(Context->ChildStdOut.Read,
                                   HANDLE_FLAG_INHERIT,
                                   0);
    if (!Success)
        ExitProcess(1);

    Success = CreatePipe(&Context->ChildStdIn.Read,
                         &Context->ChildStdIn.Write,
                         &Attributes,
                         0);
    if (!Success)
        ExitProcess(1);

    Success = SetHandleInformation(Context->ChildStdIn.Write,
                                   HANDLE_FLAG_INHERIT,
                                   0);
    if (!Success)
        ExitProcess(1);

    Success = CreateChild();

    if (!Success)
        ExitProcess(1);

    Handle[0] = Context->ProcessInfo.hThread;

    Handle[1] = CreateThread(NULL,
                             0,
                             TtyIn,
                             NULL,
                             0,
                             NULL);

    if (Handle[1] == INVALID_HANDLE_VALUE)
        ExitProcess(1);

    Handle[2] = CreateThread(NULL,
                             0,
                             TtyOut,
                             NULL,
                             0,
                             NULL);

    if (Handle[2] == INVALID_HANDLE_VALUE)
        ExitProcess(1);

    WaitForMultipleObjects(ARRAYSIZE(Handle),
                           Handle,
                           FALSE,
                           INFINITE);

    for (Index = 0; Index < ARRAYSIZE(Handle); Index++)
        if ( Handle[Index] != 0 )
            CloseHandle(Handle[Index]);

    CloseHandle(Context->ProcessInfo.hProcess);
}
