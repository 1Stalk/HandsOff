#include "../src/common.h"
#include "../src/dialog_enhancer.h"
#include "../src/file_in_use_hook.h"
#include "../src/lock_finder.h"

#include <atomic>
#include <iostream>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <string>
#include <vector>

using namespace HandsOff;

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "[FAIL] Line " << line << ": " << expr << "\n";
        ++g_failures;
    }
}

#define CHECK(expr) Check(!!(expr), #expr, __LINE__)

int RunHoldFileWorker(const wchar_t* filePath, const wchar_t* readyEventName) {
    HANDLE hFile = CreateFileW(
        filePath,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return 2;
    }

    HANDLE hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName);
    if (hReady) {
        SetEvent(hReady);
        CloseHandle(hReady);
    }

    // Bounded wait so an orphaned worker never lingers indefinitely.
    Sleep(30000);
    CloseHandle(hFile);
    return 0;
}

void TestStringAndPathHelpers() {
    CHECK(StartsWithCaseInsensitive(L"C:\\Windows\\System32", L"c:\\windows"));
    CHECK(StartsWithCaseInsensitive(L"abc", L""));
    CHECK(StartsWithCaseInsensitive(L"", L""));
    CHECK(!StartsWithCaseInsensitive(L"ab", L"abc"));
    CHECK(!StartsWithCaseInsensitive(L"C:\\Users", L"C:\\Windows"));

    CHECK(EqualsCaseInsensitive(L"Explorer.EXE", L"explorer.exe"));
    CHECK(EqualsCaseInsensitive(L"", L""));
    CHECK(!EqualsCaseInsensitive(L"foo", L"foobar"));
    CHECK(!EqualsCaseInsensitive(L"foo", L"bar"));

    CHECK(StripExtendedPathPrefix(L"\\\\?\\C:\\Temp\\file.txt") == L"C:\\Temp\\file.txt");
    CHECK(StripExtendedPathPrefix(L"\\\\?\\UNC\\server\\share\\file.txt") == L"\\\\server\\share\\file.txt");
    CHECK(StripExtendedPathPrefix(L"C:\\Temp\\file.txt") == L"C:\\Temp\\file.txt");
    CHECK(StripExtendedPathPrefix(L"") == L"");

    CHECK(TrimTrailingSlashes(L"C:\\") == L"C:\\");
    CHECK(TrimTrailingSlashes(L"C:\\Temp\\") == L"C:\\Temp");
    CHECK(TrimTrailingSlashes(L"C:\\Temp///\\\\") == L"C:\\Temp");
    CHECK(TrimTrailingSlashes(L"C:\\Temp") == L"C:\\Temp");
    CHECK(TrimTrailingSlashes(L"") == L"");

    CHECK(sizeof(ElevationUnlockRequest) == sizeof(uint32_t) * 2 + 1024 * sizeof(wchar_t));
    CHECK(sizeof(ElevationUnlockResponse) == sizeof(uint32_t) * 4);
}

void TestFormatBlockerSummary() {
    CHECK(LockFinder::FormatBlockerSummary({}) == L"Unknown Background Process");

    LockedProcessInfo p1;
    p1.pid = 1234;
    p1.processName = L"Code.exe";
    p1.appDescription = L"Visual Studio Code";
    p1.lockedRelativePath = L"src\\main.cpp";
    CHECK(
        LockFinder::FormatBlockerSummary({ p1 }) ==
        L"Visual Studio Code (Code.exe, PID 1234) [src\\main.cpp]");

    LockedProcessInfo p2;
    p2.pid = 5678;
    p2.processName = L"notepad.exe";
    p2.appDescription = L"NOTEPAD.EXE";
    CHECK(LockFinder::FormatBlockerSummary({ p2 }) == L"notepad.exe (PID 5678)");

    LockedProcessInfo p3;
    p3.pid = 9001;
    p3.processName = L"cmd.exe";

    LockedProcessInfo p4;
    p4.pid = 9002;
    p4.processName = L"pwsh.exe";

    std::wstring multi = LockFinder::FormatBlockerSummary({ p1, p2, p3, p4 });
    CHECK(multi.find(L"Visual Studio Code (Code.exe, PID 1234) [src\\main.cpp]; notepad.exe (PID 5678) (+2 more)") == 0);
}

