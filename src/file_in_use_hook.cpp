#include "file_in_use_hook.h"
#include "common.h"
#include "dialog_enhancer.h"
#include "lock_finder.h"

namespace HandsOff::FileInUseHook {

namespace {

using PFN_GetInfoForFileInUse = HRESULT(WINAPI*)(IShellItem* psi, IFileIsInUse** ppof);
using PFN_MoveFileExW = BOOL(WINAPI*)(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);

class HandsOffFileInUse final : public IFileIsInUse {
public:
    HandsOffFileInUse(std::wstring targetPath, std::vector<LockedProcessInfo>&& locks)
        : targetPath_(std::move(targetPath)),
          locks_(std::move(locks)),
          formattedSummary_(LockFinder::FormatBlockerSummary(locks_)) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IFileIsInUse))) {
            *ppv = static_cast<IFileIsInUse*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG count = --refCount_;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP GetAppName(LPWSTR* ppszName) override {
        if (!ppszName) return E_POINTER;
        size_t byteLen = (formattedSummary_.size() + 1) * sizeof(wchar_t);
        auto buf = static_cast<LPWSTR>(CoTaskMemAlloc(byteLen));
        if (!buf) return E_OUTOFMEMORY;
        memcpy(buf, formattedSummary_.c_str(), byteLen);
        *ppszName = buf;
        return S_OK;
    }

    IFACEMETHODIMP GetUsage(FILE_USAGE_TYPE* pfut) override {
        if (!pfut) return E_POINTER;
        *pfut = FUT_GENERIC;
        return S_OK;
    }

    IFACEMETHODIMP GetCapabilities(DWORD* pdwCapFlags) override {
        if (!pdwCapFlags) return E_POINTER;
        // Omit OF_CAP_CANCLOSE so windows.storage.dll doesn't kill the app before showing the dialog.
        bool canSwitch = std::any_of(locks_.begin(), locks_.end(), [](const auto& l) {
            return l.mainWindow != nullptr;
        });
        *pdwCapFlags = canSwitch ? OF_CAP_CANSWITCHTO : 0;
        return S_OK;
    }

    IFACEMETHODIMP GetSwitchToHWND(HWND* phwnd) override {
        if (!phwnd) return E_POINTER;
        *phwnd = nullptr;
        for (const auto& l : locks_) {
            if (l.mainWindow) {
                *phwnd = l.mainWindow;
                return S_OK;
            }
        }
        return E_FAIL;
    }

    IFACEMETHODIMP CloseFile() override {
        return LockFinder::UnlockTarget(targetPath_, {}, UnlockMode::KillProcessAndCloseHandles)
            ? S_OK
            : E_FAIL;
    }

private:
    ~HandsOffFileInUse() = default;

    std::atomic<ULONG> refCount_{ 1 };
    std::wstring targetPath_;
    std::vector<LockedProcessInfo> locks_;
    std::wstring formattedSummary_;
};

struct PatchedIatSlot {
    void** slotAddr;
    void* originalValue;
};

struct MoveDestRedirect {
    std::wstring srcKey;
    std::wstring dstPath;
    DWORD tick = 0;
};

constexpr DWORD kMoveRedirectTtlMs = 15000;

std::mutex g_hookMutex;
std::mutex g_redirectMutex;
std::vector<MoveDestRedirect> g_moveDestRedirects;
std::atomic<bool> g_installed{ false };
std::atomic<uint32_t> g_activeHookCalls{ 0 };
PFN_GetInfoForFileInUse g_origGetInfo = nullptr;
PFN_GetInfoForFileInUse g_trampolineGetInfo = nullptr;
std::atomic<PFN_MoveFileExW> g_origMoveFileExW{ nullptr };
BYTE* g_targetExportAddr = nullptr;
BYTE g_savedOriginalBytes[32] = {};
size_t g_patchedSize = 0;
std::vector<PatchedIatSlot> g_patchedIatSlots;

struct ScopedHookCall {
    ScopedHookCall() { g_activeHookCalls.fetch_add(1, std::memory_order_acq_rel); }
    ~ScopedHookCall() { g_activeHookCalls.fetch_sub(1, std::memory_order_acq_rel); }
};

