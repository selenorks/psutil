/*
 * Copyright (c) 2009, Jay Loden, Giampaolo Rodola'. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Helper functions related to fetching process information. Used by
 * _psutil_windows module methods.
 */

#include <Python.h>
#include <windows.h>
#include <Psapi.h>
#include <tlhelp32.h>

#include "security.h"
#include "process_info.h"
#include "ntextapi.h"
#include "../../_psutil_common.h"


/*
 * A wrapper around OpenProcess setting NSP exception if process
 * no longer exists.
 * "pid" is the process pid, "dwDesiredAccess" is the first argument
 * exptected by OpenProcess.
 * Return a process handle or NULL.
 */
HANDLE
psutil_handle_from_pid_waccess(DWORD pid, DWORD dwDesiredAccess) {
    HANDLE hProcess;
    DWORD processExitCode = 0;

    if (pid == 0) {
        // otherwise we'd get NoSuchProcess
        return AccessDenied();
    }

    hProcess = OpenProcess(dwDesiredAccess, FALSE, pid);
    if (hProcess == NULL) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            NoSuchProcess();
        else
            PyErr_SetFromWindowsErr(0);
        return NULL;
    }

    // make sure the process is running
    GetExitCodeProcess(hProcess, &processExitCode);
    if (processExitCode == 0) {
        NoSuchProcess();
        CloseHandle(hProcess);
        return NULL;
    }
    return hProcess;
}


/*
 * Same as psutil_handle_from_pid_waccess but implicitly uses
 * PROCESS_QUERY_INFORMATION | PROCESS_VM_READ as dwDesiredAccess
 * parameter for OpenProcess.
 */
HANDLE
psutil_handle_from_pid(DWORD pid) {
    DWORD dwDesiredAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    return psutil_handle_from_pid_waccess(pid, dwDesiredAccess);
}


// fetch the PEB base address from NtQueryInformationProcess()
PVOID
psutil_get_peb_address(HANDLE ProcessHandle) {
    _NtQueryInformationProcess NtQueryInformationProcess =
        (_NtQueryInformationProcess)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi;

    NtQueryInformationProcess(ProcessHandle, 0, &pbi, sizeof(pbi), NULL);
    return pbi.PebBaseAddress;
}


DWORD *
psutil_get_pids(DWORD *numberOfReturnedPIDs) {
    // Win32 SDK says the only way to know if our process array
    // wasn't large enough is to check the returned size and make
    // sure that it doesn't match the size of the array.
    // If it does we allocate a larger array and try again

    // Stores the actual array
    DWORD *procArray = NULL;
    DWORD procArrayByteSz;
    int procArraySz = 0;

    // Stores the byte size of the returned array from enumprocesses
    DWORD enumReturnSz = 0;

    do {
        procArraySz += 1024;
        free(procArray);
        procArrayByteSz = procArraySz * sizeof(DWORD);
        procArray = malloc(procArrayByteSz);
        if (procArray == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        if (! EnumProcesses(procArray, procArrayByteSz, &enumReturnSz)) {
            free(procArray);
            PyErr_SetFromWindowsErr(0);
            return NULL;
        }
    } while (enumReturnSz == procArraySz * sizeof(DWORD));

    // The number of elements is the returned size / size of each element
    *numberOfReturnedPIDs = enumReturnSz / sizeof(DWORD);

    return procArray;
}


int
psutil_pid_is_running(DWORD pid) {
    HANDLE hProcess;
    DWORD exitCode;

    // Special case for PID 0 System Idle Process
    if (pid == 0)
        return 1;
    if (pid < 0)
        return 0;

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid);
    if (NULL == hProcess) {
        // invalid parameter is no such process
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            CloseHandle(hProcess);
            return 0;
        }

        // access denied obviously means there's a process to deny access to...
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            CloseHandle(hProcess);
            return 1;
        }

        CloseHandle(hProcess);
        PyErr_SetFromWindowsErr(0);
        return -1;
    }

    if (GetExitCodeProcess(hProcess, &exitCode)) {
        CloseHandle(hProcess);
        return (exitCode == STILL_ACTIVE);
    }

    // access denied means there's a process there so we'll assume
    // it's running
    if (GetLastError() == ERROR_ACCESS_DENIED) {
        CloseHandle(hProcess);
        return 1;
    }

    PyErr_SetFromWindowsErr(0);
    CloseHandle(hProcess);
    return -1;
}


int
psutil_pid_in_proclist(DWORD pid) {
    DWORD *proclist = NULL;
    DWORD numberOfReturnedPIDs;
    DWORD i;

    proclist = psutil_get_pids(&numberOfReturnedPIDs);
    if (proclist == NULL)
        return -1;
    for (i = 0; i < numberOfReturnedPIDs; i++) {
        if (pid == proclist[i]) {
            free(proclist);
            return 1;
        }
    }

    free(proclist);
    return 0;
}


