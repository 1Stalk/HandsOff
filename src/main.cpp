#include "common.h"
#include "gui.h"
#include <shellapi.h>

namespace HandsOff {

namespace {

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
SlimMutex g_serviceStopMutex;
HANDLE g_hServiceStopEvent = nullptr;
bool g_consoleAttached = false;

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
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
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
            bool ok = false;
            std::wstring dllPath = GetSiblingDllPath();
            if (HMODULE hDll = LoadLibraryW(dllPath.c_str())) {
                using PFN_HandsOffServiceUnlock = BOOL(STDAPICALLTYPE*)(LPCWSTR, uint32_t, uint32_t*, uint32_t*);
                if (auto pUnlock = reinterpret_cast<PFN_HandsOffServiceUnlock>(
                        GetProcAddress(hDll, "HandsOffServiceUnlock")))
                {
                    ok = (pUnlock(req.targetPath, req.mode, &closed, &killed) != FALSE);
                }
                FreeLibrary(hDll);
            }

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
        SlimLockGuard lock(g_serviceStopMutex);
        if (g_hServiceStopEvent) SetEvent(g_hServiceStopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_serviceStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, &ServiceCtrlHandler);
    if (!g_serviceStatusHandle) return;

    {
        SlimLockGuard lock(g_serviceStopMutex);
        g_hServiceStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    RunOnDemandPipeServer();

    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    SlimLockGuard lock(g_serviceStopMutex);
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

bool CheckDllLoadedInExplorer(DWORD* outExplorerPid, bool* outAllExplorersLoaded = nullptr) {
    if (outExplorerPid) *outExplorerPid = 0;
    if (outAllExplorersLoaded) *outAllExplorersLoaded = true;

    DWORD shellPid = 0;
    if (HWND hShell = GetShellWindow()) {
        GetWindowThreadProcessId(hShell, &shellPid);
    }

    bool anyLoaded = false;
    bool allLoaded = true;
    DWORD firstExplorerPid = 0;
    DWORD firstLoadedPid = 0;
    bool shellLoaded = false;

    ForEachExplorerPid([&](DWORD pid) {
        if (firstExplorerPid == 0) firstExplorerPid = pid;
        if (FindHandsOffModuleInPid(pid) != nullptr) {
            anyLoaded = true;
            if (firstLoadedPid == 0) firstLoadedPid = pid;
            if (pid == shellPid) shellLoaded = true;
        } else {
            allLoaded = false;
        }
        return true;
    });

    if (outExplorerPid) {
        if (shellLoaded && shellPid != 0) *outExplorerPid = shellPid;
        else if (firstLoadedPid != 0)     *outExplorerPid = firstLoadedPid;
        else if (shellPid != 0)           *outExplorerPid = shellPid;
        else                              *outExplorerPid = firstExplorerPid;
    }
    if (outAllExplorersLoaded) {
        *outAllExplorersLoaded = anyLoaded && allLoaded;
    }
    return anyLoaded;
}

bool CheckComRegistered(std::wstring* outRegisteredDllPath = nullptr) {
    if (outRegisteredDllPath) outRegisteredDllPath->clear();

    std::wstring inprocKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr + L"\\InprocServer32";
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        wchar_t dllPath[MAX_PATH] = {};
        DWORD cb = sizeof(dllPath);
        if (RegGetValueW(
                root,
                inprocKey.c_str(),
                nullptr,
                RRF_RT_REG_SZ,
                nullptr,
                dllPath,
                &cb) != ERROR_SUCCESS ||
            dllPath[0] == L'\0')
        {
            continue;
        }

        DWORD attrs = GetFileAttributesW(dllPath);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }

        DWORD driveMask = 0;
        DWORD maskCb = sizeof(driveMask);
        bool hasDriveExt =
            (RegGetValueW(root, kDriveFolderExtSubKey, L"DriveMask", RRF_RT_REG_DWORD, nullptr, &driveMask, &maskCb) == ERROR_SUCCESS &&
             (driveMask & 0x08u) != 0);

        bool hasHklmOverlay = false;
        if (root == HKEY_LOCAL_MACHINE) {
            HKEY hOverlay = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kOverlaySubKey, 0, KEY_READ, &hOverlay) == ERROR_SUCCESS) {
                hasHklmOverlay = true;
                RegCloseKey(hOverlay);
            }
        }

        if (hasDriveExt || hasHklmOverlay) {
            if (outRegisteredDllPath) *outRegisteredDllPath = dllPath;
            return true;
        }
    }
    return false;
}

void RegisterAutorunEntry() {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;
    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --autorun";
    DWORD cb = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));

    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(root, kRunRegistrySubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), cb);
            RegCloseKey(hKey);
        }
    }
}

void UnregisterAutorunEntry() {
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, kRunRegistrySubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, kRunValueName);
            RegCloseKey(hKey);
        }
    }
}

