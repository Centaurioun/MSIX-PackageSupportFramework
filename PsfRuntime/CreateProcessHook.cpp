//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// The PsfRuntime intercepts all CreateProcess[AW] calls so that fixups can be propagated to child processes. The
// general call graph looks something like the following:
//      * CreateProcessFixup gets called by application code; forwards arguments on to:
//      * DetourCreateProcessWithDlls[AW], which calls:
//      * CreateProcessInterceptRunDll32, which identifies attempts to launch rundll32 from System32/SysWOW64,
//        redirecting those attempts to either PsfRunDll32.exe or PsfRunDll64.exe, and then calls:
//      * The actual implementation of CreateProcess[AW]
//
// NOTE: Other CreateProcess variants (e.g. CreateProcessAsUser[AW]) aren't currently detoured as the Detours framework
//       does not make it easy to accomplish that at this time.
//

#include <string_view>
#include <vector>

#include <windows.h>
#include <detours.h>
#include <psf_constants.h>
#include <psf_framework.h>
#include <psf_logging.h>

#include "Config.h"
#include <StartInfo_helper.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <findStringIC.h>

using namespace std::literals;


extern wchar_t g_PsfRunTimeModulePath[];

#if NEEDED
bool SetProcessPrivilege(LPCWSTR PrivilegeName, bool Enable)
{
    HANDLE Token;
    TOKEN_PRIVILEGES TokenPrivs;
    LUID TempLuid;
    bool Result;

    if (!::LookupPrivilegeValueW(NULL, PrivilegeName, &TempLuid))  return false;

    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))  
        return false;
   

    TokenPrivs.PrivilegeCount = 1;
    TokenPrivs.Privileges[0].Luid = TempLuid;
    TokenPrivs.Privileges[0].Attributes = (Enable ? SE_PRIVILEGE_ENABLED : 0);

    Result = (::AdjustTokenPrivileges(Token, FALSE, &TokenPrivs, 0, NULL, NULL) && ::GetLastError() == ERROR_SUCCESS);

    ::CloseHandle(Token);

    return Result;
}
#endif

// Function to determine if this process should get 32 or 64bit dll injections
// Returns 32 or 64 (or 0 for error)
typedef BOOL(WINAPI* LPFN_ISWOW64PROCESS2) (HANDLE, PUSHORT, PUSHORT);
USHORT ProcessBitness(HANDLE hProcess)
{
    USHORT pProcessMachine;
    USHORT pMachineNative;
    LPFN_ISWOW64PROCESS2 fnIsWow64Process2 = (LPFN_ISWOW64PROCESS2)GetProcAddress(
        GetModuleHandle(TEXT("kernel32")), "IsWow64Process2");

    if (fnIsWow64Process2 != NULL)
    {
        BOOL bRet = fnIsWow64Process2(hProcess, &pProcessMachine, &pMachineNative);
        if (bRet == 0)
        {
            return 0;
        }
        if (pProcessMachine == IMAGE_FILE_MACHINE_UNKNOWN)
        {
            if (pMachineNative == IMAGE_FILE_MACHINE_AMD64)
            {
                return 64;
            }
            if (pMachineNative == IMAGE_FILE_MACHINE_I386)
            {
                return 32;
            }
        }
        else
        {
            if (pProcessMachine == IMAGE_FILE_MACHINE_AMD64)
            {
                return 64;
            }
            if (pProcessMachine == IMAGE_FILE_MACHINE_I386)
            {
                return 32;
            }
        }
    }
    return 0;
}

std::wstring FixDllBitness(std::wstring originalName, USHORT bitness)
{
    std::wstring targetDll =  originalName.substr(0, originalName.length() - 6);
    switch (bitness)
    {
    case 32:
        targetDll = targetDll.append(L"32.dll");
        break;
    case 64:
        targetDll = targetDll.append(L"64.dll");
        break;
    default:
        targetDll = std::wstring(originalName);
    }
    return targetDll;
}