void TestShouldUseHandleOnlyUnlock() {
    CHECK(LockFinder::ShouldUseHandleOnlyUnlock({}));

    LockedProcessInfo sysProc;
    sysProc.pid = 4;
    sysProc.isSystemCritical = true;

    LockedProcessInfo expProc;
    expProc.pid = 200;
    expProc.isSelfExplorer = true;

    LockedProcessInfo appProc;
    appProc.pid = 300;
    appProc.processName = L"app.exe";

    CHECK(LockFinder::ShouldUseHandleOnlyUnlock({ sysProc }));
    CHECK(LockFinder::ShouldUseHandleOnlyUnlock({ sysProc, expProc }));
    CHECK(!LockFinder::ShouldUseHandleOnlyUnlock({ sysProc, appProc }));
    CHECK(!LockFinder::ShouldUseHandleOnlyUnlock({ appProc }));
}

bool SpawnLockHolder(
    const std::wstring& filePath,
    const std::wstring& eventName,
    PROCESS_INFORMATION& outPi,
    HANDLE& outEvent)
{
    outPi = {};
    outEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!outEvent) return false;

    wchar_t selfExe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, selfExe, MAX_PATH)) {
        CloseHandle(outEvent);
        outEvent = nullptr;
        return false;
    }

    std::wstring cmdLine = L"\"" + std::wstring(selfExe) + L"\" --hold-file \"" + filePath + L"\" \"" + eventName + L"\"";
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (!CreateProcessW(selfExe, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &outPi)) {
        CloseHandle(outEvent);
        outEvent = nullptr;
        return false;
    }

    DWORD waitRes = WaitForSingleObject(outEvent, 5000);
    if (waitRes != WAIT_OBJECT_0) {
        TerminateProcess(outPi.hProcess, 1);
        WaitForSingleObject(outPi.hProcess, 1000);
        CloseHandle(outPi.hThread);
        CloseHandle(outPi.hProcess);
        CloseHandle(outEvent);
        outPi = {};
        outEvent = nullptr;
        return false;
    }
    return true;
}

void CleanupChild(PROCESS_INFORMATION& pi, HANDLE& hEvent) {
    if (pi.hProcess) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 1000);
        }
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
    }
    if (hEvent) {
        CloseHandle(hEvent);
        hEvent = nullptr;
    }
}

