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

bool ActivateExtension() {
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

class HandsOffShellExt final : public ICopyHookW, public IShellIconOverlayIdentifier {
public:
    HandsOffShellExt() { ++g_dllLockCount; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICopyHookW)) {
            *ppv = static_cast<ICopyHookW*>(this);
        } else if (IsEqualIID(riid, IID_IShellIconOverlayIdentifier)) {
            *ppv = static_cast<IShellIconOverlayIdentifier*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG c = --refCount_;
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP_(UINT) CopyCallback(HWND, UINT, UINT, LPCWSTR, DWORD, LPCWSTR, DWORD) override {
        ActivateExtension();
        return IDYES;
    }

    IFACEMETHODIMP IsMemberOf(LPCWSTR, DWORD) override {
        ActivateExtension();
        return S_FALSE;
    }

    IFACEMETHODIMP GetOverlayInfo(LPWSTR, int, int*, DWORD*) override { return E_NOTIMPL; }

    IFACEMETHODIMP GetPriority(int* pPriority) override {
        if (pPriority) *pPriority = 100;
        return S_OK;
    }

private:
    ~HandsOffShellExt() { --g_dllLockCount; }
    std::atomic<ULONG> refCount_{ 1 };
};

class HandsOffClassFactory final : public IClassFactory {
public:
    HandsOffClassFactory() { ++g_dllLockCount; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG c = --refCount_;
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto* ext = new (std::nothrow) HandsOffShellExt();
        if (!ext) return E_OUTOFMEMORY;
        HRESULT hr = ext->QueryInterface(riid, ppv);
        ext->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) ++g_dllLockCount;
        else --g_dllLockCount;
        return S_OK;
    }

private:
    ~HandsOffClassFactory() { --g_dllLockCount; }
    std::atomic<ULONG> refCount_{ 1 };
};

bool IsCurrentProcessExplorer() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"explorer.exe") == 0;
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

} // namespace

} // namespace HandsOff

using namespace HandsOff;

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
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
    if (FileInUseHook::IsInstalled() || DialogEnhancer::IsRunning()) return S_FALSE;
    return (g_dllLockCount.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    ActivateExtension();
    if (!IsEqualCLSID(rclsid, CLSID_HandsOffShellExt)) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) HandsOffClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
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
        if (SetRegistryStringValue(root, clsidKey.c_str(), nullptr, kExtensionName) &&
            SetRegistryStringValue(root, inprocKey.c_str(), nullptr, dllPath) &&
            SetRegistryStringValue(root, inprocKey.c_str(), L"ThreadingModel", L"Apartment"))
        {
            anySuccess = true;
            SetRegistryStringValue(
                root, L"Software\\Classes\\Directory\\shellex\\CopyHookHandlers\\HandsOff",
                nullptr, kClsidRegistryStr);
            SetRegistryStringValue(
                root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   HandsOff",
                nullptr, kClsidRegistryStr);
        }
    }
    return anySuccess ? S_OK : E_ACCESSDENIED;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidRegistryStr;
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";

    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        RegDeleteKeyW(root, L"Software\\Classes\\Directory\\shellex\\CopyHookHandlers\\HandsOff");
        RegDeleteKeyW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   HandsOff");
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