std::string ExtractCommandLine(LPSTR commandline)
{
    std::string cmdline = commandline;
    std::string cmdonly;
    size_t inx;
    if (cmdline._Starts_with("\""))
    {
        inx = cmdline.substr(1).find_first_of("\"") + 2;
        cmdonly = cmdline.substr(0, inx);
    }
    else if (cmdline._Starts_with("\'"))
    {
        inx = cmdline.substr(1).find_first_of("\'") + 2;
        cmdonly = cmdline.substr(0, inx);
    }
    else
    {
        inx = cmdline.find_first_of(" ");
        if (inx != std::string::npos)
        {
            cmdonly = cmdline.substr(0, inx - 1);
        }
        else
            cmdonly = cmdline;
    }
    return cmdonly;
}
std::wstring ExtractCommandLine(LPWSTR commandline)
{
    std::wstring cmdline = commandline;
    std::wstring cmdonly;
    size_t inx;
    if (cmdline._Starts_with(L"\""))
    {
        inx = cmdline.substr(1).find_first_of(L"\"") + 2;
        cmdonly = cmdline.substr(0, inx);
    }
    else if (cmdline._Starts_with(L"\'"))
    {
        inx = cmdline.substr(1).find_first_of(L"\'") + 2;
        cmdonly = cmdline.substr(0, inx);
    }
    else
    {
        inx = cmdline.find_first_of(L" ");
        if (inx != std::wstring::npos)
        {
            cmdonly = cmdline.substr(0, inx - 1);
        }
        else
            cmdonly = cmdline;
    }
    return cmdonly;
}

std::string ExtractArgs(LPSTR commandline)
{
    std::string cmdline = commandline;
    std::string args = commandline;
    size_t inx;
    if (cmdline._Starts_with("\""))
    {
        inx = cmdline.substr(1).find_first_of("\"") + 2;
        if (cmdline.length() > inx)
            args = cmdline.substr(inx);
        else
            args = "";
    }
    else if (cmdline._Starts_with("\'"))
    {
        inx = cmdline.substr(1).find_first_of("\'") + 2;
        if (cmdline.length() > inx)
            args = cmdline.substr(inx);
        else
            args = "";
    }
    else
    {
        inx = cmdline.find_first_of(" ");
        if (inx != std::string::npos)
        {
            args = cmdline.substr(inx);
        }
        else
        {
            args = "";
        }
    }
    if (args.compare(" ") == 0)
        args = "";
    return args;
}
std::wstring ExtractArgs(LPWSTR commandline)
{
    std::wstring cmdline = commandline;
    std::wstring args;
    size_t inx;
    if (cmdline._Starts_with(L"\""))
    {
        inx = cmdline.substr(1).find_first_of(L"\"") + 2;
        if (cmdline.length() > inx)
            args = cmdline.substr(inx);
        else
            args = L"";
    }
    else if (cmdline._Starts_with(L"\'"))
    {
        inx = cmdline.substr(1).find_first_of(L"\'") + 2;
        if (cmdline.length() > inx)
            args = cmdline.substr(inx);
        else
            args = L"";
    }
    else
    {
        inx = cmdline.find_first_of(L" ");
        if (inx != std::wstring::npos)
        {
            args = cmdline.substr(inx);
        }
        else
        {
            args = L"";
        }
    }
    if (args.compare(L" ") == 0)
        args = L"";
    return args;
}

auto CreateProcessImpl = psf::detoured_string_function(&::CreateProcessA, &::CreateProcessW);

BOOL WINAPI CreateProcessWithPsfRunDll(
    [[maybe_unused]] _In_opt_ LPCWSTR applicationName,
    _Inout_opt_ LPWSTR commandLine,
    _In_opt_ LPSECURITY_ATTRIBUTES processAttributes,
    _In_opt_ LPSECURITY_ATTRIBUTES threadAttributes,
    _In_ BOOL inheritHandles,
    _In_ DWORD creationFlags,
    _In_opt_ LPVOID environment,
    _In_opt_ LPCWSTR currentDirectory,
    _In_ LPSTARTUPINFOW startupInfo,
    _Out_ LPPROCESS_INFORMATION processInformation) noexcept try
{
    // Detours should only be calling this function with the intent to execute "rundll32.exe"
    assert(std::wcsstr(applicationName, L"rundll32.exe"));
    assert(std::wcsstr(commandLine, L"rundll32.exe"));

    std::wstring cmdLine = psf::wrun_dll_name;
    cmdLine += (commandLine + 12); // +12 to get to the first space after "rundll32.exe"

#if _DEBUG
    Log(L"\tCreateProcessWithPsfRunDll. \n");
#endif
    return CreateProcessImpl(
        (PackageRootPath() / psf::wrun_dll_name).c_str(),
        cmdLine.data(),
        processAttributes,
        threadAttributes,
        inheritHandles,
        creationFlags,
        environment,
        currentDirectory,
        startupInfo,
        processInformation);
}
catch (...)
{
    ::SetLastError(win32_from_caught_exception());
    return FALSE;
}