std::wstring CanonicalizeLongPath(const std::wstring& path) {
    std::wstring clean = TrimTrailingSlashes(StripExtendedPathPrefix(path));
    if (clean.empty()) return clean;

    wchar_t longBuf[1024] = {};
    DWORD longLen = GetLongPathNameW(clean.c_str(), longBuf, 1020);
    if (longLen > 0 && longLen < 1020) {
        return TrimTrailingSlashes(std::wstring(longBuf, longLen));
    }
    return clean;
}

std::wstring NormalizeRedirectKey(const std::wstring& path) {
    std::wstring norm = CanonicalizeLongPath(path);
    if (!norm.empty()) {
        CharLowerBuffW(norm.data(), static_cast<DWORD>(norm.size()));
    }
    return norm;
}

void RememberMoveDestRedirect(const std::wstring& srcPath, const std::wstring& dstPath) {
    std::wstring key = NormalizeRedirectKey(srcPath);
    std::wstring cleanDst = CanonicalizeLongPath(dstPath);
    if (key.empty() || cleanDst.empty()) return;

    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_redirectMutex);
    g_moveDestRedirects.erase(
        std::remove_if(g_moveDestRedirects.begin(), g_moveDestRedirects.end(), [&](const MoveDestRedirect& r) {
            return static_cast<DWORD>(now - r.tick) > kMoveRedirectTtlMs || r.srcKey == key;
        }),
        g_moveDestRedirects.end());
    if (g_moveDestRedirects.size() >= 32) {
        g_moveDestRedirects.erase(g_moveDestRedirects.begin());
    }
    g_moveDestRedirects.push_back({ std::move(key), std::move(cleanDst), now });
}

bool TryResolveMoveDestRedirect(const std::wstring& srcPath, std::wstring& outDstPath) {
    std::wstring key = NormalizeRedirectKey(srcPath);
    if (key.empty()) return false;

    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_redirectMutex);
    g_moveDestRedirects.erase(
        std::remove_if(g_moveDestRedirects.begin(), g_moveDestRedirects.end(), [&](const MoveDestRedirect& r) {
            return static_cast<DWORD>(now - r.tick) > kMoveRedirectTtlMs;
        }),
        g_moveDestRedirects.end());

    for (auto it = g_moveDestRedirects.rbegin(); it != g_moveDestRedirects.rend(); ++it) {
        if (it->srcKey == key) {
            outDstPath = it->dstPath;
            return true;
        }
    }
    return false;
}

void WriteAbsoluteJump64(BYTE* dst, const void* target) {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    *reinterpret_cast<uint32_t*>(dst + 2) = 0;
    *reinterpret_cast<uint64_t*>(dst + 6) = reinterpret_cast<uint64_t>(target);
}

void PatchModuleIat(
    HMODULE hMod,
    const char* importName,
    void* hookFunc,
    void* targetOriginal,
    void** outFirstOriginal = nullptr)
{
    if (!hMod || !importName || !hookFunc) return;
    auto* base = reinterpret_cast<BYTE*>(hMod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto patchSlot = [&](void** slot) {
        if (*slot == hookFunc) return;
        if (outFirstOriginal && !*outFirstOriginal) {
            *outFirstOriginal = *slot;
        }
        DWORD oldProt = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            g_patchedIatSlots.push_back({ slot, *slot });
            InterlockedExchangePointer(slot, hookFunc);
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
        }
    };

    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress && impDir.Size) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
        for (; desc->Name; ++desc) {
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            auto* orig = desc->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                : thunk;
            for (; orig->u1.AddressOfData; ++orig, ++thunk) {
                if (!IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) {
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), importName) == 0) {
                        patchSlot(reinterpret_cast<void**>(&thunk->u1.Function));
                    }
                }
            }
        }
    }

    const auto& delayDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress && delayDir.Size) {
        auto* desc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayDir.VirtualAddress);
        for (; desc->DllNameRVA; ++desc) {
            if (!desc->ImportNameTableRVA || !desc->ImportAddressTableRVA) continue;
            auto* intThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportNameTableRVA);
            auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportAddressTableRVA);
            for (; intThunk->u1.AddressOfData; ++intThunk, ++iatThunk) {
                if (!IMAGE_SNAP_BY_ORDINAL(intThunk->u1.Ordinal)) {
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + intThunk->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), importName) == 0) {
                        patchSlot(reinterpret_cast<void**>(&iatThunk->u1.Function));
                    }
                } else if (targetOriginal && reinterpret_cast<void*>(iatThunk->u1.Function) == targetOriginal) {
                    patchSlot(reinterpret_cast<void**>(&iatThunk->u1.Function));
                }
            }
        }
    }
}

