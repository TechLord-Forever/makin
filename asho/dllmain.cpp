#include "stdafx.h"
#include "Psapi.h"
#include "Shlwapi.h"
#include <TlHelp32.h>
#include <vector>
#include "capstone/capstone.h"
#include "Header.h"

#pragma comment(lib, "Shlwapi.lib")

#define HOOK_COUNT 10 // ?? 10

DWORD64 hooks[HOOK_COUNT]{};
HMODULE copyNtdll{};
TCHAR tmp[MAX_PATH + 2]{};
TCHAR sys[MAX_PATH + 2]{};

typedef unsigned char byte;


VOID hookFunction(const CHAR func[], const DWORD index) {
    const auto lib = LoadLibrary(L"ntdll");
    if (!lib) {
        return;
    }
    auto f_addr = static_cast<LPVOID>(GetProcAddress(lib, func));

	csh handle;
	cs_insn *insn;
	
	if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK)
	{
		const auto count = cs_disasm(handle, static_cast<byte*>(f_addr), 0x50 /* ? */, uint64_t(f_addr), 0, &insn);
		if (count > 1)
		{
			f_addr = LPVOID(insn[1].address);
			cs_free(insn, count);
		}
		cs_close(&handle);
	}

    byte jmp[] = { 0x48, 0xB8, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF, 0xE0 };

    DWORD old;
    VirtualProtectEx(GetCurrentProcess(), f_addr, 100, PAGE_EXECUTE_READWRITE, &old);
    memcpy_s(f_addr, 2, jmp, 2);
    *reinterpret_cast<DWORD64*>(static_cast<byte*>(f_addr) + 2) = static_cast<DWORD64>(hooks[index]);
    memcpy_s(static_cast<byte*>(f_addr) + 10, 2, jmp + 10, 2);


    VirtualProtectEx(GetCurrentProcess(), f_addr, 100, old, &old);
}


// hook functions
LONG WINAPI hookNtClose(_In_ HANDLE Handle)
{
    typedef ULONG xNtQueryObject(
        _In_opt_  HANDLE                   Handle,
        _In_      DWORD ObjectInformationClass, // ObjectTypeInformation = 0x2
        _Out_opt_ PVOID                    ObjectInformation,
        _In_      ULONG                    ObjectInformationLength,
        _Out_opt_ PULONG                   ReturnLength
    );
    typedef LONG WINAPI NtClose(
        _In_ HANDLE Handle
    );
    auto ob = new byte[0x1000]{};
    auto NtQueryObject = reinterpret_cast<xNtQueryObject*>(GetProcAddress(copyNtdll, "NtQueryObject"));
    auto status = NtQueryObject(Handle, 2, ob, 0x1000, nullptr);


    delete[] ob;
    if (status) // invalid handle
    {
        TCHAR msg[0x100]{};
        swprintf_s(msg, L"[NtClose] Invalid HANDLE specified by the debuggee - 0x%llx\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.ii\n", DWORD64(Handle));
        OutputDebugStringW(msg);
        return 1; // return success 
    }
    const auto close = reinterpret_cast<NtClose*>(GetProcAddress(copyNtdll, "NtClose"));
    return close(Handle);

}


