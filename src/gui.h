#pragma once

#include "common.h"

namespace HandsOff {

struct PatchStatusInfo {
    bool isComRegistered = false;
    bool isLoadedInExplorer = false;
    DWORD explorerPid = 0;
};

struct PatchOperationResult {
    bool success = false;
    std::wstring statusMessage;
};

PatchStatusInfo QueryPatchStatus();
PatchOperationResult ApplyPatch();
PatchOperationResult RollbackPatch();
int RunGuiWindow(HINSTANCE hInstance);

} // namespace HandsOff