size_t DecodeModRmLen(const uint8_t* p, size_t rem) {
    if (rem < 1) return 0;
    uint8_t modrm = p[0];
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;
    if (mod == 0 && rm == 5) {
        // [rip + disp32] is position-dependent and unsafe to copy verbatim to a trampoline.
        return 0;
    }

    size_t len = 1;
    if (mod != 3 && rm == 4) {
        if (rem < len + 1) return 0;
        uint8_t sib = p[len++];
        if (mod == 0 && (sib & 7) == 5) {
            len += 4;
        }
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;

    return (len <= rem) ? len : 0;
}

size_t DecodeSingleSafeX64Insn(const uint8_t* code, size_t rem) {
    if (!code || rem == 0) return 0;

    // CET endbr64 / endbr32 (F3 0F 1E FA/FB)
    if (rem >= 4 && code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E &&
        (code[3] == 0xFA || code[3] == 0xFB))
    {
        return 4;
    }

    size_t pos = 0;
    bool has66 = false;
    while (pos < rem && pos < 4) {
        uint8_t b = code[pos];
        if (b == 0x66) {
            has66 = true;
            ++pos;
        } else if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                   b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
                   b == 0x64 || b == 0x65)
        {
            ++pos;
        } else {
            break;
        }
    }
    if (pos >= rem) return 0;

    bool rexW = false;
    if (code[pos] >= 0x40 && code[pos] <= 0x4F) {
        rexW = (code[pos] & 0x08) != 0;
        ++pos;
    }
    if (pos >= rem) return 0;

    uint8_t op = code[pos++];
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90) {
        return pos;
    }
    if (op == 0x6A) {
        return (pos + 1 <= rem) ? (pos + 1) : 0;
    }
    if (op == 0x68 && !has66) {
        return (pos + 4 <= rem) ? (pos + 4) : 0;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        size_t imm = rexW ? 8 : (has66 ? 2 : 4);
        return (pos + imm <= rem) ? (pos + imm) : 0;
    }

    if (op == 0x0F) {
        if (pos >= rem) return 0;
        uint8_t op2 = code[pos++];
        if (op2 == 0x1F) {
            size_t m = DecodeModRmLen(code + pos, rem - pos);
            return m ? (pos + m) : 0;
        }
        return 0;
    }

    auto isSimpleModRmOp = [](uint8_t o) {
        switch (o) {
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x84: case 0x85:
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
            return true;
        default:
            return false;
        }
    };

    if (isSimpleModRmOp(op)) {
        size_t m = DecodeModRmLen(code + pos, rem - pos);
        return m ? (pos + m) : 0;
    }

    if (op == 0x83 || op == 0x81 || op == 0xC6 || op == 0xC7) {
        if (pos >= rem) return 0;
        if ((op == 0xC6 || op == 0xC7) && ((code[pos] >> 3) & 7) != 0) {
            return 0;
        }
        size_t m = DecodeModRmLen(code + pos, rem - pos);
        if (!m) return 0;
        size_t imm = (op == 0x83 || op == 0xC6) ? 1 : (has66 ? 2 : 4);
        return (pos + m + imm <= rem) ? (pos + m + imm) : 0;
    }

    return 0;
}

size_t CalculateSafePrologueSize(const BYTE* code) {
    return CalculateSafePrologueSizeForBuffer(code, sizeof(g_savedOriginalBytes));
}