LONG NTAPI hookNtOpenProcess(_Out_ PHANDLE ProcessHandle, _In_ ACCESS_MASK DesiredAccess, _In_ LPVOID ObjectAttributes, _In_opt_ PCLIENT_ID ClientId)
{
    typedef LONG xZwOpenProcess(
        _Out_    PHANDLE            ProcessHandle,
        _In_     ACCESS_MASK        DesiredAccess,
        _In_     LPVOID ObjectAttributes,
        _In_opt_ LPVOID         ClientId
    );

    auto xOP = reinterpret_cast<xZwOpenProcess*>(GetProcAddress(copyNtdll, "NtOpenProcess"));

    TCHAR fileName[0x100]{};
    // ProcID
    if (!ClientId)
        return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
    auto procID = DWORD(ClientId->UniqueProcess);
    auto hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);

    PROCESSENTRY32 pe = { sizeof(pe) };
    if (::Process32First(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == procID) {
                _tcscpy_s(fileName, 0x100, pe.szExeFile);
                break;
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);

    if (!_tcscmp(fileName, L"csrss.exe"))
    {
        OutputDebugString(L"[NtOpenProcess] The debuggee attempts to open csrss.exe\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.i\n");
        // return diff value??
    }

    return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

LONG NTAPI hookNtCreateFile(
    _Out_ PHANDLE FileHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _Out_ LPVOID IoStatusBlock,
    _In_opt_ PLARGE_INTEGER AllocationSize,
    _In_ ULONG FileAttributes,
    _In_ ULONG ShareAccess,
    _In_ ULONG CreateDisposition,
    _In_ ULONG CreateOptions,
    _In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
    _In_ ULONG EaLength
)
{
    typedef  LONG NTAPI xZwCreateFile(
        _Out_ PHANDLE FileHandle,
        _In_ ACCESS_MASK DesiredAccess,
        _In_ POBJECT_ATTRIBUTES ObjectAttributes,
        _Out_ LPVOID IoStatusBlock,
        _In_opt_ PLARGE_INTEGER AllocationSize,
        _In_ ULONG FileAttributes,
        _In_ ULONG ShareAccess,
        _In_ ULONG CreateDisposition,
        _In_ ULONG CreateOptions,
        _In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
        _In_ ULONG EaLength
    );

    // is there a better way to remove \??\ ?
    DWORD64 size = ObjectAttributes->ObjectName->MaximumLength - (4 * sizeof(TCHAR));
    TCHAR* fileName = new TCHAR[size]{};
    _tcscpy_s(fileName, size, ObjectAttributes->ObjectName->Buffer + 4);
    TCHAR cName[MAX_PATH + 2]{};
    GetModuleFileNameEx(GetCurrentProcess(), nullptr, cName, MAX_PATH + 2);

    if (!_tcscmp(fileName, cName))
    {
        OutputDebugString(L"[NtCreateFile] The debuggee attempts to open itself exclusively\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.iii\n");
    }

    TCHAR* devName = new TCHAR[ObjectAttributes->ObjectName->MaximumLength]{};
    _tcscpy_s(devName, ObjectAttributes->ObjectName->MaximumLength, ObjectAttributes->ObjectName->Buffer);
    // TODO: add more checks like this
    if (!_tcscmp(devName, L"\\??\\PROCEXP152"))
    {
        OutputDebugString(L"[NtCreateFile] The debuggee attempts to open PROCESS EXPLORER driver [\\??\\PROCEXP152]\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.iii\n");
    }

    // Some debuggers open handle when debuggee loads dll and forget to close handle, we can use this behavior to detect a debugger (not all, but IDA Pro at least)
    // Anti-debugger trick: load a dll via LoadLibrary and try to open the same file for opened for exclusive access.
    TCHAR filePath[0x1000]{};
    _tcscpy_s(filePath, 0x1000, L"[_]");
    _tcscpy_s(filePath + 3, 0x1000 - 3, fileName);

    TCHAR fulltmp[MAX_PATH + 2]{};
    GetFullPathName(tmp, MAX_PATH + 2, fulltmp, nullptr);

    if (_tcscmp(fileName, fulltmp) && _tcscmp(fileName, sys))
        OutputDebugString(filePath);

    delete[] fileName;
    delete[] devName;
    auto oCf = reinterpret_cast<xZwCreateFile*>(GetProcAddress(copyNtdll, "NtCreateFile"));
    return oCf(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

ULONG NTAPI hookNtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State)
{
    UNREFERENCED_PARAMETER(ComponentId);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(State);
    // typedef ULONG __stdcall NtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State);
    // auto xNtSetDebugFilterState = reinterpret_cast<NtSetDebugFilterState*>(GetProcAddress(copyNtdll, "NtSetDebugFilterState"));

    OutputDebugString(L"[NtSetDebugFilterState] The debuggee attempts to use NtSetDebugFilterState trick\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.vi\n");

    // return xNtSetDebugFilterState(ComponentId, Level, State);
    return 1; // fake it ;)
}

LONG WINAPI hookNtQueryInformationProcess(
    _In_      HANDLE           ProcessHandle,
    _In_      PROCESSINFOCLASS ProcessInformationClass,
    _Out_     PVOID            ProcessInformation,
    _In_      ULONG            ProcessInformationLength,
    _Out_opt_ PULONG           ReturnLength
)
{
    typedef LONG WINAPI NtQueryInformationProcess(
        _In_      HANDLE           ProcessHandle,
        _In_      PROCESSINFOCLASS ProcessInformationClass,
        _Out_     PVOID            ProcessInformation,
        _In_      ULONG            ProcessInformationLength,
        _Out_opt_ PULONG           ReturnLength
    );

    const auto oNQi = reinterpret_cast<NtQueryInformationProcess*>(GetProcAddress(copyNtdll, "NtQueryInformationProcess"));

    const auto status = oNQi(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
    if (ProcessInformationClass == ProcessDebugPort)
    {
        OutputDebugString(L"[ProcessDebugPort] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.a\n");
        *static_cast<DWORD64*>(ProcessInformation) = 0; // fake it ;)
    }
    if (ProcessInformationClass == ProcessDebugObjectHandle)
    {
        OutputDebugString(L"[ProcessDebugObjectHandle] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.b\n");
        *static_cast<DWORD64*>(ProcessInformation) = 0; // fake it ;)
    }

    if (ProcessInformationClass == ProcessDebugFlags)
    {
        OutputDebugString(L"[ProcessDebugFlags] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.c\n");
        *static_cast<DWORD*>(ProcessInformation) = 1; // fake it ;)
    }

    return status;
}

LONG WINAPI hookNtQuerySystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength)
{
    typedef LONG WINAPI pNtQuerySystemInformation(
        ULONG SystemInformationClass,
        PVOID SystemInformation,
        ULONG SystemInformationLength,
        PULONG ReturnLength);

    pNtQuerySystemInformation * querySysInfo = (pNtQuerySystemInformation*)GetProcAddress(copyNtdll, "NtQuerySystemInformation");
    const ULONG SystemKernelDebuggerInformation = 0x23;

    auto status = querySysInfo(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);

    if (SystemInformationClass == SystemKernelDebuggerInformation)
    {
        OutputDebugString(L"[SystemKernelDebuggerInformation] The debuggee attempts to detect a kernel debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.E.iii\n");

        SYSTEM_KERNEL_DEBUGGER_INFORMATION* sysKernelInfo = static_cast<SYSTEM_KERNEL_DEBUGGER_INFORMATION*>(SystemInformation);
        sysKernelInfo->KernelDebuggerEnabled = FALSE; // fake it ;)
        sysKernelInfo->KernelDebuggerNotPresent = TRUE;
    }

    return status;
}

LONG hookNtSetInformationThread(
    _In_ HANDLE          ThreadHandle,
    _In_ THREADINFOCLASS ThreadInformationClass,
    _In_ PVOID           ThreadInformation,
    _In_ ULONG           ThreadInformationLength
)
{
    typedef LONG NtSetInformationThread(
        _In_ HANDLE          ThreadHandle,
        _In_ THREADINFOCLASS ThreadInformationClass,
        _In_ PVOID           ThreadInformation,
        _In_ ULONG           ThreadInformationLength
    );
    const auto setInfoThread = reinterpret_cast<NtSetInformationThread*>(GetProcAddress(copyNtdll, "NtSetInformationThread"));

    if (ThreadInformationClass == ThreadHideFromDebugger)
    {
        OutputDebugString(L"[ThreadHideFromDebugger] The debuggee attempts to hide/escape\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.F.iii\n");
        return 1;
    }
    else // .... 
    {
        return setInfoThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);
    }

}

LONG NTAPI hookNtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ulProcessFlags, ULONG ulThreadFlags, PRTL_USER_PROCESS_PARAMETERS RtlUserProcessParameters, LPVOID PsCreateInfo, LPVOID PsAttributeList)
{
    typedef LONG NTAPI xNtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ulProcessFlags, ULONG ulThreadFlags, PRTL_USER_PROCESS_PARAMETERS RtlUserProcessParameters, LPVOID PsCreateInfo, LPVOID PsAttributeList);

    xNtCreateUserProcess* createUserProc = (xNtCreateUserProcess*)GetProcAddress(copyNtdll, "NtCreateUserProcess");

    TCHAR msg[0x1000]{};
    _tcscat_s(msg, L"[NtCreateUserprocess] The debuggee attempts to create a new process [BLOCKED]: ");

    auto tmpvar = new TCHAR[RtlUserProcessParameters->ImagePathName.MaximumLength]{};
    _tcscpy_s(tmpvar, RtlUserProcessParameters->ImagePathName.Length, RtlUserProcessParameters->ImagePathName.Buffer);

    _tcscat_s(msg, tmpvar);
    _tcscat_s(msg, L"\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.G.i\n\n");

    OutputDebugString(msg);

    return STATUS_ACCESS_DENIED; // TODO: better way to deal with a child process

                                 // return createUserProc(ProcessHandle, ThreadHandle, ProcessDesiredAccess, ThreadDesiredAccess, ProcessObjectAttributes, ThreadObjectAttributes, ulProcessFlags, ulThreadFlags, RtlUserProcessParameters, PsCreateInfo, PsAttributeList);
}

