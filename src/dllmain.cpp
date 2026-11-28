#include "common.h"
#include "dialog_enhancer.h"
#include "file_in_use_hook.h"
#include "lock_finder.h"
#include <shlobj.h>
#include <tlhelp32.h>

namespace HandsOff {

namespace {

HMODULE g_hDllInstance = nullptr;
std::atomic<ULONG> g_dllLockCount{ 0 };
std::atomic<bool> g_wasQueriedByCom{ false };
// Set once the hot-unload sequence starts: no new activation may happen from then on.
std::atomic<bool> g_unloading{ false };
// True while this module holds its own self-pin reference (taken in DllMain), so combase may
// safely drop its reference at any time without unmapping us.
std::atomic<bool> g_pinned{ false };
// Set by DllCanUnloadNow whenever combase probes whether this DLL can unload.
// Used to verify combase has actually unlinked our CDllPathEntry from CClassCache.
std::atomic<bool> g_sawCanUnload{ false };
std::atomic<int> g_pendingGetMsgPurges{ 0 };
SlimMutex g_activationMutex;
HANDLE g_hStopLocalEvent = nullptr;
UINT g_msgPurgeComCache = 0;

void TraceState(const wchar_t* what, uint64_t a = 0, uint64_t b = 0) {
    TraceBuilder tb;
    tb.AppendStr(L"Lifecycle: ");
    tb.AppendStr(what);
    tb.AppendStr(L" a=");
    tb.AppendUint(a);
    tb.AppendStr(L" b=");
    tb.AppendUint(b);
    AppendOptionalTraceLine(tb.Finish());
}

bool IsCurrentProcessExplorer() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"explorer.exe") == 0;
}

bool IsAllowedNonExplorerHost() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    const wchar_t* exe = name ? name + 1 : path;
    return _wcsicmp(exe, L"HandsOff.exe") == 0 ||
           _wcsicmp(exe, L"regsvr32.exe") == 0;
}

bool ActivateExtension() {
    if (!IsCurrentProcessExplorer()) return false;
    if (g_unloading.load(std::memory_order_acquire)) return false;
    SlimLockGuard lock(g_activationMutex);
    if (g_unloading.load(std::memory_order_acquire)) return false;
    if (!FileInUseHook::Install()) return false;
    DialogEnhancer::Initialize(g_hDllInstance);
    return true;
}

void DeactivateExtension() {
    SlimLockGuard lock(g_activationMutex);
    DialogEnhancer::Shutdown();
    FileInUseHook::Uninstall();
    LockFinder::Shutdown();
}