void TestFindLocksAndUnlockIntegration() {
    wchar_t tempBase[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempBase);
    CHECK(len > 0 && len < MAX_PATH);
    if (len == 0) return;

    std::wstring tempDir = std::wstring(tempBase) + L"HandsOffTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                           std::to_wstring(GetTickCount());
    CHECK(CreateDirectoryW(tempDir.c_str(), nullptr) != FALSE);

    // Sub-test A: KillProcessAndCloseHandles
    {
        std::wstring targetFile = tempDir + L"\\kill_target.txt";
        std::wstring eventName = L"Local\\HandsOffTestReadyKill_" + std::to_wstring(GetCurrentProcessId());
        PROCESS_INFORMATION pi = {};
        HANDLE hReady = nullptr;

        if (SpawnLockHolder(targetFile, eventName, pi, hReady)) {
            auto locks = LockFinder::FindLocks(targetFile, true);
            bool foundChild = false;
            for (const auto& l : locks) {
                if (l.pid == pi.dwProcessId) {
                    foundChild = true;
                    break;
                }
            }
            CHECK(foundChild);
            CHECK(LockFinder::FormatBlockerSummary(locks).find(std::to_wstring(pi.dwProcessId)) != std::wstring::npos);

            uint32_t closed = 0, killed = 0;
            bool unlocked = LockFinder::UnlockTarget(
                targetFile, locks, UnlockMode::KillProcessAndCloseHandles, &closed, &killed, false);
            CHECK(unlocked);
            CHECK(closed > 0 || killed > 0);
            CHECK(DeleteFileW(targetFile.c_str()) != FALSE);
        } else {
            CHECK(false && "Failed to spawn lock-holder child process (kill test)");
        }
        CleanupChild(pi, hReady);
    }

    // Sub-test B: CloseHandlesOnly
    {
        std::wstring targetFile = tempDir + L"\\handle_only_target.txt";
        std::wstring eventName = L"Local\\HandsOffTestReadyHandle_" + std::to_wstring(GetCurrentProcessId());
        PROCESS_INFORMATION pi = {};
        HANDLE hReady = nullptr;

        if (SpawnLockHolder(targetFile, eventName, pi, hReady)) {
            auto locks = LockFinder::FindLocks(targetFile, true);
            bool foundChildWithHandle = false;
            for (const auto& l : locks) {
                if (l.pid == pi.dwProcessId && !l.handleValues.empty()) {
                    foundChildWithHandle = true;
                    break;
                }
            }
            CHECK(foundChildWithHandle);

            uint32_t closed = 0, killed = 0;
            bool unlocked = LockFinder::UnlockTarget(
                targetFile, locks, UnlockMode::CloseHandlesOnly, &closed, &killed, false);
            CHECK(unlocked);
            CHECK(closed > 0);
            CHECK(killed == 0);
            CHECK(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT);
            CHECK(DeleteFileW(targetFile.c_str()) != FALSE);
        } else {
            CHECK(false && "Failed to spawn lock-holder child process (handle-only test)");
        }
        CleanupChild(pi, hReady);
    }

    // Sub-test C: Folder lock resolution (nested file sub-item) + CloseHandlesOnly
    {
        std::wstring folderTarget = tempDir + L"\\folder_target";
        std::wstring subDir = folderTarget + L"\\sub";
        std::wstring nestedFile = subDir + L"\\nested.txt";
        CHECK(CreateDirectoryW(folderTarget.c_str(), nullptr) != FALSE);
        CHECK(CreateDirectoryW(subDir.c_str(), nullptr) != FALSE);

        std::wstring eventName = L"Local\\HandsOffTestReadyFolder_" + std::to_wstring(GetCurrentProcessId());
        PROCESS_INFORMATION pi = {};
        HANDLE hReady = nullptr;

        if (SpawnLockHolder(nestedFile, eventName, pi, hReady)) {
            auto locks = LockFinder::FindLocks(folderTarget, true);
            bool foundFolderLocker = false;
            for (const auto& l : locks) {
                if (l.pid == pi.dwProcessId && EqualsCaseInsensitive(l.lockedRelativePath, L"sub\\nested.txt")) {
                    foundFolderLocker = true;
                    break;
                }
            }
            CHECK(foundFolderLocker);
            CHECK(LockFinder::FormatBlockerSummary(locks).find(L"[sub\\nested.txt]") != std::wstring::npos);

            uint32_t closed = 0, killed = 0;
            bool unlocked = LockFinder::UnlockTarget(
                folderTarget, locks, UnlockMode::CloseHandlesOnly, &closed, &killed, false);
            CHECK(unlocked);
            CHECK(closed > 0);
            CHECK(killed == 0);
            CHECK(DeleteFileW(nestedFile.c_str()) != FALSE);
        } else {
            CHECK(false && "Failed to spawn lock-holder child process (folder test)");
        }
        CleanupChild(pi, hReady);
        RemoveDirectoryW(subDir.c_str());
        RemoveDirectoryW(folderTarget.c_str());
    }

    CHECK(LockFinder::FindLocks(tempDir + L"\\nonexistent_target_98765", true).empty());

    RemoveDirectoryW(tempDir.c_str());
}

std::wstring LoadShell32TestString(UINT id) {
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (!hShell32) return {};
    wchar_t buf[512] = {};
    int len = LoadStringW(hShell32, id, buf, 511);
    return (len > 0) ? std::wstring(buf, static_cast<size_t>(len)) : L"";
}

std::wstring FormatShell32Template1(const std::wstring& tmpl, const std::wstring& arg1) {
    size_t pos = tmpl.find(L"%1");
    if (pos == std::wstring::npos) return tmpl;
    std::wstring out = tmpl;
    out.replace(pos, 2, arg1);
    return out;
}