LONG NTAPI hookNtCreateThreadEx(
    _Out_ PHANDLE ThreadHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ HANDLE ProcessHandle,
    _In_ PVOID StartRoutine, // PUSER_THREAD_START_ROUTINE
    _In_opt_ PVOID Argument,
    _In_ ULONG CreateFlags, // THREAD_CREATE_FLAGS_*
    _In_opt_ ULONG_PTR ZeroBits,
    _In_opt_ SIZE_T StackSize,
    _In_opt_ SIZE_T MaximumStackSize,
    _In_opt_ PVOID AttributeList
)
{
    typedef LONG NTAPI xNtCreateThreadEx(
        _Out_ PHANDLE ThreadHandle,
        _In_ ACCESS_MASK DesiredAccess,
        _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
        _In_ HANDLE ProcessHandle,
        _In_ PVOID StartRoutine, // PUSER_THREAD_START_ROUTINE
        _In_opt_ PVOID Argument,
        _In_ ULONG CreateFlags, // THREAD_CREATE_FLAGS_*
        _In_opt_ ULONG_PTR ZeroBits,
        _In_opt_ SIZE_T StackSize,
        _In_opt_ SIZE_T MaximumStackSize,
        _In_opt_ PVOID AttributeList
    );
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x00000004

    xNtCreateThreadEx* exThread = (xNtCreateThreadEx*)GetProcAddress(copyNtdll, "NtCreateThreadEx");

    if (CreateFlags == THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
    {
        OutputDebugString(L"[NtCreateThreadEx] The debuggee attempts to use THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER flag to hide from us. [FLAG REMOVED]\n\tref: https://goo.gl/4auRMZ\n");

        CreateFlags ^= THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER;
    }

    return exThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

VOID doWork() {

    GetTempPath(MAX_PATH + 2, tmp);
    _tcscat_s(tmp, L"\\ntdllCopy.dll");
    GetSystemDirectory(sys, MAX_PATH + 2);
    _tcscat_s(sys, L"\\ntdll.dll");
    CopyFile(sys, tmp, FALSE);
    copyNtdll = LoadLibrary(tmp); // for us ;)
    if (!copyNtdll)
        return;

    // prepare hook functions
    hooks[0] = DWORD64(hookNtClose); // CloseHandle, NtClose
    hooks[1] = DWORD64(hookNtOpenProcess); // NtOpenProcess for csrss.exe
    hooks[2] = DWORD64(hookNtCreateFile); // ...
    hooks[3] = DWORD64(hookNtSetDebugFilterState);
    hooks[4] = DWORD64(hookNtQueryInformationProcess);
    hooks[5] = DWORD64(hookNtQuerySystemInformation);
    hooks[6] = DWORD64(hookNtSetInformationThread);
    hooks[7] = DWORD64(hookNtCreateUserProcess);
    hooks[8] = DWORD64(hookNtCreateThreadEx);
    // hook there


    //hookFunction("NtRaiseException", 0); // 
    hookFunction("NtClose", 0);
    hookFunction("NtOpenProcess", 1);
    hookFunction("NtCreateFile", 2);
    hookFunction("NtSetDebugFilterState", 3);
    hookFunction("NtQueryInformationProcess", 4);
    hookFunction("NtQuerySystemInformation", 5);
    hookFunction("NtSetInformationThread", 6);
    hookFunction("NtCreateUserProcess", 7);
    hookFunction("NtCreateThreadEx", 8);
    // hey...
    OutputDebugString(L"ImDoneHere");
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    UNREFERENCED_PARAMETER(hModule);
    UNREFERENCED_PARAMETER(lpReserved);

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // MessageBoxA(0, "injected", 0, 0);
        doWork();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

