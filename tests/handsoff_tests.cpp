#include "../src/common.h"
#include "../src/lock_finder.h"

#include <iostream>
#include <shellapi.h>
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

    LockFinder::Shutdown();

    if (g_failures > 0) {
        std::cerr << "[HandsOffTests] FAILED (" << g_failures << " assertion(s) failed)\n";
        return 1;
    }
    std::cout << "[HandsOffTests] All tests passed.\n";
    return 0;
}