void TestDialogEnhancerConflictScopingAndVerification() {
    DialogEnhancer::ResetConflictStateForTests();

    auto ui = DialogEnhancer::LoadFileInUseUiStrings();
    CHECK(!ui.tryAgainLabel.empty());
    CHECK(!ui.templates.empty());

    std::wstring fileInUseTitle = LoadShell32TestString(0x4142);
    std::wstring folderInUseTitle = LoadShell32TestString(0x4143);
    std::wstring fileTemplate = LoadShell32TestString(0x41AC);
    std::wstring folderTemplate = LoadShell32TestString(0x41B0);
    std::wstring accessDeniedTitle = LoadShell32TestString(0x41B8);
    std::wstring fileAccessDeniedTitle = LoadShell32TestString(0x4144);
    std::wstring accessDeniedDesc = LoadShell32TestString(0x41B5);
    std::wstring pathTooLongTitle = LoadShell32TestString(0x41BC);
    std::wstring pathTooLongDesc = LoadShell32TestString(0x41BD);

    LockedProcessInfo locker1;
    locker1.pid = 4242;
    locker1.processName = L"python.exe";
    locker1.appDescription = L"Python";

    DialogEnhancer::OnConflictDetected(L"D:\\Dst\\target.db", { locker1 });

    DialogEnhancer::ConflictSnapshot snap1;
    DWORD t0 = GetTickCount();
    CHECK(DialogEnhancer::TryGetPendingConflict(t0, snap1));
    CHECK(snap1.seq > 0);
    CHECK(snap1.blockerSummary == L"Python (python.exe, PID 4242)");

    // Positive: genuine File In Use dialog with matching blocker summary
    std::wstring fileDesc = FormatShell32Template1(fileTemplate, snap1.blockerSummary);
    CHECK(DialogEnhancer::IsFileInUseInterruptDialog(
        fileInUseTitle, { fileDesc }, snap1, ui));

    // Positive: genuine Folder In Use dialog with matching blocker summary
    std::wstring folderDesc = FormatShell32Template1(folderTemplate, snap1.blockerSummary);
    CHECK(DialogEnhancer::IsFileInUseInterruptDialog(
        folderInUseTitle, { folderDesc }, snap1, ui));

    // Positive: multi-tile OperationStatusWindow title ("1 Interrupted Action"), but File-In-Use text matches
    CHECK(DialogEnhancer::IsFileInUseInterruptDialog(
        L"1 Interrupted Action", { fileDesc }, snap1, ui));

    // Negative: Move-with-replace ERROR_ACCESS_DENIED dialog ("Destination Folder Access Denied" / "File Access Denied")
    CHECK(!DialogEnhancer::IsFileInUseInterruptDialog(
        accessDeniedTitle,
        { L"You'll need to provide administrator permission to move to this folder" },
        snap1, ui));
    CHECK(!DialogEnhancer::IsFileInUseInterruptDialog(
        fileAccessDeniedTitle,
        { accessDeniedDesc },
        snap1, ui));

    // Negative: Path Too Long or other unrelated "Try Again" dialog
    CHECK(!DialogEnhancer::IsFileInUseInterruptDialog(
        pathTooLongTitle, { pathTooLongDesc }, snap1, ui));

    // Negative: File In Use dialog for a different blocker / generic fallback
    std::wstring otherDesc = FormatShell32Template1(fileTemplate, L"notepad.exe (PID 9999)");
    CHECK(!DialogEnhancer::IsFileInUseInterruptDialog(
        fileInUseTitle, { otherDesc }, snap1, ui));

    // Negative: empty visibleTexts + non-FileInUse title; Positive: empty visibleTexts + FileInUse title
    CHECK(!DialogEnhancer::IsFileInUseInterruptDialog(accessDeniedTitle, {}, snap1, ui));
    if (!fileInUseTitle.empty()) {
        CHECK(DialogEnhancer::IsFileInUseInterruptDialog(fileInUseTitle, {}, snap1, ui));
    }

    // Claiming by first OperationStatusWindow prevents a second window from stealing the conflict
    HWND fakeWnd1 = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x12340));
    HWND fakeWnd2 = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x56780));
    DialogEnhancer::ConflictSnapshot claimed;
    CHECK(DialogEnhancer::TryClaimPendingConflict(snap1.seq, fakeWnd1, t0, claimed));
    CHECK(claimed.claimedByHwnd == fakeWnd1);

    DialogEnhancer::ConflictSnapshot secondAttempt;
    CHECK(!DialogEnhancer::TryGetPendingConflict(t0, secondAttempt));
    CHECK(!DialogEnhancer::TryClaimPendingConflict(snap1.seq, fakeWnd2, t0, secondAttempt));

    // Clearing the conflict on Unlock/Kill or dialog dismissal invalidates it immediately
    DialogEnhancer::ClearConflictIfSeq(snap1.seq);
    CHECK(!DialogEnhancer::TryGetPendingConflict(t0, secondAttempt));

    // Unclaimed pending conflict expires after kPendingConflictTtlMs
    DialogEnhancer::OnConflictDetected(L"D:\\Dst\\target2.db", { locker1 });
    DialogEnhancer::ConflictSnapshot snap2;
    CHECK(DialogEnhancer::TryGetPendingConflict(GetTickCount(), snap2));
    CHECK(!DialogEnhancer::TryGetPendingConflict(
        snap2.detectedTick + DialogEnhancer::kPendingConflictTtlMs + 1, snap2));

    // Lifecycle Initialize / Shutdown check
    DialogEnhancer::Initialize(nullptr);
    CHECK(DialogEnhancer::IsRunning());
    DialogEnhancer::Shutdown();
    CHECK(!DialogEnhancer::IsRunning());
    DialogEnhancer::ResetConflictStateForTests();
}

