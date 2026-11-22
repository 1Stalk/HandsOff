#include "lock_finder.h"
#include <restartmanager.h>
#include <winternl.h>
#include <map>
#include <set>

namespace HandsOff {

namespace {

constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
constexpr ULONG kSystemExtendedHandleInformation = 64;

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
};

using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

// Watchdog thread for GetFinalPathNameByHandleW so blocking pipes/drivers never hang Explorer.
// Never uses TerminateThread (which would orphan heap/loader locks).
class SafePathResolver {
public:
    static SafePathResolver& Instance() {
        static SafePathResolver inst;
        return inst;
    }

    bool QueryFilePath(HANDLE hDup, std::wstring& outPath, bool& outTimedOut) {
        outTimedOut = false;
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureWorker();
        if (!hWorker_ || !ctx_) return false;

        ctx_->currentHandle = hDup;
        ctx_->resultLen = 0;
        ResetEvent(ctx_->hDoneEvent);
        SetEvent(ctx_->hWorkEvent);

        if (WaitForSingleObject(ctx_->hDoneEvent, 12) == WAIT_OBJECT_0) {
            if (ctx_->resultLen > 0 && ctx_->resultLen < 1020) {
                outPath.assign(ctx_->resultBuf, ctx_->resultLen);
                return true;
            }
            return false;
        }

        outTimedOut = true;
        CancelSynchronousIo(hWorker_);
        if (WaitForSingleObject(ctx_->hDoneEvent, 2) != WAIT_OBJECT_0) {
            ctx_->abandoned.store(true);
            SetEvent(ctx_->hWorkEvent);
            CloseHandle(hWorker_);
            hWorker_ = nullptr;
            ctx_ = nullptr;
        }
        return false;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hWorker_ && ctx_) {
            ctx_->abandoned.store(true);
            SetEvent(ctx_->hWorkEvent);
            WaitForSingleObject(hWorker_, 500);
            CloseHandle(hWorker_);
            hWorker_ = nullptr;
            ctx_ = nullptr;
        }
    }

private:
    struct WorkerContext {
        HANDLE hWorkEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE hDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE currentHandle = nullptr;
        DWORD resultLen = 0;
        wchar_t resultBuf[1024] = {};
        std::atomic<bool> abandoned{ false };

        ~WorkerContext() {
            if (hWorkEvent) CloseHandle(hWorkEvent);
            if (hDoneEvent) CloseHandle(hDoneEvent);
        }
    };

    void EnsureWorker() {
        if (!hWorker_) {
            ctx_ = new (std::nothrow) WorkerContext();
            if (!ctx_) return;
            hWorker_ = CreateThread(nullptr, 0, &WorkerProc, ctx_, 0, nullptr);
            if (!hWorker_) {
                delete ctx_;
                ctx_ = nullptr;
            }
        }
    }

    static DWORD WINAPI WorkerProc(LPVOID param) {
        auto* c = static_cast<WorkerContext*>(param);
        while (WaitForSingleObject(c->hWorkEvent, INFINITE) == WAIT_OBJECT_0) {
            if (c->abandoned.load()) break;
            HANDLE h = c->currentHandle;
            c->resultLen = 0;
            if (h && h != INVALID_HANDLE_VALUE) {
                DWORD ft = GetFileType(h);
                if (!c->abandoned.load() && ft == FILE_TYPE_DISK) {
                    DWORD len = GetFinalPathNameByHandleW(h, c->resultBuf, 1020, FILE_NAME_NORMALIZED);
                    if (len > 0 && len < 1020) {
                        c->resultLen = len;
                    }
                }
            }
            if (c->abandoned.load()) break;
            SetEvent(c->hDoneEvent);
        }
        delete c;
        return 0;
    }

    std::mutex mutex_;
    HANDLE hWorker_ = nullptr;
    WorkerContext* ctx_ = nullptr;
};

USHORT GetFileObjectTypeIndex(const SYSTEM_HANDLE_INFORMATION_EX* info) {
    static USHORT cachedIdx = 0;
    if (cachedIdx != 0) return cachedIdx;

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    HANDLE hDummy = CreateFileW(
        sysDir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hDummy == INVALID_HANDLE_VALUE) return 0;

    ULONG_PTR myPid = GetCurrentProcessId();
    ULONG_PTR myHandle = reinterpret_cast<ULONG_PTR>(hDummy);
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& e = info->Handles[i];
        if (e.UniqueProcessId == myPid && e.HandleValue == myHandle) {
            cachedIdx = e.ObjectTypeIndex;
            break;
        }
    }
    CloseHandle(hDummy);
    return cachedIdx;
}