void CleanupRegistryDirectFallback() {
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr;
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        RegDeleteKeyW(root, kDriveFolderExtSubKey);
        RegDeleteKeyW(root, kCopyHookSubKey);
        RegDeleteKeyW(root, kLegacyCopyHookSubKey);
        RegDeleteKeyW(root, kOverlaySubKey);
        RegDeleteKeyW(root, kLegacyOverlaySubKey3);
        RegDeleteKeyW(root, kLegacyOverlayOldName);
        HKEY hApp = nullptr;
        if (RegOpenKeyExW(root, kApprovedExtSubKey, 0, KEY_SET_VALUE, &hApp) == ERROR_SUCCESS) {
            RegDeleteValueW(hApp, kClsidRegistryStr);
            RegCloseKey(hApp);
        }
        RegDeleteKeyW(root, inprocKey.c_str());
        RegDeleteKeyW(root, clsidKey.c_str());
    }
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
    struct UnloadWaitEntry {
        DWORD pid;
        HANDLE hDone;
    };
    std::vector<UnloadWaitEntry> waitEntries;
    ForEachExplorerPid([&](DWORD pid) {
        if (FindHandsOffModuleInPid(pid) != nullptr) {
            std::wstring doneName = L"Local\\HandsOffUnloadDone_";
            AppendUint(doneName, pid);
            if (HANDLE hDone = CreateEventW(nullptr, TRUE, FALSE, doneName.c_str())) {
                waitEntries.push_back({ pid, hDone });
            }
        }
        return true;
    });

    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, kUnloadEventName);
    if (!hEvent) {
        for (const auto& entry : waitEntries) CloseHandle(entry.hDone);
        return;
    }
    SetEvent(hEvent);
    for (int i = 0; i < 100; ++i) {
        bool allDone = true;
        for (const auto& entry : waitEntries) {
            if (WaitForSingleObject(entry.hDone, 0) != WAIT_OBJECT_0 &&
                FindHandsOffModuleInPid(entry.pid) != nullptr)
            {
                allDone = false;
                break;
            }
        }
        if (allDone) break;
        Sleep(25);
    }
    if (!waitEntries.empty()) {
        Sleep(25);
    }
    ResetEvent(hEvent);
    CloseHandle(hEvent);
    for (const auto& entry : waitEntries) {
        CloseHandle(entry.hDone);
    }

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

void EnsureCliOutputReady() {
    auto isRedirected = [](DWORD stdHandleId) {
        HANDLE h = GetStdHandle(stdHandleId);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    if (!isRedirected(STD_OUTPUT_HANDLE) && !isRedirected(STD_ERROR_HANDLE)) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            g_consoleAttached = true;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
}

void WriteCliText(const wchar_t* text) {
    if (!text || !*text) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD fileType = (hOut && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    DWORD mode = 0;
    if (fileType == FILE_TYPE_CHAR && GetConsoleMode(hOut, &mode)) {
        DWORD written = 0;
        WriteConsoleW(hOut, text, static_cast<DWORD>(wcslen(text)), &written, nullptr);
        return;
    }
    if (fileType == FILE_TYPE_UNKNOWN && g_consoleAttached) {
        HANDLE hCon = CreateFileW(
            L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hCon != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(hCon, text, static_cast<DWORD>(wcslen(text)), &written, nullptr);
            CloseHandle(hCon);
            return;
        }
    }
    if (fileType == FILE_TYPE_UNKNOWN) return;

    int len = static_cast<int>(wcslen(text));
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, len, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, len, utf8.data(), bytes, nullptr, nullptr);

    DWORD totalWritten = 0;
    while (totalWritten < static_cast<DWORD>(bytes)) {
        DWORD chunk = 0;
        if (!WriteFile(hOut, utf8.data() + totalWritten, static_cast<DWORD>(bytes) - totalWritten, &chunk, nullptr) ||
            chunk == 0)
        {
            break;
        }
        totalWritten += chunk;
    }
}

void PrintUsage() {
    WriteCliText(
        L"HandsOff - Native Windows 10/11 File & Folder Unlocker\n"
        L"Usage:\n"
        L"  HandsOff.exe                  Launch GUI Manager\n"
        L"  HandsOff.exe install          Install & hot-activate in Explorer\n"
        L"  HandsOff.exe uninstall        Hot-unload & remove registrations\n"
        L"  HandsOff.exe scan <path>      Inspect processes locking a file/folder\n"
        L"  HandsOff.exe unlock <path>    Close locking handles without killing\n"
        L"  HandsOff.exe kill <path>      Terminate locking apps & close handles\n");
}

int RunCliViaDll(int action, const wchar_t* targetPath) {
    std::wstring dllPath = GetSiblingDllPath();
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        WriteCliText(L"[ERROR] Failed to load HandsOff.dll.\n");
        return 1;
    }

    using PFN_OutputCb = void(STDAPICALLTYPE*)(LPCWSTR, void*);
    using PFN_CliAction = int(STDAPICALLTYPE*)(int, LPCWSTR, PFN_OutputCb, void*);
    auto pAction = reinterpret_cast<PFN_CliAction>(GetProcAddress(hDll, "HandsOffCliLockAction"));
    int exitCode = 1;
    if (pAction) {
        exitCode = pAction(action, targetPath, [](LPCWSTR text, void*) {
            WriteCliText(text);
        }, nullptr);
    } else {
        WriteCliText(L"[ERROR] Incompatible HandsOff.dll.\n");
    }
    FreeLibrary(hDll);
    return exitCode;
}

} // namespace