class MoveProbeProgressSink final : public IFileOperationProgressSink {
public:
    HRESULT lastMoveHr = S_OK;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
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
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hrMove, IShellItem*) override {
        lastMoveHr = hrMove;
        return S_OK;
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

private:
    ~MoveProbeProgressSink() = default;
    std::atomic<ULONG> refCount_{ 1 };
};

HRESULT RunSilentMoveOperation(const std::wstring& srcFile, const std::wstring& dstDir) {
    IFileOperation* pfo = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfo));
    if (FAILED(hr) || !pfo) return hr;

    auto* sink = new (std::nothrow) MoveProbeProgressSink();
    if (!sink) {
        pfo->Release();
        return E_OUTOFMEMORY;
    }

    DWORD cookie = 0;
    pfo->Advise(sink, &cookie);
    pfo->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);

    IShellItem* psiSrc = nullptr;
    IShellItem* psiDst = nullptr;
    HRESULT moveHr = E_FAIL;
    if (SUCCEEDED(SHCreateItemFromParsingName(srcFile.c_str(), nullptr, IID_PPV_ARGS(&psiSrc))) &&
        SUCCEEDED(SHCreateItemFromParsingName(dstDir.c_str(), nullptr, IID_PPV_ARGS(&psiDst))))
    {
        if (SUCCEEDED(pfo->MoveItem(psiSrc, psiDst, nullptr, nullptr))) {
            pfo->PerformOperations();
            moveHr = sink->lastMoveHr;
        }
    }

    if (psiSrc) psiSrc->Release();
    if (psiDst) psiDst->Release();
    pfo->Unadvise(cookie);
    sink->Release();
    pfo->Release();
    return moveHr;
}