std::wstring NormalizeTargetDosPath(const std::wstring& inputPath, bool& outIsDirectory) {
    outIsDirectory = false;
    std::wstring clean = TrimTrailingSlashes(StripExtendedPathPrefix(inputPath));
    if (clean.empty()) return clean;

    DWORD attrs = GetFileAttributesW(clean.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        outIsDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    HANDLE h = CreateFileW(
        clean.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[1024];
        DWORD len = GetFinalPathNameByHandleW(h, buf, 1020, FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (len > 0 && len < 1020) {
            return TrimTrailingSlashes(StripExtendedPathPrefix(std::wstring(buf, len)));
        }
    }

    wchar_t fullBuf[1024];
    DWORD fullLen = GetFullPathNameW(clean.c_str(), 1020, fullBuf, nullptr);
    if (fullLen > 0 && fullLen < 1020) {
        return TrimTrailingSlashes(StripExtendedPathPrefix(std::wstring(fullBuf, fullLen)));
    }
    return clean;
}

bool MatchesTargetOrInsideDirectory(
    const std::wstring& candidateDosPath,
    const std::wstring& targetDosPath,
    bool targetIsDirectory,
    std::wstring& outRelativeSubPath)
{
    outRelativeSubPath.clear();
    std::wstring cand = TrimTrailingSlashes(StripExtendedPathPrefix(candidateDosPath));
    if (EqualsCaseInsensitive(cand, targetDosPath)) return true;

    if (targetIsDirectory && cand.size() > targetDosPath.size() + 1 &&
        StartsWithCaseInsensitive(cand, targetDosPath))
    {
        wchar_t sep = cand[targetDosPath.size()];
        if (sep == L'\\' || sep == L'/') {
            outRelativeSubPath = cand.substr(targetDosPath.size() + 1);
            return true;
        }
    }
    return false;
}

std::wstring ExtractFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool QueryProcessImagePath(DWORD pid, std::wstring& outPath) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    wchar_t buf[1024];
    DWORD len = 1020;
    bool ok = (QueryFullProcessImageNameW(hProc, 0, buf, &len) != FALSE);
    if (ok) outPath.assign(buf, len);
    CloseHandle(hProc);
    return ok;
}

void EnableDebugPrivilege() {
    static std::once_flag once;
    std::call_once(once, []() {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return;
        TOKEN_PRIVILEGES tp = {};
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(hToken);
    });
}

bool IsCriticalSystemProcess(DWORD pid, const std::wstring& procName) {
    if (pid <= 4) return true;
    static constexpr const wchar_t* kCritical[] = {
        L"system", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"svchost.exe", L"winlogon.exe",
        L"dwm.exe", L"fontdrvhost.exe", L"memory compression", L"registry",
        L"spoolsv.exe", L"searchindexer.exe", L"msmpeng.exe",
        L"sihost.exe", L"taskhostw.exe", L"runtimebroker.exe",
        L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
        L"searchhost.exe", L"textinputhost.exe", L"ctfmon.exe",
        L"audiodg.exe", L"dllhost.exe", L"conhost.exe", L"wmiprvse.exe"
    };
    for (const wchar_t* name : kCritical) {
        if (_wcsicmp(procName.c_str(), name) == 0) return true;
    }
    return false;
}

bool IsPipeHeavySystemProcess(const std::wstring& exeName) {
    static constexpr const wchar_t* kSkip[] = {
        L"csrss.exe", L"lsass.exe", L"services.exe", L"smss.exe", L"wininit.exe", L"dwm.exe"
    };
    for (const wchar_t* name : kSkip) {
        if (_wcsicmp(exeName.c_str(), name) == 0) return true;
    }
    return false;
}

HWND FindMainWindowForPid(DWORD pid) {
    struct EnumCtx { DWORD pid; HWND hwnd; } ctx{ pid, nullptr };
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lParam);
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
            DWORD_PTR len = 0;
            if (SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 12, &len) && len > 0) {
                c->hwnd = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

std::wstring GetFriendlyAppDescription(HWND mainWindow, const std::wstring& imagePath, const std::wstring& fallbackName) {
    if (!imagePath.empty()) {
        DWORD dummy = 0;
        DWORD verSize = GetFileVersionInfoSizeW(imagePath.c_str(), &dummy);
        if (verSize > 0) {
            std::vector<BYTE> verBuf(verSize);
            if (GetFileVersionInfoW(imagePath.c_str(), 0, verSize, verBuf.data())) {
                struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; }* lpTrans = nullptr;
                UINT cbTrans = 0;
                if (VerQueryValueW(verBuf.data(), L"\\VarFileInfo\\Translation",
                                   reinterpret_cast<LPVOID*>(&lpTrans), &cbTrans) &&
                    cbTrans >= sizeof(LANGANDCODEPAGE))
                {
                    wchar_t subBlock[128];
                    wsprintfW(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                              lpTrans[0].wLanguage, lpTrans[0].wCodePage);
                    LPWSTR desc = nullptr;
                    UINT descLen = 0;
                    if (VerQueryValueW(verBuf.data(), subBlock, reinterpret_cast<LPVOID*>(&desc), &descLen) &&
                        desc && descLen > 1 && wcslen(desc) > 0)
                    {
                        return std::wstring(desc);
                    }
                }
            }
        }
    }

    if (mainWindow) {
        wchar_t title[256] = {};
        DWORD_PTR copied = 0;
        if (SendMessageTimeoutW(mainWindow, WM_GETTEXT, 250, reinterpret_cast<LPARAM>(title),
                                SMTO_ABORTIFHUNG | SMTO_NORMAL, 15, &copied) &&
            copied > 0)
        {
            return std::wstring(title);
        }
    }
    return fallbackName;
}

