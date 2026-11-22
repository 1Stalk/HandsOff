#pragma once

#include "common.h"
#include "lock_finder.h"

namespace HandsOff::DialogEnhancer {

inline constexpr DWORD kPendingConflictTtlMs = 10000;

struct ConflictSnapshot {
    uint64_t seq = 0;
    std::wstring targetPath;
    std::vector<LockedProcessInfo> locks;
    std::wstring blockerSummary;
    DWORD detectedTick = 0;
    HWND claimedByHwnd = nullptr;
    bool handleOnlyPrimary = false;
};

struct FileInUseTemplate {
    std::wstring prefix;
    std::wstring suffix;
};

struct FileInUseUiStrings {
    std::wstring tryAgainLabel;
    std::wstring fileInUseTitle;
    std::wstring folderInUseTitle;
    std::vector<FileInUseTemplate> templates;
};

void Initialize(HMODULE hModule);
void Shutdown();
bool IsRunning();
void OnConflictDetected(const std::wstring& targetPath, const std::vector<LockedProcessInfo>& locks);

FileInUseUiStrings LoadFileInUseUiStrings();
std::wstring NormalizeDialogText(const std::wstring& text);
bool IsFileInUseInterruptDialog(
    const std::wstring& windowTitle,
    const std::vector<std::wstring>& visibleTexts,
    const ConflictSnapshot& conflict,
    const FileInUseUiStrings& uiStrings);
bool TryGetPendingConflict(DWORD nowTick, ConflictSnapshot& outSnapshot);
bool TryClaimPendingConflict(uint64_t seq, HWND hwndOwner, DWORD nowTick, ConflictSnapshot& outSnapshot);
void ClearConflictIfSeq(uint64_t seq);
void ResetConflictStateForTests();

} // namespace HandsOff::DialogEnhancer
