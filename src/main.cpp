#include "common.h"
#include "gui.h"
#include "lock_finder.h"
#include <iostream>
#include <shellapi.h>

namespace HandsOff {

namespace {

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
std::mutex g_serviceStopMutex;
HANDLE g_hServiceStopEvent = nullptr;

bool WaitPipeOverlapped(HANDLE hPipe, OVERLAPPED& ov, DWORD timeoutMs) {
    HANDLE waits[2] = { ov.hEvent, g_hServiceStopEvent };
    DWORD waitCount = g_hServiceStopEvent ? 2 : 1;
    if (WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs) == WAIT_OBJECT_0) {
        return true;
    }
    CancelIoEx(hPipe, &ov);
    DWORD dummy = 0;
    GetOverlappedResult(hPipe, &ov, &dummy, TRUE);
    return false;
}

bool PipeReadOverlapped(HANDLE hPipe, HANDLE hEvent, void* buf, DWORD size, DWORD* outBytes) {
    ResetEvent(hEvent);
    OVERLAPPED ov = {};
    ov.hEvent = hEvent;
    if (!ReadFile(hPipe, buf, size, nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (!WaitPipeOverlapped(hPipe, ov, 5000)) return false;
    }
    return GetOverlappedResult(hPipe, &ov, outBytes, FALSE) != FALSE;
}

bool PipeWriteOverlapped(HANDLE hPipe, HANDLE hEvent, const void* buf, DWORD size, DWORD* outBytes) {
    ResetEvent(hEvent);
    OVERLAPPED ov = {};
    ov.hEvent = hEvent;
    if (!WriteFile(hPipe, buf, size, nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (!WaitPipeOverlapped(hPipe, ov, 5000)) return false;
    }
    return GetOverlappedResult(hPipe, &ov, outBytes, FALSE) != FALSE;
}

void RunOnDemandPipeServer() {
    SECURITY_DESCRIPTOR sd = {};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };

    HANDLE hPipe = CreateNamedPipeW(
        kServicePipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        4, sizeof(ElevationUnlockResponse), sizeof(ElevationUnlockRequest), 500, &sa);
    if (hPipe == INVALID_HANDLE_VALUE) return;

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent) {
        CloseHandle(hPipe);
        return;
    }

    OVERLAPPED ov = {};
    ov.hEvent = hEvent;

    bool connected = false;
    if (ConnectNamedPipe(hPipe, &ov)) {
        connected = true;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING && WaitPipeOverlapped(hPipe, ov, 5000)) {
            DWORD dummy = 0;
            connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE) != FALSE;
        }
    }

    if (connected) {
        ElevationUnlockRequest req = {};
        DWORD bytesRead = 0;
        if (PipeReadOverlapped(hPipe, hEvent, &req, sizeof(req), &bytesRead) &&
            bytesRead == sizeof(req) &&
            req.magic == 0x484F4646)
        {
            req.targetPath[1023] = L'\0';
            uint32_t closed = 0, killed = 0;
            bool ok = LockFinder::UnlockTarget(
                req.targetPath, {}, static_cast<UnlockMode>(req.mode), &closed, &killed, false);

            ElevationUnlockResponse resp = {
                ok ? 1u : 0u, closed, killed, ok ? 0u : GetLastError()
            };
            DWORD bytesWritten = 0;
            if (PipeWriteOverlapped(hPipe, hEvent, &resp, sizeof(resp), &bytesWritten)) {
                FlushFileBuffers(hPipe);
            }
        }
        DisconnectNamedPipe(hPipe);
    }
    CloseHandle(hEvent);
    CloseHandle(hPipe);
}