void TestMoveWithReplaceSharingViolationHook() {
    constexpr HRESULT kCopyEngineAccessDeniedDest = static_cast<HRESULT>(0x80270022);
    constexpr HRESULT kCopyEngineSharingViolationDest = static_cast<HRESULT>(0x80270028);

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(coHr));
    if (FAILED(coHr)) return;

    wchar_t tempBase[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempBase);
    CHECK(len > 0 && len < MAX_PATH);
    if (len == 0) {
        CoUninitialize();
        return;
    }

    std::wstring tempDir = std::wstring(tempBase) + L"HandsOffMoveTest_" + std::to_wstring(GetCurrentProcessId()) +
                           L"_" + std::to_wstring(GetTickCount());
    std::wstring srcDir = tempDir + L"\\src";
    std::wstring dstDir = tempDir + L"\\dst";
    CHECK(CreateDirectoryW(tempDir.c_str(), nullptr) != FALSE);
    CHECK(CreateDirectoryW(srcDir.c_str(), nullptr) != FALSE);
    CHECK(CreateDirectoryW(dstDir.c_str(), nullptr) != FALSE);

    std::wstring srcFile = srcDir + L"\\catalog.db";
    std::wstring dstFile = dstDir + L"\\catalog.db";

    auto writeFile = [](const std::wstring& path, const char* content) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, content, static_cast<DWORD>(strlen(content)), &written, nullptr);
        CloseHandle(h);
        return ok != FALSE;
    };

    CHECK(writeFile(srcFile, "new_version"));
    CHECK(writeFile(dstFile, "locked_old"));

    std::wstring eventName = L"Local\\HandsOffMoveTestReady_" + std::to_wstring(GetCurrentProcessId());
    PROCESS_INFORMATION pi = {};
    HANDLE hReady = nullptr;
    if (SpawnLockHolder(dstFile, eventName, pi, hReady)) {
        // Without hook: same-volume MoveFileExW over a locked file yields COPYENGINE_E_ACCESS_DENIED_DEST.
        CHECK(!FileInUseHook::IsInstalled());
        CHECK(RunSilentMoveOperation(srcFile, dstDir) == kCopyEngineAccessDeniedDest);

        // With hook installed: MoveFileExW translates ERROR_ACCESS_DENIED -> ERROR_SHARING_VIOLATION,
        // and GetInfoForFileInUse(psiSrc) redirects to the locked destination path.
        CHECK(FileInUseHook::Install());
        CHECK(FileInUseHook::IsInstalled());
        CHECK(RunSilentMoveOperation(srcFile, dstDir) == kCopyEngineSharingViolationDest);

        using PFN_GetInfoForFileInUse = HRESULT(WINAPI*)(IShellItem*, IFileIsInUse**);
        HMODULE hWinStorage = GetModuleHandleW(L"windows.storage.dll");
        auto pGetInfo = hWinStorage
            ? reinterpret_cast<PFN_GetInfoForFileInUse>(GetProcAddress(hWinStorage, "GetInfoForFileInUse"))
            : nullptr;
        CHECK(pGetInfo != nullptr);
        if (pGetInfo) {
            DialogEnhancer::ResetConflictStateForTests();
            IShellItem* psiSrc = nullptr;
            CHECK(SUCCEEDED(SHCreateItemFromParsingName(srcFile.c_str(), nullptr, IID_PPV_ARGS(&psiSrc))));
            if (psiSrc) {
                IFileIsInUse* pInUse = nullptr;
                CHECK(SUCCEEDED(pGetInfo(psiSrc, &pInUse)) && pInUse != nullptr);
                if (pInUse) {
                    LPWSTR appName = nullptr;
                    CHECK(SUCCEEDED(pInUse->GetAppName(&appName)) && appName != nullptr);
                    if (appName) {
                        CHECK(std::wstring(appName).find(std::to_wstring(pi.dwProcessId)) != std::wstring::npos);
                        CoTaskMemFree(appName);
                    }
                    pInUse->Release();
                }
                DialogEnhancer::ConflictSnapshot snap;
                CHECK(DialogEnhancer::TryGetPendingConflict(GetTickCount(), snap));
                CHECK(EqualsCaseInsensitive(snap.targetPath, dstFile));
                DialogEnhancer::ResetConflictStateForTests();
                psiSrc->Release();
            }
        }

        // Uninstalling restores the original unhooked MoveFileExW behavior.
        FileInUseHook::Uninstall();
        CHECK(!FileInUseHook::IsInstalled());
        CHECK(RunSilentMoveOperation(srcFile, dstDir) == kCopyEngineAccessDeniedDest);

        // After unlocking, moving with replace succeeds cleanly (e.g. COPYENGINE_S_YES = 0x00270008).
        CleanupChild(pi, hReady);
        CHECK(FileInUseHook::Install());
        CHECK(SUCCEEDED(RunSilentMoveOperation(srcFile, dstDir)));
        CHECK(GetFileAttributesW(srcFile.c_str()) == INVALID_FILE_ATTRIBUTES);
        FileInUseHook::Uninstall();
    } else {
        CHECK(false && "Failed to spawn lock-holder child process (move test)");
    }

    CleanupChild(pi, hReady);
    DeleteFileW(srcFile.c_str());
    DeleteFileW(dstFile.c_str());
    RemoveDirectoryW(srcDir.c_str());
    RemoveDirectoryW(dstDir.c_str());
    RemoveDirectoryW(tempDir.c_str());
    CoUninitialize();
}

} // namespace

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 4 && _wcsicmp(argv[1], L"--hold-file") == 0) {
        int rc = RunHoldFileWorker(argv[2], argv[3]);
        LocalFree(argv);
        return rc;
    }
    if (argv) LocalFree(argv);

    TestStringAndPathHelpers();
    TestFormatBlockerSummary();
    TestShouldUseHandleOnlyUnlock();
    TestFindLocksAndUnlockIntegration();
    TestDialogEnhancerConflictScopingAndVerification();
    TestMoveWithReplaceSharingViolationHook();

    LockFinder::Shutdown();

    if (g_failures > 0) {
        std::cerr << "[HandsOffTests] FAILED (" << g_failures << " assertion(s) failed)\n";
        return 1;
    }
    std::cout << "[HandsOffTests] All tests passed.\n";
    return 0;
}