BOOL WINAPI Hook_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    ScopedHookCall activeGuard;
    PFN_MoveFileExW orig = g_origMoveFileExW.load(std::memory_order_acquire);
    if (!orig) {
        if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
            orig = reinterpret_cast<PFN_MoveFileExW>(GetProcAddress(hKernel32, "MoveFileExW"));
        }
    }
    if (!orig) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }

    if (orig(lpExistingFileName, lpNewFileName, dwFlags)) {
        return TRUE;
    }

    DWORD err = GetLastError();
    // Moving over an open file on the same NTFS volume returns ERROR_ACCESS_DENIED (5)
    // instead of ERROR_SHARING_VIOLATION (32), bypassing Explorer's File-In-Use prompt.
    if (err == ERROR_ACCESS_DENIED &&
        (dwFlags & MOVEFILE_REPLACE_EXISTING) != 0 &&
        lpNewFileName != nullptr &&
        lpNewFileName[0] != L'\0')
    {
        DWORD dstAttrs = GetFileAttributesW(lpNewFileName);
        if (dstAttrs != INVALID_FILE_ATTRIBUTES &&
            (dstAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            !LockFinder::FindLocks(lpNewFileName, false).empty())
        {
            if (lpExistingFileName && lpExistingFileName[0] != L'\0') {
                RememberMoveDestRedirect(lpExistingFileName, lpNewFileName);
            }
            err = ERROR_SHARING_VIOLATION;
        }
    }

    SetLastError(err);
    return FALSE;
}

HRESULT WINAPI Hook_GetInfoForFileInUse(IShellItem* psi, IFileIsInUse** ppof) {
    ScopedHookCall activeGuard;
    if (!ppof) return E_POINTER;
    *ppof = nullptr;
    if (!psi) return E_INVALIDARG;

    LPWSTR pwszFilePath = nullptr;
    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pwszFilePath)) && pwszFilePath) {
        std::wstring targetPath(pwszFilePath);
        CoTaskMemFree(pwszFilePath);

        auto locks = LockFinder::FindLocks(targetPath, false);
        // For Move-with-replace conflicts, shell32 queries GetInfoForFileInUse with the
        // source IShellItem even though the destination file is the one held by another process.
        std::wstring redirectedDst;
        if (locks.empty() && TryResolveMoveDestRedirect(targetPath, redirectedDst)) {
            locks = LockFinder::FindLocks(redirectedDst, false);
            if (!locks.empty()) {
                targetPath = std::move(redirectedDst);
            }
        }
        if (!locks.empty()) {
            auto locksCopy = locks;
            if (auto* customObj = new (std::nothrow) HandsOffFileInUse(targetPath, std::move(locks))) {
                DialogEnhancer::OnConflictDetected(targetPath, locksCopy);
                *ppof = customObj;
                return S_OK;
            }
        }
    }

    PFN_GetInfoForFileInUse orig = g_trampolineGetInfo ? g_trampolineGetInfo : g_origGetInfo;
    return orig ? orig(psi, ppof) : E_FAIL;
}

} // namespace