VOID WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    if (ctrlCode == SERVICE_CONTROL_STOP || ctrlCode == SERVICE_CONTROL_SHUTDOWN) {
        g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        std::lock_guard<std::mutex> lock(g_serviceStopMutex);
        if (g_hServiceStopEvent) SetEvent(g_hServiceStopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_serviceStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, &ServiceCtrlHandler);
    if (!g_serviceStatusHandle) return;

    {
        std::lock_guard<std::mutex> lock(g_serviceStopMutex);
        g_hServiceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    RunOnDemandPipeServer();

    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    std::lock_guard<std::mutex> lock(g_serviceStopMutex);
    if (g_hServiceStopEvent) {
        CloseHandle(g_hServiceStopEvent);
        g_hServiceStopEvent = nullptr;
    }
}

template <typename Callback>
void ForEachExplorerPid(Callback&& onPid) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                if (!onPid(pe.th32ProcessID)) break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

HMODULE FindHandsOffModuleInPid(DWORD pid) {
    HMODULE hFound = nullptr;
    HANDLE hModSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hModSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        if (Module32FirstW(hModSnap, &me)) {
            do {
                if (_wcsicmp(me.szModule, L"HandsOff.dll") == 0) {
                    hFound = me.hModule;
                    break;
                }
            } while (Module32NextW(hModSnap, &me));
        }
        CloseHandle(hModSnap);
    }
    return hFound;
}

bool CheckDllLoadedInExplorer(DWORD* outExplorerPid) {
    if (outExplorerPid) *outExplorerPid = 0;
    bool isLoaded = false;

    ForEachExplorerPid([&](DWORD pid) {
        if (outExplorerPid && *outExplorerPid == 0) *outExplorerPid = pid;
        if (FindHandsOffModuleInPid(pid) != nullptr) {
            isLoaded = true;
            if (outExplorerPid) *outExplorerPid = pid;
            return false;
        }
        return true;
    });
    return isLoaded;
}

bool CheckComRegistered() {
    std::wstring subKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr + L"\\InprocServer32";
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return true;
        }
    }
    return false;
}

bool HotInjectIntoRunningExplorer(const std::wstring& dllPath) {
    bool anyInjected = false;
    size_t byteSize = (dllPath.size() + 1) * sizeof(wchar_t);
    auto pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!pLoadLib) return false;

    ForEachExplorerPid([&](DWORD pid) {
        if (FindHandsOffModuleInPid(pid) != nullptr) {
            anyInjected = true;
            return true;
        }
        HANDLE hProc = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE, pid);
        if (hProc) {
            if (void* remoteMem = VirtualAllocEx(hProc, nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
                if (WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), byteSize, nullptr)) {
                    if (HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteMem, 0, nullptr)) {
                        WaitForSingleObject(hThread, 2000);
                        CloseHandle(hThread);
                        anyInjected = true;
                    }
                }
                VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
            }
            CloseHandle(hProc);
        }
        return true;
    });
    return anyInjected;
}

void HotUnloadFromRunningExplorer() {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, kUnloadEventName);
    if (!hEvent) return;
    SetEvent(hEvent);
    for (int i = 0; i < 40 && CheckDllLoadedInExplorer(nullptr); ++i) {
        Sleep(20);
    }
    ResetEvent(hEvent);
    CloseHandle(hEvent);

    if (auto pFreeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary")))
    {
        ForEachExplorerPid([&](DWORD pid) {
            for (int attempt = 0; attempt < 4; ++attempt) {
                HMODULE hRemoteMod = FindHandsOffModuleInPid(pid);
                if (!hRemoteMod) break;
                HANDLE hProc = OpenProcess(
                    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                    FALSE, pid);
                if (!hProc) break;
                HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pFreeLib, hRemoteMod, 0, nullptr);
                if (hThread) {
                    WaitForSingleObject(hThread, 1000);
                    CloseHandle(hThread);
                }
                CloseHandle(hProc);
                if (!hThread) break;
            }
            return true;
        });
    }
}