template <typename Callback>
void ScanKernelFileHandles(
    const std::wstring& normTarget,
    bool isDirectory,
    bool ignoreCurrentProcess,
    const std::set<DWORD>& filterPids,
    Callback&& onMatch)
{
    static auto NtQuerySysInfo = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!NtQuerySysInfo) return;

    ULONG bufSize = 2 * 1024 * 1024;
    std::vector<BYTE> buffer(bufSize);
    ULONG returnLen = 0;
    NTSTATUS status = kStatusInfoLengthMismatch;

    for (int attempt = 0; attempt < 6; ++attempt) {
        status = NtQuerySysInfo(kSystemExtendedHandleInformation, buffer.data(), bufSize, &returnLen);
        if (status != kStatusInfoLengthMismatch) break;
        bufSize = (returnLen > bufSize) ? (returnLen + 512 * 1024) : (bufSize * 2);
        buffer.resize(bufSize);
    }
    if (status < 0) return;

    const auto* handleInfo = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX*>(buffer.data());
    USHORT fileTypeIdx = GetFileObjectTypeIndex(handleInfo);
    DWORD currentPid = GetCurrentProcessId();

    std::map<DWORD, std::vector<ULONG_PTR>> pidHandles;
    for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; ++i) {
        const auto& e = handleInfo->Handles[i];
        if (fileTypeIdx != 0 && e.ObjectTypeIndex != fileTypeIdx) continue;
        DWORD pid = static_cast<DWORD>(e.UniqueProcessId);
        if (pid <= 4) continue;
        if (ignoreCurrentProcess && pid == currentPid) continue;
        if (!filterPids.empty() && filterPids.count(pid) == 0) continue;
        pidHandles[pid].push_back(e.HandleValue);
    }

    HANDLE hSelf = GetCurrentProcess();
    int totalTimeouts = 0;

    for (const auto& [pid, handles] : pidHandles) {
        if (filterPids.empty()) {
            std::wstring imgPath;
            if (QueryProcessImagePath(pid, imgPath) && IsPipeHeavySystemProcess(ExtractFileName(imgPath))) {
                continue;
            }
        }

        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!hProc) continue;

        int procTimeouts = 0;
        for (ULONG_PTR hVal : handles) {
            HANDLE hDup = nullptr;
            if (DuplicateHandle(hProc, reinterpret_cast<HANDLE>(hVal), hSelf, &hDup, 0, FALSE, 0)) {
                std::wstring resolvedPath;
                bool timedOut = false;
                if (SafePathResolver::Instance().QueryFilePath(hDup, resolvedPath, timedOut)) {
                    std::wstring relPath;
                    if (MatchesTargetOrInsideDirectory(resolvedPath, normTarget, isDirectory, relPath)) {
                        onMatch(pid, relPath, hVal);
                    }
                }
                CloseHandle(hDup);

                if (timedOut) {
                    ++totalTimeouts;
                    if (++procTimeouts >= 6) break;
                }
            }
        }
        CloseHandle(hProc);
        if (totalTimeouts >= 24) break;
    }
}