bool Install() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_installed.load()) return true;

    HMODULE hWinStorage = LoadLibraryW(L"windows.storage.dll");
    if (!hWinStorage) return false;

    g_targetExportAddr = reinterpret_cast<BYTE*>(GetProcAddress(hWinStorage, "GetInfoForFileInUse"));
    if (!g_targetExportAddr) return false;

    g_origGetInfo = reinterpret_cast<PFN_GetInfoForFileInUse>(g_targetExportAddr);

    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (hShell32) {
        PatchModuleIat(
            hShell32,
            "GetInfoForFileInUse",
            reinterpret_cast<void*>(&Hook_GetInfoForFileInUse),
            g_targetExportAddr);
    }

    void* origMove = nullptr;
    PatchModuleIat(hWinStorage, "MoveFileExW", reinterpret_cast<void*>(&Hook_MoveFileExW), nullptr, &origMove);
    if (hShell32) {
        PatchModuleIat(hShell32, "MoveFileExW", reinterpret_cast<void*>(&Hook_MoveFileExW), nullptr, &origMove);
    }
    if (!origMove) {
        if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
            origMove = reinterpret_cast<void*>(GetProcAddress(hKernel32, "MoveFileExW"));
        }
    }
    if (origMove) {
        g_origMoveFileExW.store(reinterpret_cast<PFN_MoveFileExW>(origMove), std::memory_order_release);
    }

    size_t prologueLen = CalculateSafePrologueSize(g_targetExportAddr);
    if (prologueLen >= 14 && prologueLen <= sizeof(g_savedOriginalBytes)) {
        if (auto* tramp = static_cast<BYTE*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
            memcpy(g_savedOriginalBytes, g_targetExportAddr, prologueLen);
            memcpy(tramp, g_targetExportAddr, prologueLen);
            WriteAbsoluteJump64(tramp + prologueLen, g_targetExportAddr + prologueLen);
            g_trampolineGetInfo = reinterpret_cast<PFN_GetInfoForFileInUse>(tramp);

            DWORD oldProt = 0;
            if (VirtualProtect(g_targetExportAddr, prologueLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
                WriteAbsoluteJump64(g_targetExportAddr, reinterpret_cast<const void*>(&Hook_GetInfoForFileInUse));
                g_patchedSize = prologueLen;
                VirtualProtect(g_targetExportAddr, prologueLen, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), g_targetExportAddr, prologueLen);
            } else {
                VirtualFree(tramp, 0, MEM_RELEASE);
                g_trampolineGetInfo = nullptr;
            }
        }
    }

    g_installed.store(true);
    return true;
}

void Uninstall() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (!g_installed.load()) return;

    if (g_targetExportAddr && g_patchedSize >= 14) {
        DWORD oldProt = 0;
        if (VirtualProtect(g_targetExportAddr, g_patchedSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(g_targetExportAddr, g_savedOriginalBytes, g_patchedSize);
            VirtualProtect(g_targetExportAddr, g_patchedSize, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), g_targetExportAddr, g_patchedSize);
        }
        g_patchedSize = 0;
    }

    for (const auto& slot : g_patchedIatSlots) {
        DWORD oldProt = 0;
        if (VirtualProtect(slot.slotAddr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            InterlockedExchangePointer(slot.slotAddr, slot.originalValue);
            VirtualProtect(slot.slotAddr, sizeof(void*), oldProt, &oldProt);
        }
    }
    g_patchedIatSlots.clear();

    // Wait for any threads currently executing inside the hook or trampoline before freeing the trampoline.
    for (int i = 0; i < 200 && g_activeHookCalls.load(std::memory_order_acquire) > 0; ++i) {
        Sleep(5);
    }
    if (g_trampolineGetInfo && g_activeHookCalls.load(std::memory_order_acquire) == 0) {
        VirtualFree(reinterpret_cast<void*>(g_trampolineGetInfo), 0, MEM_RELEASE);
        g_trampolineGetInfo = nullptr;
    }
    {
        std::lock_guard<std::mutex> rlock(g_redirectMutex);
        g_moveDestRedirects.clear();
    }
    g_installed.store(false);
}

bool IsInstalled() {
    return g_installed.load();
}

HookDiagnostics GetDiagnostics() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    HookDiagnostics diag;
    diag.installed = g_installed.load();
    diag.patchedIatCount = static_cast<uint32_t>(g_patchedIatSlots.size());
    diag.iatPatched = !g_patchedIatSlots.empty();
    diag.inlineHooked = (g_patchedSize >= 14 && g_trampolineGetInfo != nullptr);
    diag.prologueLen = g_patchedSize;
    return diag;
}

size_t CalculateSafePrologueSizeForBuffer(const uint8_t* code, size_t maxBytes) {
    if (!code || maxBytes < 14) return 0;
    size_t cap = std::min<size_t>(maxBytes, sizeof(g_savedOriginalBytes));
    size_t total = 0;
    while (total < 14) {
        size_t insnLen = DecodeSingleSafeX64Insn(code + total, cap - total);
        if (insnLen == 0) return 0;
        total += insnLen;
    }
    return (total <= cap) ? total : 0;
}

} // namespace HandsOff::FileInUseHook