template <typename CharT>
using startup_info_t = std::conditional_t<std::is_same_v<CharT, char>, STARTUPINFOA, STARTUPINFOW>;

template <typename CharT>
BOOL WINAPI CreateProcessFixup(
    _In_opt_ const CharT* applicationName,
    _Inout_opt_ CharT* commandLine,
    _In_opt_ LPSECURITY_ATTRIBUTES processAttributes,
    _In_opt_ LPSECURITY_ATTRIBUTES threadAttributes,
    _In_ BOOL inheritHandles,
    _In_ DWORD creationFlags,
    _In_opt_ LPVOID environment,
    _In_opt_ const CharT* currentDirectory,
    _In_ startup_info_t<CharT>* startupInfo,
    _Out_ LPPROCESS_INFORMATION processInformation) noexcept try
{
    DWORD PossiblyModifiedCreationFlags = creationFlags;


    bool skipForce = false;  // exclude out certain processes from forcing to run inside the container, like conhost and maybe cmd and powershell
    
                             ///MyProcThreadAttributeList *fullList =  new MyProcThreadAttributeList(true,true,true);
    MyProcThreadAttributeList *partialList = new MyProcThreadAttributeList(true, true, false);
    ///MyProcThreadAttributeList* protList = new MyProcThreadAttributeList(false, true, true);
    ///MyProcThreadAttributeList* noList = new MyProcThreadAttributeList(false, true, false);
    
    STARTUPINFOEXW startupInfoExW =
    {
        {
        sizeof(startupInfoExW)
        , nullptr // lpReserved
        , nullptr // lpDesktop
        , nullptr // lpTitle
        , 0 // dwX
        , 0 // dwY
        , 0 // dwXSize
        , 0 // swYSize
        , 0 // dwXCountChar
        , 0 // dwYCountChar
        , 0 // dwFillAttribute
        , STARTF_USESHOWWINDOW // dwFlags
        , 0
        }
    };
    STARTUPINFOEXA startupInfoExA =
    {
        {
        sizeof(startupInfoExA)
        , nullptr // lpReserved
        , nullptr // lpDesktop
        , nullptr // lpTitle
        , 0 // dwX
        , 0 // dwY
        , 0 // dwXSize
        , 0 // swYSize
        , 0 // dwXCountChar
        , 0 // dwYCountChar
        , 0 // dwFillAttribute
        , STARTF_USESHOWWINDOW // dwFlags
        , 0 // wShowWindow
        }
    };
    STARTUPINFOEX *MyReplacementStartupInfo = reinterpret_cast<STARTUPINFOEX*>(startupInfo);
    
    if constexpr (psf::is_ansi<CharT>)
    {
        if (findStringIC(commandLine, "conhost")) skipForce = true;
        //else if (findStringIC(commandLine, "cmd.exe")) skipForce = true;
        //else if (findStringIC(commandLine, "powershell.exe")) skipForce = true;
    }
    else
    {
        if (findStringIC(commandLine, L"conhost")) skipForce = true;
        //else if (findStringIC(commandLine, L"cmd.exe")) skipForce = true;
        //else if (findStringIC(commandline, L"powershell.exe")) skipForce = true;
    }
    
    if (!skipForce) 
    {
        if ((creationFlags & EXTENDED_STARTUPINFO_PRESENT) != 0)
        {
            Log(L"\tCreateProcessFixup: Extended StartupInfo present but want to force running inside container. Not implemented yet.");
            // Hopefully it is set to start in the container anyway.
        }
        else
        {
            Log(L"\tCreateProcessFixup: Add Extended StartupInfo to force running inside container.");
            // There are situations where processes jump out of the container and this helps to make them stay within.
            // Both cmd and powershell are such cases.
            PossiblyModifiedCreationFlags |= EXTENDED_STARTUPINFO_PRESENT;
            if constexpr (psf::is_ansi<CharT>)
            {
                STARTUPINFOEXA* si = reinterpret_cast<STARTUPINFOEXA*>(startupInfo);
                startupInfoExA.StartupInfo.cb = sizeof(startupInfoExA);
                startupInfoExA.StartupInfo.cbReserved2 = si->StartupInfo.cbReserved2;
                startupInfoExA.StartupInfo.dwFillAttribute = si->StartupInfo.dwFillAttribute;
                startupInfoExA.StartupInfo.dwFlags = si->StartupInfo.dwFlags;
                startupInfoExA.StartupInfo.dwX = si->StartupInfo.dwX;
                startupInfoExA.StartupInfo.dwXCountChars = si->StartupInfo.dwXCountChars;
                startupInfoExA.StartupInfo.dwXSize = si->StartupInfo.dwXSize;
                startupInfoExA.StartupInfo.dwY = si->StartupInfo.dwY;
                startupInfoExA.StartupInfo.dwYCountChars = si->StartupInfo.dwYCountChars;
                startupInfoExA.StartupInfo.dwYSize = si->StartupInfo.dwYSize;
                startupInfoExA.StartupInfo.hStdError = si->StartupInfo.hStdError;
                startupInfoExA.StartupInfo.hStdInput = si->StartupInfo.hStdInput;
                startupInfoExA.StartupInfo.hStdOutput = si->StartupInfo.hStdOutput;
                startupInfoExA.StartupInfo.lpDesktop = si->StartupInfo.lpDesktop;
                startupInfoExA.StartupInfo.lpReserved = si->StartupInfo.lpReserved;
                startupInfoExA.StartupInfo.lpReserved2 = si->StartupInfo.lpReserved2;
                startupInfoExA.StartupInfo.lpTitle = si->StartupInfo.lpTitle;
                startupInfoExA.StartupInfo.wShowWindow = si->StartupInfo.wShowWindow;
                startupInfoExA.lpAttributeList = partialList->get();
                MyReplacementStartupInfo = reinterpret_cast<STARTUPINFOEX*>(&startupInfoExA);
            }
            else
            {
                STARTUPINFOEXW* si = reinterpret_cast<STARTUPINFOEXW*>(startupInfo);
                startupInfoExW.StartupInfo.cb = sizeof(startupInfoExW);
                startupInfoExW.StartupInfo.cbReserved2 = si->StartupInfo.cbReserved2;
                startupInfoExW.StartupInfo.dwFillAttribute = si->StartupInfo.dwFillAttribute;
                startupInfoExW.StartupInfo.dwFlags = si->StartupInfo.dwFlags;
                startupInfoExW.StartupInfo.dwX = si->StartupInfo.dwX;
                startupInfoExW.StartupInfo.dwXCountChars = si->StartupInfo.dwXCountChars;
                startupInfoExW.StartupInfo.dwXSize = si->StartupInfo.dwXSize;
                startupInfoExW.StartupInfo.dwY = si->StartupInfo.dwY;
                startupInfoExW.StartupInfo.dwYCountChars = si->StartupInfo.dwYCountChars;
                startupInfoExW.StartupInfo.dwYSize = si->StartupInfo.dwYSize;
                startupInfoExW.StartupInfo.hStdError = si->StartupInfo.hStdError;
                startupInfoExW.StartupInfo.hStdInput = si->StartupInfo.hStdInput;
                startupInfoExW.StartupInfo.hStdOutput = si->StartupInfo.hStdOutput;
                startupInfoExW.StartupInfo.lpDesktop = si->StartupInfo.lpDesktop;
                startupInfoExW.StartupInfo.lpReserved = si->StartupInfo.lpReserved;
                startupInfoExW.StartupInfo.lpReserved2 = si->StartupInfo.lpReserved2;
                startupInfoExW.StartupInfo.lpTitle = si->StartupInfo.lpTitle;
                startupInfoExW.StartupInfo.wShowWindow = si->StartupInfo.wShowWindow;
                startupInfoExW.lpAttributeList = partialList->get();
                MyReplacementStartupInfo = reinterpret_cast<STARTUPINFOEX*>(&startupInfoExW);
            }
        }
    }



    // We can't detour child processes whose executables are located outside of the package as they won't have execute
    // access to the fixup dlls. Instead of trying to replicate the executable search logic when determining the location
    // of the target executable, create the process as suspended and let the system tell us where the executable is.
    // The structure below allows us to track the new process, even if the caller didn't specify one.
    PROCESS_INFORMATION pi = { 0 };
    if (!processInformation)
    {
        processInformation = &pi;
    }

    // In order to perform injection, we will force the new process to start suspended, then inject PsfRuntime, and then resume the process.
    PossiblyModifiedCreationFlags |= CREATE_SUSPENDED;

    if (::CreateProcessImpl(
        applicationName,
        commandLine,
        processAttributes,
        threadAttributes,
        inheritHandles,
        PossiblyModifiedCreationFlags,
        environment,
        currentDirectory,
        reinterpret_cast<startup_info_t<CharT>*>(MyReplacementStartupInfo), ///startupInfo,
        processInformation) == FALSE )
    {
        DWORD err = GetLastError();
        if (err == 0x2e4)
        {
            // The app requires elevation, so try it this way.  Not perfect.
            // The better solution is to determine the need during packaging and add
            // an external manifest file to Psflauncher to elevate first.
            if ((creationFlags & EXTENDED_STARTUPINFO_PRESENT) != 0)
            {
                SHELLEXECUTEINFO* shExInfo;
                SHELLEXECUTEINFOA shExInfoA;
                SHELLEXECUTEINFOW shExInfoW;
                std::string cmdA;
                std::string argsA;
                std::wstring cmdW;
                std::wstring argsW;
                if constexpr (psf::is_ansi<CharT>)
                {
                    shExInfoA.cbSize = sizeof(shExInfoA);
                    shExInfoA.fMask = 0x00000040; // SEE_MASK_NOCLOSEPROCESS;
                    shExInfoA.hwnd = 0;
                    shExInfoA.lpVerb = "runas";
                    cmdA = ExtractCommandLine(commandLine);
                    shExInfoA.lpFile = cmdA.c_str();
                    argsA = ExtractArgs(commandLine);
                    shExInfoA.lpParameters = argsA.c_str();
                    shExInfoA.lpDirectory = currentDirectory;
                    shExInfoA.nShow = MyReplacementStartupInfo->StartupInfo.wShowWindow;
                    shExInfoA.hInstApp = 0;
                    shExInfo = reinterpret_cast<SHELLEXECUTEINFO*>(&shExInfoA);
                }
                else
                {
                    shExInfoW.cbSize = sizeof(shExInfoW);
                    shExInfoW.fMask = 0x00000040; // SEE_MASK_NOCLOSEPROCESS;
                    shExInfoW.hwnd = 0;
                    shExInfoW.lpVerb = L"runas";               // Operation to perform
                    cmdW = ExtractCommandLine(commandLine);
                    shExInfoW.lpFile = cmdW.c_str();
                    argsW = ExtractArgs(commandLine);
                    shExInfoW.lpParameters = argsW.c_str();
                    shExInfoW.lpDirectory = currentDirectory;
                    shExInfoW.nShow = MyReplacementStartupInfo->StartupInfo.wShowWindow;
                    shExInfoW.hInstApp = 0;
                    shExInfo = reinterpret_cast<SHELLEXECUTEINFO*>(&shExInfoW);
                }

                BOOL resp;
                if constexpr (psf::is_ansi<CharT>)
                {
                    resp = ::ShellExecuteExA(&shExInfoA);
                }
                else
                {
                    resp = ::ShellExecuteExW(&shExInfoW);
                }
                if (resp)
                {
                    processInformation->dwProcessId = GetProcessId(shExInfo->hProcess);
                    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, processInformation->dwProcessId);
                    if (hSnap != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 lpte;
                        lpte.dwSize = sizeof(lpte);
                        BOOL b = Thread32First(hSnap, &lpte);
                        if (b)
                        {
                            processInformation->dwThreadId = lpte.th32ThreadID;
                            processInformation->hThread = OpenThread(THREAD_SUSPEND_RESUME, false, processInformation->dwThreadId);
                            if (processInformation->hThread != INVALID_HANDLE_VALUE)
                            {
                                DWORD dRet = ::SuspendThread(processInformation->hThread);
                                if (dRet == 0)
                                {
                                    ;
                                }
                            }
                            processInformation->hProcess = shExInfo->hProcess;
                        }
                        CloseHandle(hSnap);
                    }
                }
            }
            else
            {
                // just remove the added startup info
                PossiblyModifiedCreationFlags = creationFlags | CREATE_SUSPENDED;
                if (::CreateProcessImpl(
                    applicationName,
                    commandLine,
                    processAttributes,
                    threadAttributes,
                    inheritHandles,
                    PossiblyModifiedCreationFlags,
                    environment,
                    currentDirectory,
                    startupInfo,
                    processInformation) == FALSE)
                {
                    // looks bad, have no idea why or what to do here if here.
                    err = GetLastError();
                    return FALSE;
                }
            }
        }
        else
        {
            Log(L"\tCreateProcessFixup: Creation returned false trying to force in container without prot 0x%x retry with prot.", err);
            Log(L"\tCreateProcessFixup: pid reported as 0x%x", processInformation->dwProcessId);
            if (processInformation->dwProcessId == 0)
            {
                return FALSE;
            }
        }
#if NONO
        // This is some test code to try starting the process in different ways.  
        // It didn't help and will go away once we are confortable that all of the edge cases are covered.
        PossiblyModifiedCreationFlags = creationFlags | CREATE_SUSPENDED;
        MyReplacementStartupInfo->lpAttributeList = fullList->get();
        if (::CreateProcessImpl(
            applicationName,
            commandLine,
            processAttributes,
            threadAttributes,
            inheritHandles,
            PossiblyModifiedCreationFlags,
            environment,
            currentDirectory,
            reinterpret_cast<startup_info_t<CharT>*>(MyReplacementStartupInfo), ///startupInfo,
            processInformation) == FALSE)
        {
            err = GetLastError();
            Log(L"\tCreateProcessFixup: Creation returned false on retry with prot 0x%x", err);

            MyReplacementStartupInfo->lpAttributeList = protList->get();
            if (::CreateProcessImpl(
                applicationName,
                commandLine,
                processAttributes,
                threadAttributes,
                inheritHandles,
                PossiblyModifiedCreationFlags,
                environment,
                currentDirectory,
                reinterpret_cast<startup_info_t<CharT>*>(MyReplacementStartupInfo), ///startupInfo,
                processInformation) == FALSE)
            {
                err = GetLastError();
                Log(L"\tCreateProcessFixup: Creation returned false on retry with prot no force 0x%x", err);
                MyReplacementStartupInfo->lpAttributeList = noList->get();
                if (::CreateProcessImpl(
                    applicationName,
                    commandLine,
                    processAttributes,
                    threadAttributes,
                    inheritHandles,
                    PossiblyModifiedCreationFlags,
                    environment,
                    currentDirectory,
                    reinterpret_cast<startup_info_t<CharT>*>(MyReplacementStartupInfo), ///startupInfo,
                    processInformation) == FALSE)
                {
                    err = GetLastError();
                    Log(L"\tCreateProcessFixup: Creation returned false on retry without none 0x%x", err);
                    PossiblyModifiedCreationFlags = creationFlags | CREATE_SUSPENDED;
                    if (::CreateProcessImpl(
                        applicationName,
                        commandLine,
                        processAttributes,
                        threadAttributes,
                        false,
                        PossiblyModifiedCreationFlags,
                        environment,
                        currentDirectory,
                        startupInfo,
                        processInformation) == FALSE)
                    {
                        err = GetLastError();
                        Log(L"\tCreateProcessFixup: Creation returned false on retry without extension 0x%x", err);
                        return FALSE;
                    }
                }
            }
        }
#endif

    }
