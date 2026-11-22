#include "common.h"
#include "dialog_enhancer.h"
#include "file_in_use_hook.h"
#include "lock_finder.h"
#include <shlobj.h>

namespace HandsOff {

namespace {

HMODULE g_hDllInstance = nullptr;
std::atomic<ULONG> g_dllLockCount{ 0 };
std::mutex g_activationMutex;
HANDLE g_hStopLocalEvent = nullptr;

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
           _wcsicmp(exe, L"regsvr32.exe") == 0 ||
           _wcsicmp(exe, L"HandsOffTests.exe") == 0;
}

bool ActivateExtension() {
    if (!IsCurrentProcessExplorer()) return false;
    std::lock_guard<std::mutex> lock(g_activationMutex);
    if (!FileInUseHook::Install()) return false;
    DialogEnhancer::Initialize(g_hDllInstance);
    return true;
}

void DeactivateExtension() {
    std::lock_guard<std::mutex> lock(g_activationMutex);
    DialogEnhancer::Shutdown();
    FileInUseHook::Uninstall();
    LockFinder::Shutdown();
}

DWORD WINAPI StartupAndHotUnloadThread(LPVOID) {
    ActivateExtension();

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
        DeactivateExtension();
        if (g_hStopLocalEvent) {
            CloseHandle(g_hStopLocalEvent);
            g_hStopLocalEvent = nullptr;
        }
        FreeLibraryAndExitThread(g_hDllInstance, 0);
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
            g_hStopLocalEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (HANDLE hThread = CreateThread(nullptr, 0, &StartupAndHotUnloadThread, nullptr, 0, nullptr)) {
                CloseHandle(hThread);
            }
        }
    } else if (fdwReason == DLL_PROCESS_DETACH && lpReserved == nullptr) {
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
    if (FileInUseHook::IsInstalled() || DialogEnhancer::IsRunning()) return S_FALSE;
    return (g_dllLockCount.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID, REFIID, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    if (!IsCurrentProcessExplorer()) return CLASS_E_CLASSNOTAVAILABLE;

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
            SetRegistryStringValue(root, inprocKey.c_str(), L"ThreadingModel", L"Apartment"))
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
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
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
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" BOOL STDAPICALLTYPE HandsOffInitializeHook() {
    return ActivateExtension() ? TRUE : FALSE;
}

extern "C" VOID STDAPICALLTYPE HandsOffRemoveHook() {
    DeactivateExtension();
}
