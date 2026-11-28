#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace HandsOff {

class SlimMutex {
public:
    constexpr SlimMutex() noexcept = default;
    SlimMutex(const SlimMutex&) = delete;
    SlimMutex& operator=(const SlimMutex&) = delete;

    void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
    void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
};

class SlimLockGuard {
public:
    explicit SlimLockGuard(SlimMutex& m) noexcept : m_(m) { m_.lock(); }
    ~SlimLockGuard() { m_.unlock(); }
    SlimLockGuard(const SlimLockGuard&) = delete;
    SlimLockGuard& operator=(const SlimLockGuard&) = delete;

private:
    SlimMutex& m_;
};

// {8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}
inline constexpr GUID CLSID_HandsOffShellExt = {
    0x8e4a2b19, 0x5c71, 0x4d83, { 0x9a, 0x2e, 0x6f, 0x1c, 0x9b, 0x04, 0xe5, 0xd2 }
};

inline constexpr const wchar_t* kClsidRegistryStr       = L"{8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}";
inline constexpr const wchar_t* kExtensionName          = L"HandsOff Shell Extension";
inline constexpr const wchar_t* kDriveFolderExtSubKey   = L"Software\\Classes\\Drive\\shellex\\FolderExtensions\\{8E4A2B19-5C71-4D83-9A2E-6F1C9B04E5D2}";
inline constexpr const wchar_t* kCopyHookSubKey         = L"Software\\Classes\\Directory\\shellex\\CopyHookHandlers\\HandsOff";
inline constexpr const wchar_t* kLegacyCopyHookSubKey   = L"Software\\Classes\\Directory\\shellex\\CopyHookHandlers\\CloseAlready";
inline constexpr const wchar_t* kOverlaySubKey          = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\            HandsOff";
inline constexpr const wchar_t* kLegacyOverlaySubKey3   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   HandsOff";
inline constexpr const wchar_t* kLegacyOverlayOldName   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   CloseAlready";
inline constexpr const wchar_t* kApprovedExtSubKey      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
inline constexpr const wchar_t* kRunRegistrySubKey      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* kRunValueName           = L"HandsOff";
inline constexpr const wchar_t* kServiceName            = L"HandsOffSvc";
inline constexpr const wchar_t* kServiceDisplay         = L"HandsOff On-Demand Unlock Service";
inline constexpr const wchar_t* kServicePipeName        = L"\\\\.\\pipe\\HandsOffElevationPipe";
inline constexpr const wchar_t* kUnloadEventName        = L"Local\\HandsOffUnloadEvent";
inline constexpr const wchar_t* kBarWindowClass         = L"HandsOffActionBarWnd";

inline bool IsProcessElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION elev = {};
    DWORD cb = sizeof(elev);
    bool isElev = GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cb) &&
                  elev.TokenIsElevated != 0;
    CloseHandle(hToken);
    return isElev;
}

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

inline void AppendUint(std::wstring& out, uint64_t val, int minDigits = 1) {
    wchar_t buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<wchar_t>(L'0' + (val % 10));
        val /= 10;
    } while (val != 0);
    while (n < minDigits && n < 24) {
        buf[n++] = L'0';
    }
    while (n > 0) {
        out.push_back(buf[--n]);
    }
}

inline std::wstring UintToWString(uint64_t val) {
    std::wstring out;
    AppendUint(out, val);
    return out;
}

inline void AppendHex4(std::wstring& out, uint16_t val) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    for (int shift = 12; shift >= 0; shift -= 4) {
        out.push_back(kHex[(val >> shift) & 0xF]);
    }
}

struct TraceBuilder {
    wchar_t buf[1024];
    size_t len = 0;

    void AppendStr(const wchar_t* s, size_t maxChars = 1024) {
        if (!s) return;
        while (*s && maxChars > 0 && len + 1 < 1024) {
            buf[len++] = *s++;
            --maxChars;
        }
    }

    void AppendUint(uint64_t val, int minDigits = 1) {
        wchar_t tmp[24];
        int n = 0;
        do {
            tmp[n++] = static_cast<wchar_t>(L'0' + (val % 10));
            val /= 10;
        } while (val != 0);
        while (n < minDigits && n < 24) tmp[n++] = L'0';
        while (n > 0 && len + 1 < 1024) buf[len++] = tmp[--n];
    }

    const wchar_t* Finish() {
        buf[len] = L'\0';
        return buf;
    }
};

inline void AppendOptionalTraceLine(const wchar_t* line) {
    if (!line || !*line) return;
    wchar_t tracePath[MAX_PATH] = {};
    DWORD cb = sizeof(tracePath);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\HandsOff",
            L"TraceFile",
            RRF_RT_REG_SZ,
            nullptr,
            tracePath,
            &cb) != ERROR_SUCCESS ||
        tracePath[0] == L'\0')
    {
        return;
    }

    HANDLE hFile = CreateFileW(
        tracePath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    TraceBuilder tb;
    tb.AppendStr(L"[");
    tb.AppendUint(st.wHour, 2);
    tb.AppendStr(L":");
    tb.AppendUint(st.wMinute, 2);
    tb.AppendStr(L":");
    tb.AppendUint(st.wSecond, 2);
    tb.AppendStr(L".");
    tb.AppendUint(st.wMilliseconds, 3);
    tb.AppendStr(L" pid=");
    tb.AppendUint(GetCurrentProcessId());
    tb.AppendStr(L" tid=");
    tb.AppendUint(GetCurrentThreadId());
    tb.AppendStr(L"] ");
    tb.AppendStr(line, 900);
    tb.AppendStr(L"\r\n");

    if (tb.len > 0) {
        char utf8[3072];
        int bytes = WideCharToMultiByte(
            CP_UTF8, 0, tb.buf, static_cast<int>(tb.len), utf8, sizeof(utf8), nullptr, nullptr);
        if (bytes > 0) {
            DWORD written = 0;
            WriteFile(hFile, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        }
    }
    CloseHandle(hFile);
}

} // namespace HandsOff