bool RequestElevatedServiceUnlock(
    const std::wstring& targetPath,
    UnlockMode mode,
    uint32_t* outClosedHandles,
    uint32_t* outTerminatedProcs)
{
    if (SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE hSvc = OpenServiceW(hScm, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS)) {
            StartServiceW(hSvc, 0, nullptr);
            CloseServiceHandle(hSvc);
        }
        CloseServiceHandle(hScm);
    }

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    DWORD deadline = GetTickCount() + 800;
    for (;;) {
        hPipe = CreateFileW(kServicePipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        DWORD remaining = deadline - GetTickCount();
        if (static_cast<int>(remaining) <= 0) return false;
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(kServicePipeName, remaining)) return false;
        } else if (err == ERROR_FILE_NOT_FOUND) {
            Sleep(25);
        } else {
            return false;
        }
    }

    ElevationUnlockRequest req = {};
    req.magic = 0x484F4646;
    req.mode = static_cast<uint32_t>(mode);
    wcsncpy_s(req.targetPath, targetPath.c_str(), _TRUNCATE);

    DWORD ioBytes = 0;
    ElevationUnlockResponse resp = {};
    bool ok = false;
    if (WriteFile(hPipe, &req, sizeof(req), &ioBytes, nullptr) && ioBytes == sizeof(req) &&
        ReadFile(hPipe, &resp, sizeof(resp), &ioBytes, nullptr) && ioBytes == sizeof(resp))
    {
        ok = (resp.success != 0);
        if (outClosedHandles)   *outClosedHandles += resp.closedHandles;
        if (outTerminatedProcs) *outTerminatedProcs += resp.terminatedProcesses;
    }
    CloseHandle(hPipe);
    return ok;
}

std::mutex g_cacheMutex;
std::wstring g_cachedPath;
bool g_cachedIgnoreSelf = false;
DWORD g_cachedTick = 0;
std::vector<LockedProcessInfo> g_cachedResults;

} // namespace