LRESULT CALLBACK PurgeComCacheHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam) {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (g_msgPurgeComCache != 0 && cwp->message == g_msgPurgeComCache) {
            CoFreeUnusedLibrariesEx(0, 0);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK PurgeComCacheGetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == PM_REMOVE && lParam) {
        auto* msg = reinterpret_cast<MSG*>(lParam);
        if (g_msgPurgeComCache != 0 && msg->message == g_msgPurgeComCache) {
            msg->message = WM_NULL;
            CoFreeUnusedLibrariesEx(0, 0);
            g_pendingGetMsgPurges.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool IsComEntryStillCached() {
    g_sawCanUnload.store(false, std::memory_order_release);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoFreeUnusedLibrariesEx(0, 0);
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return g_sawCanUnload.load(std::memory_order_acquire);
}

void PurgeComClassCacheSinglePass(bool includeWindowThreadsInPost) {
    HRESULT hrMta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoFreeUnusedLibrariesEx(0, 0);
    if (SUCCEEDED(hrMta)) {
        CoUninitialize();
    }

    HRESULT hrSta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoFreeUnusedLibrariesEx(0, 0);
    if (SUCCEEDED(hrSta)) {
        CoUninitialize();
    }

    if (!g_msgPurgeComCache) {
        g_msgPurgeComCache = RegisterWindowMessageW(L"HandsOff_PurgeComCacheMsg");
    }
    if (!g_msgPurgeComCache) return;

    struct ThreadHwndPair {
        DWORD tid;
        HWND hwnd;
    };
    struct EnumTargetsCtx {
        DWORD pid;
        std::vector<ThreadHwndPair> targets;
        void Add(HWND hwnd) {
            DWORD winPid = 0;
            DWORD tid = GetWindowThreadProcessId(hwnd, &winPid);
            if (winPid == pid && tid != 0) {
                for (const auto& item : targets) {
                    if (item.tid == tid) return;
                }
                targets.push_back({ tid, hwnd });
            }
        }
    } ctx{ GetCurrentProcessId(), {} };

    // Enumerate message-only windows too: combase.dll creates per-STA OleMainThreadWndClass
    // under HWND_MESSAGE, which EnumWindows skips.
    HWND hMsg = nullptr;
    while ((hMsg = FindWindowExW(HWND_MESSAGE, hMsg, nullptr, nullptr)) != nullptr) {
        ctx.Add(hMsg);
    }

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        reinterpret_cast<EnumTargetsCtx*>(lParam)->Add(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    DWORD selfTid = GetCurrentThreadId();
    for (const auto& t : ctx.targets) {
        if (t.tid == selfTid) continue;
        if (HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, &PurgeComCacheHookProc, g_hDllInstance, t.tid)) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(t.hwnd, g_msgPurgeComCache, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 150, &res);
            UnhookWindowsHookEx(hHook);
        }
    }

    // Windowless STA threads (such as windows_storage!CShellTaskThread) pump messages via
    // MsgWaitForMultipleObjects + PeekMessageW without owning any HWND, so WH_CALLWNDPROC
    // misses them. Wake every message-queue thread via WH_GETMESSAGE + PostThreadMessageW.
    std::vector<HHOOK> getMsgHooks;
    g_pendingGetMsgPurges.store(0, std::memory_order_release);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        DWORD myPid = GetCurrentProcessId();
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(hSnap, &te)) {
            do {
                if (te.th32OwnerProcessID != myPid || te.th32ThreadID == selfTid) continue;
                if (!includeWindowThreadsInPost) {
                    bool hasWindow = false;
                    for (const auto& t : ctx.targets) {
                        if (t.tid == te.th32ThreadID) { hasWindow = true; break; }
                    }
                    if (hasWindow) continue;
                }
                if (HHOOK hGet = SetWindowsHookExW(WH_GETMESSAGE, &PurgeComCacheGetMsgProc, g_hDllInstance, te.th32ThreadID)) {
                    g_pendingGetMsgPurges.fetch_add(1, std::memory_order_acq_rel);
                    if (PostThreadMessageW(te.th32ThreadID, g_msgPurgeComCache, 0, 0)) {
                        getMsgHooks.push_back(hGet);
                    } else {
                        g_pendingGetMsgPurges.fetch_sub(1, std::memory_order_acq_rel);
                        UnhookWindowsHookEx(hGet);
                    }
                }
            } while (Thread32Next(hSnap, &te));
        }
        CloseHandle(hSnap);
    }

    if (!getMsgHooks.empty()) {
        for (int w = 0; w < 8 && g_pendingGetMsgPurges.load(std::memory_order_acquire) > 0; ++w) {
            Sleep(15);
        }
        for (HHOOK hGet : getMsgHooks) {
            UnhookWindowsHookEx(hGet);
        }
    }
}