bool RegisterOnDemandService() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --service";

    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hScm) return false;

    SC_HANDLE hSvc = OpenServiceW(hScm, kServiceName, SERVICE_CHANGE_CONFIG);
    if (hSvc) {
        ChangeServiceConfigW(hSvc, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                             SERVICE_ERROR_IGNORE, cmdLine.c_str(),
                             nullptr, nullptr, nullptr, nullptr, nullptr, kServiceDisplay);
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hScm);
        return true;
    }

    hSvc = CreateServiceW(hScm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
                          SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                          cmdLine.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    bool ok = (hSvc != nullptr);
    if (hSvc) CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ok;
}

void UnregisterOnDemandService() {
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) return;
    if (SC_HANDLE hSvc = OpenServiceW(hScm, kServiceName, SERVICE_STOP | DELETE)) {
        SERVICE_STATUS st = {};
        ControlService(hSvc, SERVICE_CONTROL_STOP, &st);
        DeleteService(hSvc);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hScm);
}

int CmdScan(const std::wstring& targetPath) {
    DWORD t0 = GetTickCount();
    auto locks = LockFinder::FindLocks(targetPath, true);
    DWORD elapsed = GetTickCount() - t0;

    std::wcout << L"Target: " << targetPath << L"\n"
               << L"Scan time: " << elapsed << L" ms\n";
    if (locks.empty()) {
        std::wcout << L"Result: No locking processes found.\n";
        return 0;
    }

    std::wcout << L"Summary for Explorer Dialog: \"" << LockFinder::FormatBlockerSummary(locks) << L"\"\n"
               << L"Blocking Processes (" << locks.size() << L"):\n";
    for (const auto& l : locks) {
        std::wcout << L"  - PID " << l.pid << L" | " << l.processName
                   << L" (" << l.appDescription << L")"
                   << L" | Handles: " << l.handleValues.size();
        if (!l.lockedRelativePath.empty()) std::wcout << L" | SubItem: " << l.lockedRelativePath;
        if (l.isSystemCritical) std::wcout << L" [SYSTEM-CRITICAL]";
        if (l.isSelfExplorer)   std::wcout << L" [EXPLORER-SELF]";
        std::wcout << L"\n";
    }
    return 0;
}

int CmdUnlock(const std::wstring& targetPath, UnlockMode mode) {
    auto locks = LockFinder::FindLocks(targetPath, true);
    if (locks.empty()) {
        std::wcout << L"No locks found on: " << targetPath << L"\n";
        return 0;
    }

    uint32_t closedHandles = 0, killedProcs = 0;
    bool ok = LockFinder::UnlockTarget(targetPath, locks, mode, &closedHandles, &killedProcs);
    std::wcout << (ok ? L"[OK] Unlocked target" : L"[WARN] Partial or failed unlock")
               << L" | Closed Handles: " << closedHandles
               << L" | Terminated Processes: " << killedProcs << L"\n";
    return ok ? 0 : 1;
}

void PrintUsage() {
    std::wcout << L"HandsOff - Native Windows 10/11 File & Folder Unlocker\n"
               << L"Usage:\n"
               << L"  HandsOff.exe                  Launch GUI Manager\n"
               << L"  HandsOff.exe install          Install & hot-activate in Explorer\n"
               << L"  HandsOff.exe uninstall        Hot-unload & remove registrations\n"
               << L"  HandsOff.exe scan <path>      Inspect processes locking a file/folder\n"
               << L"  HandsOff.exe unlock <path>    Close locking handles without killing\n"
               << L"  HandsOff.exe kill <path>      Terminate locking apps & close handles\n";
}

