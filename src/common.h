#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace HandsOff {

// {8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}
inline constexpr GUID CLSID_HandsOffShellExt = {
    0x8e4a2b19, 0x5c71, 0x4d83, { 0x9a, 0x2e, 0x6f, 0x1c, 0x9b, 0x04, 0xe5, 0xd2 }
};

inline constexpr const wchar_t* kClsidRegistryStr = L"{8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}";
inline constexpr const wchar_t* kExtensionName    = L"HandsOff Shell Extension";
inline constexpr const wchar_t* kServiceName      = L"HandsOffSvc";
inline constexpr const wchar_t* kServiceDisplay   = L"HandsOff On-Demand Unlock Service";
inline constexpr const wchar_t* kServicePipeName  = L"\\\\.\\pipe\\HandsOffElevationPipe";
inline constexpr const wchar_t* kUnloadEventName  = L"Local\\HandsOffUnloadEvent";
inline constexpr const wchar_t* kBarWindowClass   = L"HandsOffActionBarWnd";

enum class UnlockMode : uint32_t {
    CloseHandlesOnly           = 1,
    KillProcessAndCloseHandles = 2
};

#pragma pack(push, 1)
struct ElevationUnlockRequest {
    uint32_t magic;      // 0x484F4646 ('HOFF')
    uint32_t mode;       // UnlockMode
    wchar_t  targetPath[1024];
};

struct ElevationUnlockResponse {
    uint32_t success;
    uint32_t closedHandles;
    uint32_t terminatedProcesses;
    uint32_t errorCode;
};
#pragma pack(pop)

inline bool StartsWithCaseInsensitive(const std::wstring& str, const std::wstring& prefix) {
    return str.size() >= prefix.size() && _wcsnicmp(str.c_str(), prefix.c_str(), prefix.size()) == 0;
}

inline bool EqualsCaseInsensitive(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

inline std::wstring StripExtendedPathPrefix(const std::wstring& path) {
    if (StartsWithCaseInsensitive(path, L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (StartsWithCaseInsensitive(path, L"\\\\?\\"))     return path.substr(4);
    return path;
}

inline std::wstring TrimTrailingSlashes(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

inline bool IsSystemDarkMode(bool defaultDark = true) {
    DWORD value = defaultDark ? 0 : 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size) == ERROR_SUCCESS)
    {
        return value == 0;
    }
    return defaultDark;
}

inline std::wstring GetSiblingDllPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    return ((slash == std::wstring::npos) ? L"." : path.substr(0, slash)) + L"\\HandsOff.dll";
}

} // namespace HandsOff
