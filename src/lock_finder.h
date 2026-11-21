#pragma once

#include "common.h"

namespace HandsOff {

struct LockedProcessInfo {
    DWORD pid = 0;
    std::wstring processName;
    std::wstring appDescription;
    std::wstring lockedRelativePath;
    std::vector<ULONG_PTR> handleValues;
    HWND mainWindow = nullptr;
    bool isSystemCritical = false;
    bool isSelfExplorer = false;
};

namespace LockFinder {

std::vector<LockedProcessInfo> FindLocks(const std::wstring& targetPath, bool ignoreCurrentProcess = false);

std::wstring FormatBlockerSummary(const std::vector<LockedProcessInfo>& locks);

bool ShouldUseHandleOnlyUnlock(const std::vector<LockedProcessInfo>& locks);

bool UnlockTarget(
    const std::wstring& targetPath,
    const std::vector<LockedProcessInfo>& locks,
    UnlockMode mode,
    uint32_t* outClosedHandles = nullptr,
    uint32_t* outTerminatedProcs = nullptr,
    bool allowElevationFallback = true);

void Shutdown();

} // namespace LockFinder

} // namespace HandsOff