DWORD WINAPI StartupAndHotUnloadThread(LPVOID param) {
    HMODULE hSelfPin = static_cast<HMODULE>(param);

    ActivateExtension();
    Sleep(15);

    HANDLE hGlobalUnload = CreateEventW(nullptr, TRUE, FALSE, kUnloadEventName);
    HANDLE waits[2] = {};
    DWORD waitCount = 0;
    int globalIdx = -1;
    if (g_hStopLocalEvent) waits[waitCount++] = g_hStopLocalEvent;
    if (hGlobalUnload) {
        globalIdx = static_cast<int>(waitCount);
        waits[waitCount++] = hGlobalUnload;
    }

    DWORD res = (waitCount > 0)
        ? WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE)
        : WAIT_FAILED;
    if (hGlobalUnload) CloseHandle(hGlobalUnload);

    if (globalIdx >= 0 && res == WAIT_OBJECT_0 + static_cast<DWORD>(globalIdx)) {
        // Block any further (re-)activation, e.g. from a late DllGetClassObject call.
        g_unloading.store(true, std::memory_order_release);
        TraceState(L"unload begin queriedByCom", g_wasQueriedByCom.load());

        // combase.dll caches DllCanUnloadNow/DllGetClassObject pointers of every in-proc server it
        // loaded in a CClassCache::CDllPathEntry. Unmapping our image while that entry still exists
        // crashes explorer.exe with FAST_FAIL_GUARD_ICALL_CHECK_FAILURE on the next COM probe or
        // CoCreateInstance. Purge all apartments (HWND + windowless message queues) and probe until
        // combase no longer calls our DllCanUnloadNow (meaning CDllPathEntry has been unlinked).
        if (g_wasQueriedByCom.load(std::memory_order_acquire)) {
            int purgePasses = 0;
            bool stillCached = true;
            for (int i = 0; i < 16; ++i) {
                PurgeComClassCacheSinglePass(i > 0);
                purgePasses = i + 1;
                stillCached = IsComEntryStillCached();
                if (!stillCached) break;
                Sleep(30);
            }
            TraceState(L"purge done stillCached/passes", stillCached ? 1 : 0, purgePasses);
            if (!stillCached) {
                // Allow CFinishComposite::Finish in combase to finish its FreeLibrary call.
                Sleep(30);
            }
        }

        DeactivateExtension();
        TraceState(L"deactivated, releasing pin");

        if (g_hStopLocalEvent) {
            CloseHandle(g_hStopLocalEvent);
            g_hStopLocalEvent = nullptr;
        }

        std::wstring doneName = L"Local\\HandsOffUnloadDone_";
        AppendUint(doneName, GetCurrentProcessId());
        if (HANDLE hDone = OpenEventW(EVENT_MODIFY_STATE, FALSE, doneName.c_str())) {
            SetEvent(hDone);
            CloseHandle(hDone);
        }

        FreeLibraryAndExitThread(hSelfPin ? hSelfPin : g_hDllInstance, 0);
    }
    return 0;
}

bool SetRegistryStringValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = RegSetValueExW(
        hKey, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(data),
        static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

bool SetRegistryDwordValue(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, DWORD data) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = RegSetValueExW(
        hKey, valueName, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&data),
        sizeof(DWORD));
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

void DeleteRegistryValueIfPresent(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, valueName);
        RegCloseKey(hKey);
    }
}

} // namespace

} // namespace HandsOff