void AttachToParentTerminalIfPresent() {
    auto isRedirected = [](DWORD stdHandleId) {
        HANDLE h = GetStdHandle(stdHandleId);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    bool outRedir = isRedirected(STD_OUTPUT_HANDLE);
    bool errRedir = isRedirected(STD_ERROR_HANDLE);
    if ((!outRedir || !errRedir) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        if (!outRedir) freopen_s(&fp, "CONOUT$", "w", stdout);
        if (!errRedir) freopen_s(&fp, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    std::cout.clear();
    std::cerr.clear();
    std::wcout.clear();
    std::wcerr.clear();
}

} // namespace

PatchStatusInfo QueryPatchStatus() {
    PatchStatusInfo info;
    info.isComRegistered = CheckComRegistered();
    info.isLoadedInExplorer = CheckDllLoadedInExplorer(&info.explorerPid);
    return info;
}

PatchOperationResult ApplyPatch() {
    PatchOperationResult res;
    std::wstring dllPath = GetSiblingDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return { false, L"HandsOff.dll not found." };
    }

    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) return { false, L"Failed to load HandsOff.dll." };

    auto pReg = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(hDll, "DllRegisterServer"));
    HRESULT hr = pReg ? pReg() : E_FAIL;
    FreeLibrary(hDll);
    if (FAILED(hr)) return { false, L"COM registration failed." };

    RegisterOnDemandService();
    bool injected = HotInjectIntoRunningExplorer(dllPath);
    Sleep(60);

    return { true, injected ? L"Applied in File Explorer." : L"Registered. Restart Explorer to activate." };
}

PatchOperationResult RollbackPatch() {
    HotUnloadFromRunningExplorer();

    std::wstring dllPath = GetSiblingDllPath();
    if (HMODULE hDll = LoadLibraryW(dllPath.c_str())) {
        if (auto pUnreg = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(hDll, "DllUnregisterServer"))) {
            pUnreg();
        }
        FreeLibrary(hDll);
    }

    UnregisterOnDemandService();
    Sleep(40);
    return { true, L"Uninstalled cleanly." };
}

} // namespace HandsOff

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (!argv || argc < 2) {
        if (argv) LocalFree(argv);
        return HandsOff::RunGuiWindow(hInstance);
    }

    if (_wcsicmp(argv[1], L"--service") == 0) {
        LocalFree(argv);
        SERVICE_TABLE_ENTRYW table[] = {
            { const_cast<LPWSTR>(HandsOff::kServiceName), &HandsOff::ServiceMain },
            { nullptr, nullptr }
        };
        StartServiceCtrlDispatcherW(table);
        return 0;
    }

    if (_wcsicmp(argv[1], L"gui") == 0) {
        LocalFree(argv);
        return HandsOff::RunGuiWindow(hInstance);
    }

    HandsOff::AttachToParentTerminalIfPresent();

    std::wstring cmd = argv[1];
    int exitCode = 0;

    if (_wcsicmp(cmd.c_str(), L"install") == 0) {
        auto res = HandsOff::ApplyPatch();
        std::wcout << (res.success ? L"[OK] " : L"[ERROR] ") << res.statusMessage << L"\n";
        exitCode = res.success ? 0 : 1;
    } else if (_wcsicmp(cmd.c_str(), L"uninstall") == 0) {
        auto res = HandsOff::RollbackPatch();
        std::wcout << (res.success ? L"[OK] " : L"[ERROR] ") << res.statusMessage << L"\n";
        exitCode = res.success ? 0 : 1;
    } else if (_wcsicmp(cmd.c_str(), L"scan") == 0 && argc >= 3) {
        exitCode = HandsOff::CmdScan(argv[2]);
    } else if (_wcsicmp(cmd.c_str(), L"unlock") == 0 && argc >= 3) {
        exitCode = HandsOff::CmdUnlock(argv[2], HandsOff::UnlockMode::CloseHandlesOnly);
    } else if (_wcsicmp(cmd.c_str(), L"kill") == 0 && argc >= 3) {
        exitCode = HandsOff::CmdUnlock(argv[2], HandsOff::UnlockMode::KillProcessAndCloseHandles);
    } else {
        HandsOff::PrintUsage();
        exitCode = 1;
    }

    LocalFree(argv);
    return exitCode;
}