// Check exit code from a process handle. Return FALSE on an error also
// XXX - not used anymore
int
handlep_is_running(HANDLE hProcess) {
    DWORD dwCode;

    if (NULL == hProcess)
        return 0;
    if (GetExitCodeProcess(hProcess, &dwCode)) {
        if (dwCode == STILL_ACTIVE)
            return 1;
    }
    return 0;
}


/*
 * returns a Python list representing the arguments for the process
 * with given pid or NULL on error.
 */
PyObject *
psutil_get_cmdline(long pid) {
    int nArgs, i;
    LPWSTR *szArglist = NULL;
    HANDLE hProcess = NULL;
    PVOID pebAddress;
    PVOID rtlUserProcParamsAddress;
    UNICODE_STRING commandLine;
    WCHAR *commandLineContents = NULL;
    PyObject *py_unicode = NULL;
    PyObject *py_retlist = NULL;

    hProcess = psutil_handle_from_pid(pid);
    if (hProcess == NULL)
        return NULL;
    pebAddress = psutil_get_peb_address(hProcess);

    // get the address of ProcessParameters
#ifdef _WIN64
    if (!ReadProcessMemory(hProcess, (PCHAR)pebAddress + 32,
                           &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#else
    if (!ReadProcessMemory(hProcess, (PCHAR)pebAddress + 0x10,
                           &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#endif
    {
        ////printf("Could not read the address of ProcessParameters!\n");
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    // read the CommandLine UNICODE_STRING structure
#ifdef _WIN64
    if (!ReadProcessMemory(hProcess, (PCHAR)rtlUserProcParamsAddress + 112,
                           &commandLine, sizeof(commandLine), NULL))
#else
    if (!ReadProcessMemory(hProcess, (PCHAR)rtlUserProcParamsAddress + 0x40,
                           &commandLine, sizeof(commandLine), NULL))
#endif
    {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }


    // allocate memory to hold the command line
    commandLineContents = (WCHAR *)malloc(commandLine.Length + 1);
    if (commandLineContents == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    // read the command line
    if (!ReadProcessMemory(hProcess, commandLine.Buffer,
                           commandLineContents, commandLine.Length, NULL))
    {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }

    // Null-terminate the string to prevent wcslen from returning
    // incorrect length the length specifier is in characters, but
    // commandLine.Length is in bytes.
    commandLineContents[(commandLine.Length / sizeof(WCHAR))] = '\0';

    // attempt to parse the command line using Win32 API, fall back
    // on string cmdline version otherwise
    szArglist = CommandLineToArgvW(commandLineContents, &nArgs);
    if (szArglist == NULL) {
        PyErr_SetFromWindowsErr(0);
        goto error;
    }
    else {
        // arglist parsed as array of UNICODE_STRING, so convert each to
        // Python string object and add to arg list
        py_retlist = Py_BuildValue("[]");
        if (py_retlist == NULL)
            goto error;
        for (i = 0; i < nArgs; i++) {
            py_unicode = PyUnicode_FromWideChar(
                szArglist[i], wcslen(szArglist[i]));
            if (py_unicode == NULL)
                goto error;
            if (PyList_Append(py_retlist, py_unicode))
                goto error;
            Py_XDECREF(py_unicode);
        }
    }

    if (szArglist != NULL)
        LocalFree(szArglist);
    free(commandLineContents);
    CloseHandle(hProcess);
    return py_retlist;

error:
    Py_XDECREF(py_unicode);
    Py_XDECREF(py_retlist);
    if (hProcess != NULL)
        CloseHandle(hProcess);
    if (commandLineContents != NULL)
        free(commandLineContents);
    if (szArglist != NULL)
        LocalFree(szArglist);
    return NULL;
}


PyObject *psutil_get_cwd(long pid) {
    HANDLE processHandle = NULL;
    PVOID pebAddress;
    PVOID rtlUserProcParamsAddress;
    UNICODE_STRING currentDirectory;
    WCHAR *currentDirectoryContent = NULL;
    PyObject *py_unicode = NULL;

    processHandle = psutil_handle_from_pid(pid);
    if (processHandle == NULL)
        return NULL;

    pebAddress = psutil_get_peb_address(processHandle);

    // get the address of ProcessParameters
#ifdef _WIN64
    if (!ReadProcessMemory(processHandle, (PCHAR)pebAddress + 32,
                           &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#else
    if (!ReadProcessMemory(processHandle, (PCHAR)pebAddress + 0x10,
                           &rtlUserProcParamsAddress, sizeof(PVOID), NULL))
#endif
    {
        CloseHandle(processHandle);
        if (GetLastError() == ERROR_PARTIAL_COPY) {
            // this occurs quite often with system processes
            return AccessDenied();
        }
        else {
            return PyErr_SetFromWindowsErr(0);
        }
    }

    // Read the currentDirectory UNICODE_STRING structure.
    // 0x24 refers to "CurrentDirectoryPath" of RTL_USER_PROCESS_PARAMETERS
    // structure, see:
    // http://wj32.wordpress.com/2009/01/24/
    //     howto-get-the-command-line-of-processes/
#ifdef _WIN64
    if (!ReadProcessMemory(processHandle, (PCHAR)rtlUserProcParamsAddress + 56,
                           &currentDirectory, sizeof(currentDirectory), NULL))
#else
    if (!ReadProcessMemory(processHandle,
                           (PCHAR)rtlUserProcParamsAddress + 0x24,
                           &currentDirectory, sizeof(currentDirectory), NULL))
#endif
    {
        CloseHandle(processHandle);
        if (GetLastError() == ERROR_PARTIAL_COPY) {
            // this occurs quite often with system processes
            return AccessDenied();
        }
        else {
            return PyErr_SetFromWindowsErr(0);
        }
    }

    // allocate memory to hold cwd
    currentDirectoryContent = (WCHAR *)malloc(currentDirectory.Length + 1);
    if (currentDirectoryContent == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    // read cwd
    if (!ReadProcessMemory(processHandle, currentDirectory.Buffer,
                           currentDirectoryContent, currentDirectory.Length,
                           NULL))
    {
        if (GetLastError() == ERROR_PARTIAL_COPY) {
            // this occurs quite often with system processes
            AccessDenied();
        }
        else {
            PyErr_SetFromWindowsErr(0);
        }
        goto error;
    }

    // null-terminate the string to prevent wcslen from returning
    // incorrect length the length specifier is in characters, but
    // currentDirectory.Length is in bytes
    currentDirectoryContent[(currentDirectory.Length / sizeof(WCHAR))] = '\0';

    // convert wchar array to a Python unicode string
    py_unicode = PyUnicode_FromWideChar(
        currentDirectoryContent, wcslen(currentDirectoryContent));
    if (py_unicode == NULL)
        goto error;
    CloseHandle(processHandle);
    free(currentDirectoryContent);
    return py_unicode;

error:
    Py_XDECREF(py_unicode);
    if (currentDirectoryContent != NULL)
        free(currentDirectoryContent);
    if (processHandle != NULL)
        CloseHandle(processHandle);
    return NULL;
}


#define PH_FIRST_PROCESS(Processes) ((PSYSTEM_PROCESS_INFORMATION)(Processes))
#define PH_NEXT_PROCESS(Process) ( \
   ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset ? \
   (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(Process) + \
        ((PSYSTEM_PROCESS_INFORMATION)(Process))->NextEntryOffset) : \
   NULL)

const int STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
const int STATUS_BUFFER_TOO_SMALL = 0xC0000023L;

/*
 * Given a process PID and a PSYSTEM_PROCESS_INFORMATION structure
 * fills the structure with various process information by using
 * NtQuerySystemInformation.
 * We use this as a fallback when faster functions fail with access
 * denied. This is slower because it iterates over all processes.
 * On success return 1, else 0 with Python exception already set.
 */
int
psutil_get_proc_info(DWORD pid, PSYSTEM_PROCESS_INFORMATION *retProcess,
                     PVOID *retBuffer) {
    static ULONG initialBufferSize = 0x4000;
    NTSTATUS status;
    PVOID buffer;
    ULONG bufferSize;
    PSYSTEM_PROCESS_INFORMATION process;

    // get NtQuerySystemInformation
    typedef DWORD (_stdcall * NTQSI_PROC) (int, PVOID, ULONG, PULONG);
    NTQSI_PROC NtQuerySystemInformation;
    HINSTANCE hNtDll;
    hNtDll = LoadLibrary(TEXT("ntdll.dll"));
    NtQuerySystemInformation = (NTQSI_PROC)GetProcAddress(
        hNtDll, "NtQuerySystemInformation");

    bufferSize = initialBufferSize;
    buffer = malloc(bufferSize);
    if (buffer == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    while (TRUE) {
        status = NtQuerySystemInformation(SystemProcessInformation, buffer,
                                          bufferSize, &bufferSize);

        if (status == STATUS_BUFFER_TOO_SMALL ||
                status == STATUS_INFO_LENGTH_MISMATCH)
        {
            free(buffer);
            buffer = malloc(bufferSize);
            if (buffer == NULL) {
                PyErr_NoMemory();
                goto error;
            }
        }
        else {
            break;
        }
    }

    if (status != 0) {
        PyErr_Format(PyExc_RuntimeError, "NtQuerySystemInformation() failed");
        goto error;
    }

    if (bufferSize <= 0x20000)
        initialBufferSize = bufferSize;

    process = PH_FIRST_PROCESS(buffer);
    do {
        if (process->UniqueProcessId == (HANDLE)pid) {
            *retProcess = process;
            *retBuffer = buffer;
            return 1;
        }
    } while ( (process = PH_NEXT_PROCESS(process)) );

    NoSuchProcess();
    goto error;

error:
    FreeLibrary(hNtDll);
    if (buffer != NULL)
        free(buffer);
    return 0;
}