#ifdef _DEBUG
    Log(L"\tCreateProcessFixup: Process has started as suspended, now consider injections...");
#endif
    iwstring path;
    DWORD size = MAX_PATH;
    path.resize(size - 1);
    while (true)
    {
        if (::QueryFullProcessImageNameW(processInformation->hProcess, 0, path.data(), &size))
        {
            path.resize(size);
            break;
        }
        else if (auto err = ::GetLastError(); err == ERROR_INSUFFICIENT_BUFFER)
        {
            size *= 2;
            path.resize(size - 1);
        }
        else
        {
            // Unexpected error
            Log(L"\tCreateProcessFixup: Unable to retrieve process.");
            ::TerminateProcess(processInformation->hProcess, ~0u);
            ::CloseHandle(processInformation->hProcess);
            ::CloseHandle(processInformation->hThread);

            ::SetLastError(err);
            return FALSE;
        }
    }

    // std::filesystem::path comparison doesn't seem to handle case-insensitivity or root-local device paths...
    iwstring_view packagePath(PackageRootPath().native().c_str(), PackageRootPath().native().length());
    iwstring_view finalPackagePath(FinalPackageRootPath().native().c_str(), FinalPackageRootPath().native().length());
    iwstring_view exePath = path;
    auto fixupPath = [](iwstring_view& p)
    {
            if ((p.length() >= 4) && (p.substr(0, 4) == LR"(\\?\)"_isv))
            {
                p = p.substr(4);
            }
    };
    fixupPath(packagePath);
    fixupPath(finalPackagePath);
    fixupPath(exePath);