std::vector<LockedProcessInfo> LockFinder::FindLocks(const std::wstring& targetPath, bool ignoreCurrentProcess) {
    if (targetPath.empty()) return {};
    EnableDebugPrivilege();

    bool isDirectory = false;
    std::wstring normTarget = NormalizeTargetDosPath(targetPath, isDirectory);
    if (normTarget.empty()) return {};

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (GetTickCount() - g_cachedTick < 1200 &&
            g_cachedIgnoreSelf == ignoreCurrentProcess &&
            EqualsCaseInsensitive(g_cachedPath, normTarget) &&
            !g_cachedResults.empty())
        {
            return g_cachedResults;
        }
    }

    DWORD currentPid = GetCurrentProcessId();
    std::map<DWORD, LockedProcessInfo> foundMap;

    auto addOrUpdate = [&](DWORD pid, const std::wstring& relPath, ULONG_PTR handleVal, const std::wstring& rmAppName) {
        if (pid == 0 || (ignoreCurrentProcess && pid == currentPid)) return;

        auto [it, inserted] = foundMap.try_emplace(pid);
        if (inserted) {
            LockedProcessInfo& info = it->second;
            info.pid = pid;
            std::wstring imagePath;
            if (QueryProcessImagePath(pid, imagePath)) {
                info.processName = ExtractFileName(imagePath);
            }
            if (info.processName.empty()) {
                info.processName = !rmAppName.empty() ? rmAppName : L"System";
            }
            info.mainWindow = FindMainWindowForPid(pid);
            info.appDescription = GetFriendlyAppDescription(
                info.mainWindow, imagePath, !rmAppName.empty() ? rmAppName : info.processName);
            info.isSystemCritical = IsCriticalSystemProcess(pid, info.processName);
            info.isSelfExplorer = (pid == currentPid) || (_wcsicmp(info.processName.c_str(), L"explorer.exe") == 0);
            info.lockedRelativePath = relPath;
            if (handleVal != 0) info.handleValues.push_back(handleVal);
        } else {
            if (it->second.lockedRelativePath.empty() && !relPath.empty()) {
                it->second.lockedRelativePath = relPath;
            }
            if (handleVal != 0) {
                auto& hv = it->second.handleValues;
                if (std::find(hv.begin(), hv.end(), handleVal) == hv.end()) {
                    hv.push_back(handleVal);
                }
            }
        }
    };

    // Stage 1: Running executables inside target path
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID <= 4) continue;
                std::wstring imgPath;
                std::wstring rel;
                if (QueryProcessImagePath(pe.th32ProcessID, imgPath) &&
                    MatchesTargetOrInsideDirectory(imgPath, normTarget, isDirectory, rel))
                {
                    addOrUpdate(pe.th32ProcessID, rel, 0, pe.szExeFile);
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    // Stage 2: Windows Restart Manager API
    if (isDirectory || foundMap.empty()) {
        std::vector<std::wstring> rmFiles;
        if (!isDirectory) {
            rmFiles.push_back(normTarget);
        } else {
            WIN32_FIND_DATAW fd = {};
            HANDLE hFind = FindFirstFileExW((normTarget + L"\\*").c_str(), FindExInfoBasic, &fd,
                                            FindExSearchNameMatch, nullptr, 0);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        rmFiles.push_back(normTarget + L"\\" + fd.cFileName);
                        if (rmFiles.size() >= 24) break;
                    }
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
        }

        if (!rmFiles.empty()) {
            DWORD rmSession = 0;
            wchar_t sessionKey[CCH_RM_SESSION_KEY + 1] = {};
            if (RmStartSession(&rmSession, 0, sessionKey) == ERROR_SUCCESS) {
                std::vector<LPCWSTR> ptrs;
                ptrs.reserve(rmFiles.size());
                for (const auto& s : rmFiles) ptrs.push_back(s.c_str());

                if (RmRegisterResources(rmSession, static_cast<UINT>(ptrs.size()), ptrs.data(),
                                        0, nullptr, 0, nullptr) == ERROR_SUCCESS)
                {
                    UINT needed = 0, count = 16;
                    DWORD reasons = RmRebootReasonNone;
                    std::vector<RM_PROCESS_INFO> rmInfos(count);
                    DWORD rmRes = RmGetList(rmSession, &needed, &count, rmInfos.data(), &reasons);
                    if (rmRes == ERROR_MORE_DATA) {
                        count = needed;
                        rmInfos.resize(count);
                        rmRes = RmGetList(rmSession, &needed, &count, rmInfos.data(), &reasons);
                    }
                    if (rmRes == ERROR_SUCCESS) {
                        for (UINT i = 0; i < count; ++i) {
                            addOrUpdate(rmInfos[i].Process.dwProcessId, L"", 0, rmInfos[i].strAppName);
                        }
                    }
                }
                RmEndSession(rmSession);
            }
        }
    }

    // Stage 3: Native NT kernel handle enumeration (filtered to known PIDs for single files)
    std::set<DWORD> filterPids;
    if (!isDirectory && !foundMap.empty()) {
        for (const auto& [pid, _] : foundMap) filterPids.insert(pid);
    }

    ScanKernelFileHandles(normTarget, isDirectory, ignoreCurrentProcess, filterPids,
        [&](DWORD pid, const std::wstring& relPath, ULONG_PTR handleVal) {
            addOrUpdate(pid, relPath, handleVal, L"");
        });

    if (foundMap.size() > 1) {
        foundMap.erase(currentPid);
    }

    std::vector<LockedProcessInfo> results;
    results.reserve(foundMap.size());
    for (auto& [_, info] : foundMap) {
        results.push_back(std::move(info));
    }

    std::sort(results.begin(), results.end(), [](const LockedProcessInfo& a, const LockedProcessInfo& b) {
        if (a.isSelfExplorer != b.isSelfExplorer) return !a.isSelfExplorer;
        if (a.isSystemCritical != b.isSystemCritical) return !a.isSystemCritical;
        return a.pid < b.pid;
    });

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cachedPath = normTarget;
        g_cachedIgnoreSelf = ignoreCurrentProcess;
        g_cachedTick = GetTickCount();
        g_cachedResults = results;
    }
    return results;
}