using namespace HandsOff;

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        // Never allow HandsOff.dll to load into third-party processes (DAWs, browsers, games, etc.)
        // that enumerate shell extensions via File Open/Save dialogs. Returning FALSE instructs
        // ntdll!LdrpLoadDll to immediately unmap the DLL before LoadLibraryExW returns.
        if (!IsCurrentProcessExplorer() && !IsAllowedNonExplorerHost()) {
            return FALSE;
        }
        g_hDllInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        if (IsCurrentProcessExplorer()) {
            // Pin the module (+1) before spawning StartupAndHotUnloadThread so short-lived
            // explorer.exe instances (e.g. /select launchers) cannot unmap the DLL while
            // the worker thread is inside Sleep(15) or WaitForMultipleObjects.
            HMODULE hSelfPin = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&StartupAndHotUnloadThread),
                &hSelfPin);
            g_pinned.store(hSelfPin != nullptr, std::memory_order_release);
            g_hStopLocalEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (HANDLE hThread = CreateThread(nullptr, 0, &StartupAndHotUnloadThread, hSelfPin, 0, nullptr)) {
                CloseHandle(hThread);
            } else if (g_hStopLocalEvent) {
                CloseHandle(g_hStopLocalEvent);
                g_hStopLocalEvent = nullptr;
            }
        }
    } else if (fdwReason == DLL_PROCESS_DETACH && lpReserved == nullptr) {
        if (IsCurrentProcessExplorer()) TraceState(L"DLL_PROCESS_DETACH (unmapping)");
        if (g_hStopLocalEvent) {
            SetEvent(g_hStopLocalEvent);
            CloseHandle(g_hStopLocalEvent);
            g_hStopLocalEvent = nullptr;
        }
        FileInUseHook::Uninstall();
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    if (!IsCurrentProcessExplorer()) return S_OK;
    // Our own pin reference (taken in DllMain) keeps the image mapped for as long as the hooks
    // are alive and until the hot-unload worker releases it, so combase may drop its reference
    // (and unlink its cached CDllPathEntry) at any time. Answering S_OK here is what removes
    // the cached DllCanUnloadNow/DllGetClassObject pointers from combase before we unmap.
    if (!g_pinned.load(std::memory_order_acquire)) return S_FALSE;
    if (g_dllLockCount.load() != 0) return S_FALSE;
    g_sawCanUnload.store(true, std::memory_order_release);
    return S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID, REFIID, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    if (!IsCurrentProcessExplorer()) return CLASS_E_CLASSNOTAVAILABLE;

    g_wasQueriedByCom.store(true, std::memory_order_release);

    // Activate our hooks in explorer.exe and return CLASS_E_CLASSNOTAVAILABLE so shell32.dll
    // never caches an ICopyHookW* or IShellIconOverlayIdentifier* vtable pointer into HandsOff.dll
    // and never consumes one of Windows Explorer's 11 user icon overlay slots.
    ActivateExtension();
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
    wchar_t dllPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_hDllInstance, dllPath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr;
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";

    bool anySuccess = false;
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        RegDeleteKeyW(root, kLegacyCopyHookSubKey);
        RegDeleteKeyW(root, kLegacyOverlaySubKey3);
        RegDeleteKeyW(root, kLegacyOverlayOldName);

        if (SetRegistryStringValue(root, clsidKey.c_str(), nullptr, kExtensionName) &&
            SetRegistryStringValue(root, inprocKey.c_str(), nullptr, dllPath) &&
            SetRegistryStringValue(root, inprocKey.c_str(), L"ThreadingModel", L"Both"))
        {
            // Drive\shellex\FolderExtensions (with DriveMask=255) is read from HKEY_CLASSES_ROOT
            // (merging HKCU\Software\Classes and HKLM\Software\Classes) by windows.storage.dll on
            // every Explorer startup (even with HideIcons=1) and in every separate Explorer process,
            // working 100% without requiring Administrator privileges.
            bool driveExtOk =
                SetRegistryStringValue(root, kDriveFolderExtSubKey, nullptr, kExtensionName) &&
                SetRegistryDwordValue(root, kDriveFolderExtSubKey, L"DriveMask", 0xFFu);
            bool copyOk = SetRegistryStringValue(root, kCopyHookSubKey, nullptr, kClsidRegistryStr);
            SetRegistryStringValue(root, kOverlaySubKey, nullptr, kClsidRegistryStr);
            SetRegistryStringValue(root, kApprovedExtSubKey, kClsidRegistryStr, kExtensionName);
            if (driveExtOk && copyOk) {
                anySuccess = true;
            }
        }
    }
    return anySuccess ? S_OK : E_ACCESSDENIED;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr;
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";

    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        RegDeleteKeyW(root, kDriveFolderExtSubKey);
        RegDeleteKeyW(root, kCopyHookSubKey);
        RegDeleteKeyW(root, kLegacyCopyHookSubKey);
        RegDeleteKeyW(root, kOverlaySubKey);
        RegDeleteKeyW(root, kLegacyOverlaySubKey3);
        RegDeleteKeyW(root, kLegacyOverlayOldName);
        DeleteRegistryValueIfPresent(root, kApprovedExtSubKey, kClsidRegistryStr);
        RegDeleteKeyW(root, inprocKey.c_str());
        RegDeleteKeyW(root, clsidKey.c_str());
    }
    return S_OK;
}