#if _DEBUG
    Log(L"\tCreateProcessFixup: Possible injection to process %ls %d.\n", exePath.data(), processInformation->dwProcessId);
#endif
    //if (((exePath.length() >= packagePath.length()) && (exePath.substr(0, packagePath.length()) == packagePath)) ||
    //    ((exePath.length() >= finalPackagePath.length()) && (exePath.substr(0, finalPackagePath.length()) == finalPackagePath)))
    // TRM: 2021-10-21 We do want to inject into exe processes that are outside of the package structure, for example PowerShell for a cmd file,
    // TRM: 2021-11-03 but only if the new process is running inside the Container ...
    bool allowInjection = false;

    try
    {
        if ((PossiblyModifiedCreationFlags & EXTENDED_STARTUPINFO_PRESENT) != 0)
        {

#if _DEBUG
            Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute: Has extended Attribute.");
#endif
            if constexpr (psf::is_ansi<CharT>)
            {
#if _DEBUG
                Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute: narrow");
#endif
                STARTUPINFOEXA* si = reinterpret_cast<STARTUPINFOEXA*>(MyReplacementStartupInfo);
                if (si->lpAttributeList != NULL)
                {
#if _DEBUG
                    DumpStartupAttributes(reinterpret_cast<SIH_PROC_THREAD_ATTRIBUTE_LIST*>(si->lpAttributeList));
#endif
                    allowInjection = DoesAttributeSpecifyInside(reinterpret_cast<SIH_PROC_THREAD_ATTRIBUTE_LIST*>(si->lpAttributeList));
                }
                else
                {
#if _DEBUG
                    Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute: attlist is null.");
#endif
                    allowInjection = true;
                }
            }
            else
            {
#if _DEBUG
                Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute:: wide");
#endif
                STARTUPINFOEXW* si = reinterpret_cast<STARTUPINFOEXW*>(MyReplacementStartupInfo);
                if (si->lpAttributeList != NULL)
                {
#if _DEBUG
                    DumpStartupAttributes(reinterpret_cast<SIH_PROC_THREAD_ATTRIBUTE_LIST*>(si->lpAttributeList));
#endif
                    allowInjection = DoesAttributeSpecifyInside(reinterpret_cast<SIH_PROC_THREAD_ATTRIBUTE_LIST*>(si->lpAttributeList));
                }
                else
                {
#if _DEBUG
                    Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute: attlist is null.");
#endif
                    allowInjection = true;
                }
            }
        }
        else
        {
#if _DEBUG
            Log(L"\tCreateProcessFixup: CreateProcessImpl Attribute: Does not have extended attribute and should be added.");
#endif
            allowInjection = true;
        }
    }
    catch (...)
    {
        Log(L"\tCreateProcessFixup: Exception testing for attribute list, assuming none.");
        allowInjection = false;
    }

    if (allowInjection)
    {
        // There are situations where the new process might jump outside of the container to run.
        // In those situations, we can't inject dlls into it.
        try
        {
            BOOL b;
            BOOL res = IsProcessInJob(processInformation->hProcess, nullptr, &b);
            if (res != 0)
            {
                if (b == false)
                {
                    allowInjection = false;
#if _DEBUG
                    Log(L"\tCreateProcessFixup: New process has broken away, do not inject.");
#endif
                }
                else
                {
                    Log(L"\tCreateProcessFixup: New process is in a job, allow.");
                    // NOTE: we could maybe try to see if in the same job, but this is probably good enough.
                }
            }
            else
            {
#if _DEBUG
                Log(L"\tCreateProcessFixup: Unable to detect job status of new process, ignore for now and try to inject 0x%x 0x%x 0x%x.",res, GetLastError(),b);
#endif
            }
        }
        catch (...)
        {
            allowInjection = false;
#if _DEBUG
            Log(L"\tCreateProcessFixup: Exception while trying to determine job status of new process. Do not inject.");
#endif
        }
    }

    if (allowInjection)
    {
        // The target executable is in the package, so we _do_ want to fixup it
#if _DEBUG
        Log(L"\tCreateProcessFixup: Allowed Injection, so yes");
#endif
        // Fix for issue #167: allow subprocess to be a different bitness than this process.
        USHORT bitness = ProcessBitness(processInformation->hProcess);
#if _DEBUG
        Log(L"\tCreateProcessFixup: Injection for PID=%d Bitness=%d", processInformation->dwProcessId, bitness);
#endif  
        std::wstring wtargetDllName = FixDllBitness(std::wstring(psf::runtime_dll_name), bitness);
#if _DEBUG
        Log(L"\tCreateProcessFixup: Use runtime %ls", wtargetDllName.c_str());
#endif
        ///static const auto pathToPsfRuntime = (PackageRootPath() / wtargetDllName.c_str()).string();
        static std::string pathToPsfRuntime;
        if (g_PsfRunTimeModulePath[0] != 0x0)
        {
            std::filesystem::path RuntimePath = g_PsfRunTimeModulePath;
            pathToPsfRuntime = RuntimePath.string();
        }
        else
        {
            pathToPsfRuntime = (PackageRootPath() / wtargetDllName.c_str()).string();
        }
        const char * targetDllPath = NULL;
#if _DEBUG
        Log("\tCreateProcessFixup: Inject %s into PID=%d", pathToPsfRuntime.c_str(), processInformation->dwProcessId);
#endif

        if (std::filesystem::exists(pathToPsfRuntime))
        {
            targetDllPath = pathToPsfRuntime.c_str();
        }
        else
        {
            // Possibly the dll is in the folder with the exe and not at the package root.
            Log(L"\tCreateProcessFixup: %ls not found at package root, try target folder.", wtargetDllName.c_str());

            std::filesystem::path altPathToExeRuntime = exePath.data();
            static const auto altPathToPsfRuntime = (altPathToExeRuntime.parent_path() / pathToPsfRuntime.c_str()).string();
#if _DEBUG
            Log(L"\tCreateProcessFixup: alt target filename is now %s", altPathToPsfRuntime.c_str());
#endif
            if (std::filesystem::exists(altPathToPsfRuntime))
            {
                targetDllPath = altPathToPsfRuntime.c_str();
#if _DEBUG
                Log(L"\tCreateProcessFixup: alt target exists.");
#endif
            }
            else
            {
#if _DEBUG
                Log(L"\tCreateProcessFixup: Not present there either, try elsewhere in package.");
#endif
                // If not in those two locations, must check everywhere in package.
                // The child process might also be in another package folder, so look elsewhere in the package.
                for (auto& dentry : std::filesystem::recursive_directory_iterator(PackageRootPath()))
                {
                    try
                    {
                        if (dentry.path().filename().compare(wtargetDllName) == 0)
                        {
                            static const auto altDirPathToPsfRuntime = narrow(dentry.path().c_str());
#if _DEBUG
                            Log(L"\tCreateProcessFixup: Found match as %ls", dentry.path().c_str());
#endif
                            targetDllPath = altDirPathToPsfRuntime.c_str();
                            break;
                        }
                    }
                    catch (...)
                    {
                        Log(L"\tCreateProcessFixup: Non-fatal error enumerating directories while looking for PsfRuntime.");
                    }
                }

            }
        }

        if (targetDllPath != NULL)
        {
            Log("\tCreateProcessFixup: Attempt injection into %d using %s", processInformation->dwProcessId, targetDllPath);
            if (!::DetourUpdateProcessWithDll(processInformation->hProcess, &targetDllPath, 1))
            {
                Log("\tCreateProcessFixup: %s unable to inject, err=0x%x.", targetDllPath, ::GetLastError());
                if (!::DetourProcessViaHelperDllsW(processInformation->dwProcessId, 1, &targetDllPath, CreateProcessWithPsfRunDll))
                {
                    Log("\tCreateProcessFixup: %s unable to inject with RunDll either (Skipping), err=0x%x.", targetDllPath, ::GetLastError());
                }
            }
            else
            {
                Log(L"\tCreateProcessFixup: Injected %ls into PID=%d\n", wtargetDllName.c_str(), processInformation->dwProcessId);
            }
        }
        else
        {
            Log(L"\tCreateProcessFixup: %ls not found, skipping.", wtargetDllName.c_str());
        }
    }
    else
    {
        Log(L"\tCreateProcessFixup: The new process is not inside the container, so doesn't inject...");
    }
    if ((creationFlags & CREATE_SUSPENDED) != CREATE_SUSPENDED)
    {
        // Caller did not want the process to start suspended
        ::ResumeThread(processInformation->hThread);
        SetLastError(0);
    }

    if (processInformation == &pi)
    {
        // If we created this strucure we must close the handles in it
        ::CloseHandle(processInformation->hProcess);
        ::CloseHandle(processInformation->hThread);
    }

    return TRUE;
}
catch (...)
{
    int err = win32_from_caught_exception();
    Log(L"CreateProcessFixup: exception 0x%x", err);
    ::SetLastError(err);
    return FALSE;
}

DECLARE_STRING_FIXUP(CreateProcessImpl, CreateProcessFixup);