PatchStatusInfo QueryPatchStatus() {
    PatchStatusInfo info;
    std::wstring regDllPath;
    info.isComRegistered = CheckComRegistered(&regDllPath);
    bool allExplorersLoaded = false;
    info.isLoadedInExplorer = CheckDllLoadedInExplorer(&info.explorerPid, &allExplorersLoaded);

    // Self-heal if registered in HKLM but a newly spawned or HideIcons=1 Explorer process hasn't loaded the DLL yet.
    if (info.isComRegistered && !allExplorersLoaded && !regDllPath.empty()) {
        if (HotInjectIntoRunningExplorer(regDllPath)) {
            Sleep(40);
            info.isLoadedInExplorer = CheckDllLoadedInExplorer(&info.explorerPid, &allExplorersLoaded);
        }
    }
    return info;
}

PatchOperationResult ApplyPatch() {
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
    RegisterAutorunEntry();
    bool injected = HotInjectIntoRunningExplorer(dllPath);
    Sleep(60);

    return { true, injected ? L"Applied in File Explorer." : L"Registered. Restart Explorer to activate." };
}

PatchOperationResult RollbackPatch() {
    // Remove COM and autorun registrations BEFORE unloading from Explorer so Explorer cannot
    // re-instantiate the COM extension while it is being unloaded.
    UnregisterAutorunEntry();

    std::wstring dllPath = GetSiblingDllPath();
    bool unregisteredViaDll = false;
    if (HMODULE hDll = LoadLibraryW(dllPath.c_str())) {
        if (auto pUnreg = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(hDll, "DllUnregisterServer"))) {
            unregisteredViaDll = SUCCEEDED(pUnreg());
        }
        FreeLibrary(hDll);
    }
    if (!unregisteredViaDll) {
        CleanupRegistryDirectFallback();
    }

    HotUnloadFromRunningExplorer();
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

    if (_wcsicmp(argv[1], L"--autorun") == 0) {
        LocalFree(argv);
        std::wstring dllPath;
        if (!HandsOff::CheckComRegistered(&dllPath) || dllPath.empty()) {
            dllPath = HandsOff::GetSiblingDllPath();
            if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                return 0;
            }
        }
        for (int attempt = 0; attempt < 15; ++attempt) {
            DWORD pid = 0;
            bool allLoaded = false;
            HandsOff::CheckDllLoadedInExplorer(&pid, &allLoaded);
            if (pid != 0) {
                if (!allLoaded) {
                    HandsOff::HotInjectIntoRunningExplorer(dllPath);
                }
                break;
            }
            Sleep(200);
        }
        return 0;
    }

    if (_wcsicmp(argv[1], L"--elevated-install") == 0) {
        LocalFree(argv);
        auto res = HandsOff::ApplyPatch();
        return res.success ? 0 : 1;
    }

    if (_wcsicmp(argv[1], L"--elevated-uninstall") == 0) {
        LocalFree(argv);
        auto res = HandsOff::RollbackPatch();
        return res.success ? 0 : 1;
    }

    if (_wcsicmp(argv[1], L"gui") == 0) {
        LocalFree(argv);
        return HandsOff::RunGuiWindow(hInstance);
    }

    HandsOff::EnsureCliOutputReady();

    const wchar_t* cmd = argv[1];
    int exitCode = 0;

    if (_wcsicmp(cmd, L"install") == 0) {
        auto res = HandsOff::ApplyPatch();
        HandsOff::WriteCliText(res.success ? L"[OK] " : L"[ERROR] ");
        HandsOff::WriteCliText(res.statusMessage.c_str());
        HandsOff::WriteCliText(L"\n");
        exitCode = res.success ? 0 : 1;
    } else if (_wcsicmp(cmd, L"uninstall") == 0) {
        auto res = HandsOff::RollbackPatch();
        HandsOff::WriteCliText(res.success ? L"[OK] " : L"[ERROR] ");
        HandsOff::WriteCliText(res.statusMessage.c_str());
        HandsOff::WriteCliText(L"\n");
        exitCode = res.success ? 0 : 1;
    } else if (_wcsicmp(cmd, L"scan") == 0 && argc >= 3) {
        exitCode = HandsOff::RunCliViaDll(0, argv[2]);
    } else if (_wcsicmp(cmd, L"unlock") == 0 && argc >= 3) {
        exitCode = HandsOff::RunCliViaDll(1, argv[2]);
    } else if (_wcsicmp(cmd, L"kill") == 0 && argc >= 3) {
        exitCode = HandsOff::RunCliViaDll(2, argv[2]);
    } else {
        HandsOff::PrintUsage();
        exitCode = 1;
    }

    LocalFree(argv);
    return exitCode;
}