extern "C" BOOL STDAPICALLTYPE HandsOffInitializeHook() {
    return ActivateExtension() ? TRUE : FALSE;
}

extern "C" VOID STDAPICALLTYPE HandsOffRemoveHook() {
    DeactivateExtension();
}

extern "C" BOOL STDAPICALLTYPE HandsOffServiceUnlock(
    LPCWSTR targetPath,
    uint32_t mode,
    uint32_t* outClosedHandles,
    uint32_t* outTerminatedProcs)
{
    if (!targetPath || !*targetPath) return FALSE;
    uint32_t closed = 0, killed = 0;
    bool ok = LockFinder::UnlockTarget(
        targetPath, {}, static_cast<UnlockMode>(mode), &closed, &killed, false);
    LockFinder::Shutdown();
    if (outClosedHandles)   *outClosedHandles = closed;
    if (outTerminatedProcs) *outTerminatedProcs = killed;
    return ok ? TRUE : FALSE;
}

using PFN_HandsOffOutputCallback = void(STDAPICALLTYPE*)(LPCWSTR text, void* userCtx);

extern "C" int STDAPICALLTYPE HandsOffCliLockAction(
    int action,
    LPCWSTR targetPath,
    PFN_HandsOffOutputCallback onOutput,
    void* userCtx)
{
    if (!targetPath || !*targetPath) return 1;

    std::wstring out;
    int exitCode = 0;

    if (action == 0) {
        DWORD t0 = GetTickCount();
        auto locks = LockFinder::FindLocks(targetPath, true);
        DWORD elapsed = GetTickCount() - t0;

        out += L"Target: ";
        out += targetPath;
        out += L"\nScan time: ";
        AppendUint(out, elapsed);
        out += L" ms\n";

        if (locks.empty()) {
            out += L"Result: No locking processes found.\n";
        } else {
            out += L"Summary for Explorer Dialog: \"";
            out += LockFinder::FormatBlockerSummary(locks);
            out += L"\"\nBlocking Processes (";
            AppendUint(out, locks.size());
            out += L"):\n";
            for (const auto& l : locks) {
                out += L"  - PID ";
                AppendUint(out, l.pid);
                out += L" | ";
                out += l.processName;
                out += L" (";
                out += l.appDescription;
                out += L") | Handles: ";
                AppendUint(out, l.handleValues.size());
                if (!l.lockedRelativePath.empty()) {
                    out += L" | SubItem: ";
                    out += l.lockedRelativePath;
                }
                if (l.isSystemCritical) out += L" [SYSTEM-CRITICAL]";
                if (l.isSelfExplorer)   out += L" [EXPLORER-SELF]";
                out += L"\n";
            }
        }
    } else {
        UnlockMode mode = (action == 2)
            ? UnlockMode::KillProcessAndCloseHandles
            : UnlockMode::CloseHandlesOnly;
        auto locks = LockFinder::FindLocks(targetPath, true);
        if (locks.empty()) {
            out += L"No locks found on: ";
            out += targetPath;
            out += L"\n";
        } else {
            uint32_t closedHandles = 0, killedProcs = 0;
            bool ok = LockFinder::UnlockTarget(targetPath, locks, mode, &closedHandles, &killedProcs);
            out += ok ? L"[OK] Unlocked target" : L"[WARN] Partial or failed unlock";
            out += L" | Closed Handles: ";
            AppendUint(out, closedHandles);
            out += L" | Terminated Processes: ";
            AppendUint(out, killedProcs);
            out += L"\n";
            exitCode = ok ? 0 : 1;
        }
    }

    LockFinder::Shutdown();
    if (onOutput && !out.empty()) {
        onOutput(out.c_str(), userCtx);
    }
    return exitCode;
}
