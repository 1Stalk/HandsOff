#pragma once

#include <cstddef>
#include <cstdint>

namespace HandsOff::FileInUseHook {

struct HookDiagnostics {
    bool installed = false;
    bool iatPatched = false;
    bool inlineHooked = false;
    size_t prologueLen = 0;
    uint32_t patchedIatCount = 0;
};

bool Install();
void Uninstall();
bool IsInstalled();
HookDiagnostics GetDiagnostics();
size_t CalculateSafePrologueSizeForBuffer(const uint8_t* code, size_t maxBytes);

} // namespace HandsOff::FileInUseHook