std::wstring LockFinder::FormatBlockerSummary(const std::vector<LockedProcessInfo>& locks) {
    if (locks.empty()) return L"Unknown Background Process";

    std::wstring out;
    size_t limit = std::min<size_t>(locks.size(), 2);
    for (size_t i = 0; i < limit; ++i) {
        if (i > 0) out += L"; ";
        const auto& l = locks[i];
        if (!l.appDescription.empty() && !EqualsCaseInsensitive(l.appDescription, l.processName)) {
            out += l.appDescription + L" (" + l.processName + L", PID " + std::to_wstring(l.pid) + L")";
        } else {
            out += l.processName + L" (PID " + std::to_wstring(l.pid) + L")";
        }
        if (!l.lockedRelativePath.empty()) {
            out += L" [" + l.lockedRelativePath + L"]";
        }
    }
    if (locks.size() > limit) {
        out += L" (+" + std::to_wstring(locks.size() - limit) + L" more)";
    }
    return out;
}

bool LockFinder::ShouldUseHandleOnlyUnlock(const std::vector<LockedProcessInfo>& locks) {
    if (locks.empty()) return true;
    return std::all_of(locks.begin(), locks.end(), [](const LockedProcessInfo& l) {
        return l.isSelfExplorer || l.isSystemCritical;
    });
}

bool LockFinder::UnlockTarget(
    const std::wstring& targetPath,
    const std::vector<LockedProcessInfo>& initialLocks,
    UnlockMode mode,
    uint32_t* outClosedHandles,
    uint32_t* outTerminatedProcs,
    bool allowElevationFallback)
{
    EnableDebugPrivilege();

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cachedTick = 0;
        g_cachedResults.clear();
    }

    uint32_t closedCount = 0;
    uint32_t killedCount = 0;
    bool needElevationFallback = false;

    std::vector<LockedProcessInfo> locks = initialLocks.empty()
        ? FindLocks(targetPath, false)
        : initialLocks;

    HANDLE hSelf = GetCurrentProcess();
    DWORD selfPid = GetCurrentProcessId();

    for (const auto& proc : locks) {
        if (!proc.handleValues.empty()) {
            HANDLE hDupProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, proc.pid);
            if (hDupProc) {
                for (ULONG_PTR hVal : proc.handleValues) {
                    HANDLE hClosed = nullptr;
                    if (DuplicateHandle(hDupProc, reinterpret_cast<HANDLE>(hVal), hSelf, &hClosed,
                                        0, FALSE, DUPLICATE_CLOSE_SOURCE))
                    {
                        if (hClosed) CloseHandle(hClosed);
                        ++closedCount;
                    }
                }
                CloseHandle(hDupProc);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                needElevationFallback = true;
            }
        } else if (!proc.isSelfExplorer && proc.pid != selfPid) {
            HANDLE hProbe = OpenProcess(PROCESS_DUP_HANDLE, FALSE, proc.pid);
            if (hProbe) {
                CloseHandle(hProbe);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                needElevationFallback = true;
            }
        }

        if (mode == UnlockMode::KillProcessAndCloseHandles &&
            !proc.isSelfExplorer && !proc.isSystemCritical && proc.pid != selfPid)
        {
            HANDLE hTerm = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, proc.pid);
            if (hTerm) {
                if (TerminateProcess(hTerm, 1)) {
                    WaitForSingleObject(hTerm, 400);
                    ++killedCount;
                }
                CloseHandle(hTerm);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                needElevationFallback = true;
            }
        }
    }

    if (needElevationFallback && allowElevationFallback) {
        RequestElevatedServiceUnlock(targetPath, mode, &closedCount, &killedCount);
    }

    if (outClosedHandles)   *outClosedHandles = closedCount;
    if (outTerminatedProcs) *outTerminatedProcs = killedCount;
    return (closedCount > 0 || killedCount > 0);
}

void LockFinder::Shutdown() {
    SafePathResolver::Instance().Shutdown();
}

} // namespace HandsOff
