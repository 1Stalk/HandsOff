#pragma once

#include "common.h"
#include "lock_finder.h"

namespace HandsOff::DialogEnhancer {

void Initialize(HMODULE hModule);
void Shutdown();
bool IsRunning();
void OnConflictDetected(const std::wstring& targetPath, const std::vector<LockedProcessInfo>& locks);

} // namespace HandsOff::DialogEnhancer
