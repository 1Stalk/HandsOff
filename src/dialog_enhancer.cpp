#include "dialog_enhancer.h"
#include <dwmapi.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

namespace HandsOff::DialogEnhancer {

namespace {

constexpr const wchar_t* kSubclassProp = L"HandsOffOpStatusCtxPtr";
constexpr UINT_PTR kTimerIdCheckInterrupt = 0x4801;
constexpr UINT_PTR kTimerIdAutoRetry      = 0x4802;
constexpr const wchar_t* kSharedConflictMemName   = L"Local\\HandsOffActiveConflictMem";
constexpr const wchar_t* kSharedConflictMutexName = L"Local\\HandsOffActiveConflictMutex";
constexpr uint32_t kSharedConflictMagic           = 0x484F4353; // 'HOCS'
constexpr uint32_t kSharedConflictVersion         = 1;

#pragma pack(push, 1)
struct SharedConflictState {
    uint32_t magic;
    uint32_t version;
    uint64_t nextSeq;
    uint64_t seq;
    DWORD    detectedTick;
    uint32_t handleOnlyPrimary;
    uint32_t firstLockPid;
    uint64_t claimedByHwnd;
    wchar_t  targetPath[1024];
    wchar_t  blockerSummary[512];
    wchar_t  firstLockProcessName[128];
};
#pragma pack(pop)

std::mutex g_conflictMutex;
uint64_t g_nextConflictSeq = 0;
ConflictSnapshot g_activeConflict;

struct SharedConflictMapping {
    HANDLE hMutex = nullptr;
    HANDLE hMap = nullptr;
    SharedConflictState* view = nullptr;

    ~SharedConflictMapping() {
        if (view) UnmapViewOfFile(view);
        if (hMap) CloseHandle(hMap);
        if (hMutex) CloseHandle(hMutex);
    }

    bool EnsureOpen() {
        if (!hMutex) {
            hMutex = CreateMutexW(nullptr, FALSE, kSharedConflictMutexName);
        }
        if (!hMap) {
            hMap = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                sizeof(SharedConflictState), kSharedConflictMemName);
        }
        if (hMap && !view) {
            view = static_cast<SharedConflictState*>(
                MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedConflictState)));
        }
        if (view && (view->magic != kSharedConflictMagic || view->version != kSharedConflictVersion)) {
            ZeroMemory(view, sizeof(SharedConflictState));
            view->magic = kSharedConflictMagic;
            view->version = kSharedConflictVersion;
        }
        return view != nullptr;
    }
};

SharedConflictMapping g_sharedConflict;

struct ScopedNamedMutex {
    HANDLE hMutex = nullptr;
    bool locked = false;
    explicit ScopedNamedMutex(HANDLE h) : hMutex(h) {
        if (hMutex) {
            DWORD r = WaitForSingleObject(hMutex, 50);
            locked = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
        }
    }
    ~ScopedNamedMutex() {
        if (locked && hMutex) ReleaseMutex(hMutex);
    }
};

ConflictSnapshot SnapshotFromShared(const SharedConflictState& s) {
    ConflictSnapshot snap;
    snap.seq = s.seq;
    snap.targetPath = s.targetPath;
    snap.blockerSummary = s.blockerSummary;
    snap.detectedTick = s.detectedTick;
    snap.claimedByHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(s.claimedByHwnd));
    snap.handleOnlyPrimary = (s.handleOnlyPrimary != 0);
    if (s.firstLockPid != 0) {
        LockedProcessInfo info;
        info.pid = s.firstLockPid;
        info.processName = s.firstLockProcessName;
        snap.locks.push_back(std::move(info));
    }
    return snap;
}

void WakeAllExplorerTriggerThreads() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                std::wstring evName = L"Local\\HandsOffWake_" + std::to_wstring(pe.th32ProcessID);
                if (HANDLE hEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, evName.c_str())) {
                    SetEvent(hEv);
                    CloseHandle(hEv);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

std::mutex g_lifecycleMutex;
std::mutex g_wakeEventMutex;
HMODULE g_hModule = nullptr;
std::atomic<bool> g_running{ false };
HANDLE g_hTriggerThread = nullptr;
HANDLE g_hWakeEvent = nullptr;
UINT g_msgAttachOnUiThread = 0;
UINT g_msgDetachOnUiThread = 0;

std::mutex g_attachedWindowsMutex;
std::vector<HWND> g_attachedWindows;

void RegisterAttachedWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_attachedWindowsMutex);
    if (std::find(g_attachedWindows.begin(), g_attachedWindows.end(), hwnd) == g_attachedWindows.end()) {
        g_attachedWindows.push_back(hwnd);
    }
}

void UnregisterAttachedWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_attachedWindowsMutex);
    g_attachedWindows.erase(
        std::remove(g_attachedWindows.begin(), g_attachedWindows.end(), hwnd),
        g_attachedWindows.end());
}

bool IsHighContrastActive() {
    HIGHCONTRASTW hc = { sizeof(hc) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

double GetColorLuminance(COLORREF c) {
    return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c);
}

HBITMAP CreateTopDownDib(HDC hdcRef, int w, int h, void** outBits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(hdcRef, &bmi, DIB_RGB_COLORS, outBits, nullptr, 0);
}

bool DetectDialogBackgroundColor(HWND hwndDui, COLORREF& outBgColor, bool& outIsDark) {
    if (IsHighContrastActive()) {
        outBgColor = GetSysColor(COLOR_WINDOW);
        outIsDark = GetColorLuminance(outBgColor) < 128.0;
        return true;
    }

    RECT rcDui = {};
    if (IsWindow(hwndDui) && IsWindowVisible(hwndDui) && GetClientRect(hwndDui, &rcDui)) {
        int duiW = rcDui.right - rcDui.left;
        int duiH = rcDui.bottom - rcDui.top;
        if (duiW > 24 && duiH > 24) {
            constexpr int kSize = 16;
            HDC hdcScreen = GetDC(hwndDui);
            if (hdcScreen) {
                HDC hdcMem = CreateCompatibleDC(hdcScreen);
                void* bits = nullptr;
                HBITMAP hBmp = CreateTopDownDib(hdcScreen, kSize, kSize, &bits);
                bool ok = false;
                if (hdcMem && hBmp && bits) {
                    HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);
                    auto* px = static_cast<uint32_t*>(bits);
                    std::fill_n(px, kSize * kSize, 0x00FE01FEu);

                    POINT oldOrg = {};
                    SetViewportOrgEx(hdcMem, -8, -(duiH - 18), &oldOrg);
                    SendMessageW(hwndDui, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(hdcMem),
                                 PRF_CLIENT | PRF_ERASEBKGND);
                    SetViewportOrgEx(hdcMem, oldOrg.x, oldOrg.y, nullptr);
                    GdiFlush();

                    uint32_t sampled = px[4 * kSize + 4] & 0x00FFFFFF;
                    SelectObject(hdcMem, oldBmp);

                    if (sampled != 0x00FE01FE) {
                        outBgColor = RGB((sampled >> 16) & 0xFF, (sampled >> 8) & 0xFF, sampled & 0xFF);
                        outIsDark = GetColorLuminance(outBgColor) < 128.0;
                        ok = true;
                    }
                }
                if (hBmp) DeleteObject(hBmp);
                if (hdcMem) DeleteDC(hdcMem);
                ReleaseDC(hwndDui, hdcScreen);
                if (ok) return true;
            }
        }
    }

    outIsDark = false;
    outBgColor = GetSysColor(COLOR_WINDOW);
    return false;
}

bool ThemeDrawsDarkPushButton(HTHEME hTheme) {
    if (!hTheme) return false;
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    void* bits = nullptr;
    HBITMAP hBmp = CreateTopDownDib(hdcScreen, 24, 24, &bits);
    bool isDark = false;
    if (hdcMem && hBmp && bits) {
        HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);
        RECT rc = { 0, 0, 24, 24 };
        HBRUSH hBg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdcMem, &rc, hBg);
        DeleteObject(hBg);

        if (SUCCEEDED(DrawThemeBackground(hTheme, hdcMem, BP_PUSHBUTTON, PBS_NORMAL, &rc, nullptr))) {
            GdiFlush();
            uint32_t c = static_cast<uint32_t*>(bits)[12 * 24 + 12];
            isDark = GetColorLuminance(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)) < 128.0;
        }
        SelectObject(hdcMem, oldBmp);
    }
    if (hBmp) DeleteObject(hBmp);
    if (hdcMem) DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return isDark;
}

std::wstring StripAcceleratorAmpersands(const wchar_t* text) {
    if (!text) return {};
    std::wstring out;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p != L'&') out.push_back(*p);
    }
    return out;
}

std::wstring LoadShell32RawString(UINT resourceId) {
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (!hShell32) return {};
    wchar_t buf[512] = {};
    int len = LoadStringW(hShell32, resourceId, buf, 511);
    return (len > 0) ? std::wstring(buf, static_cast<size_t>(len)) : L"";
}

std::wstring LoadShell32CleanString(UINT resourceId) {
    std::wstring raw = LoadShell32RawString(resourceId);
    return raw.empty() ? L"" : StripAcceleratorAmpersands(raw.c_str());
}

std::wstring NormalizeAndLowerDialogText(const std::wstring& text) {
    std::wstring norm = NormalizeDialogText(text);
    if (!norm.empty()) {
        CharLowerBuffW(norm.data(), static_cast<DWORD>(norm.size()));
    }
    return norm;
}

FileInUseTemplate ParseFileInUseTemplate(const std::wstring& raw) {
    FileInUseTemplate tmpl;
    if (raw.empty()) return tmpl;

    size_t pos = raw.find(L"%1");
    if (pos == std::wstring::npos) {
        tmpl.prefix = NormalizeAndLowerDialogText(raw);
        return tmpl;
    }

    tmpl.prefix = NormalizeAndLowerDialogText(raw.substr(0, pos));
    size_t after = pos + 2;
    if (after < raw.size() && raw[after] == L'!') {
        size_t endBang = raw.find(L'!', after + 1);
        if (endBang != std::wstring::npos) {
            after = endBang + 1;
        }
    }
    tmpl.suffix = NormalizeAndLowerDialogText(raw.substr(after));
    return tmpl;
}

bool MatchesTemplate(const std::wstring& normLowerText, const FileInUseTemplate& tmpl) {
    if (tmpl.prefix.empty() && tmpl.suffix.empty()) return false;
    if (!tmpl.prefix.empty() && !tmpl.suffix.empty()) {
        size_t p1 = normLowerText.find(tmpl.prefix);
        if (p1 == std::wstring::npos) return false;
        size_t p2 = normLowerText.find(tmpl.suffix, p1 + tmpl.prefix.size());
        return p2 != std::wstring::npos;
    }
    const std::wstring& part = !tmpl.prefix.empty() ? tmpl.prefix : tmpl.suffix;
    return part.size() >= 8 && normLowerText.find(part) != std::wstring::npos;
}

bool MatchesConflictSummary(const std::vector<std::wstring>& normLowerTexts, const ConflictSnapshot& conflict) {
    std::wstring normSummary = NormalizeAndLowerDialogText(conflict.blockerSummary);
    if (normSummary.empty()) return false;

    std::wstring pidToken;
    std::wstring procToken;
    if (!conflict.locks.empty()) {
        pidToken = NormalizeAndLowerDialogText(L"PID " + std::to_wstring(conflict.locks[0].pid));
        procToken = NormalizeAndLowerDialogText(conflict.locks[0].processName);
    }

    for (const auto& t : normLowerTexts) {
        if (t.find(normSummary) != std::wstring::npos) return true;
        if (normSummary.size() > 64 && t.find(normSummary.substr(0, 64)) != std::wstring::npos) return true;
        if (!pidToken.empty() && !procToken.empty() &&
            t.find(pidToken) != std::wstring::npos &&
            t.find(procToken) != std::wstring::npos)
        {
            return true;
        }
    }
    return false;
}

std::wstring ReadWindowTitleText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, title.data(), len + 1);
    if (copied <= 0) return {};
    title.resize(static_cast<size_t>(copied));
    return title;
}

bool IsAccessibleElementHidden(IAccessible* pAcc, const VARIANT& varChild) {
    VARIANT varState;
    VariantInit(&varState);
    bool hidden = SUCCEEDED(pAcc->get_accState(varChild, &varState)) &&
                  varState.vt == VT_I4 &&
                  (varState.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0;
    VariantClear(&varState);
    return hidden;
}

template <typename Callback>
bool VisitAccessibleElements(IAccessible* pAcc, Callback&& onElement, int depth = 0) {
    if (!pAcc || depth > 8) return false;

    VARIANT varSelf;
    VariantInit(&varSelf);
    varSelf.vt = VT_I4;
    varSelf.lVal = CHILDID_SELF;

    if (depth > 0 && IsAccessibleElementHidden(pAcc, varSelf)) return false;

    auto checkItem = [&](IAccessible* accObj, const VARIANT& varChild) -> bool {
        if (IsAccessibleElementHidden(accObj, varChild)) return false;

        VARIANT varRole;
        VariantInit(&varRole);
        long role = (SUCCEEDED(accObj->get_accRole(varChild, &varRole)) && varRole.vt == VT_I4) ? varRole.lVal : 0;
        VariantClear(&varRole);

        bool isButton = (role == ROLE_SYSTEM_PUSHBUTTON || role == ROLE_SYSTEM_CHECKBUTTON);
        bool isText = (role == ROLE_SYSTEM_STATICTEXT || role == ROLE_SYSTEM_TEXT || role == ROLE_SYSTEM_LINK);
        if (!isButton && !isText) return false;

        std::wstring name;
        BSTR bstrName = nullptr;
        if (SUCCEEDED(accObj->get_accName(varChild, &bstrName)) && bstrName) {
            name = StripAcceleratorAmpersands(bstrName);
            SysFreeString(bstrName);
        }
        if (name.empty() && isText) {
            BSTR bstrVal = nullptr;
            if (SUCCEEDED(accObj->get_accValue(varChild, &bstrVal)) && bstrVal) {
                name = StripAcceleratorAmpersands(bstrVal);
                SysFreeString(bstrVal);
            }
        }
        return onElement(accObj, varChild, role, name);
    };

    if (checkItem(pAcc, varSelf)) return true;

    long childCount = 0;
    if (FAILED(pAcc->get_accChildCount(&childCount)) || childCount <= 0 || childCount > 64) {
        return false;
    }

    std::vector<VARIANT> children(static_cast<size_t>(childCount));
    for (auto& v : children) VariantInit(&v);

    long obtained = 0;
    bool stopped = false;
    if (SUCCEEDED(AccessibleChildren(pAcc, 0, childCount, children.data(), &obtained))) {
        for (long i = 0; i < obtained; ++i) {
            if (!stopped) {
                if (children[i].vt == VT_DISPATCH && children[i].pdispVal) {
                    IAccessible* pChildAcc = nullptr;
                    if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_PPV_ARGS(&pChildAcc))) && pChildAcc) {
                        stopped = VisitAccessibleElements(pChildAcc, onElement, depth + 1);
                        pChildAcc->Release();
                    }
                } else if (children[i].vt == VT_I4) {
                    stopped = checkItem(pAcc, children[i]);
                }
            }
            VariantClear(&children[i]);
        }
    }
    return stopped;
}

struct AccessibleControlInfo {
    long role = 0;
    std::wstring name;
    RECT rcClient = {};
};

struct NativeButtonRowLayout {
    bool tryAgainVisible = false;
    RECT rcTryAgain = {};
    RECT rcLeftmostAction = {};
    int gapPx = 12;
    int cancelWidthPx = 0;
    int minAllowedLeftPx = 20;
    std::vector<std::wstring> visibleTexts;
};

NativeButtonRowLayout InspectNativeButtonRow(
    HWND hwndOp,
    HWND hwndDui,
    const std::wstring& tryAgainLabel,
    int defaultGapPx,
    int leftMarginPx)
{
    NativeButtonRowLayout layout;
    layout.gapPx = defaultGapPx;
    layout.minAllowedLeftPx = leftMarginPx;

    if (!IsWindow(hwndDui) || !IsWindowVisible(hwndDui) || tryAgainLabel.empty()) return layout;

    RECT rcDuiClient = {};
    GetWindowRect(hwndDui, &rcDuiClient);
    MapWindowPoints(HWND_DESKTOP, hwndOp, reinterpret_cast<LPPOINT>(&rcDuiClient), 2);
    layout.minAllowedLeftPx = rcDuiClient.left + leftMarginPx;

    IAccessible* pRootAcc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwndDui, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&pRootAcc))) || !pRootAcc) {
        return layout;
    }

    std::vector<AccessibleControlInfo> controls;
    VisitAccessibleElements(pRootAcc, [&](IAccessible* accObj, const VARIANT& varChild, long role, const std::wstring& name) {
        if (role == ROLE_SYSTEM_STATICTEXT || role == ROLE_SYSTEM_TEXT || role == ROLE_SYSTEM_LINK) {
            if (!name.empty()) {
                layout.visibleTexts.push_back(name);
            }
            return false;
        }

        long x = 0, y = 0, w = 0, h = 0;
        if (SUCCEEDED(accObj->accLocation(&x, &y, &w, &h, varChild)) && w > 0 && h > 0) {
            RECT rc = { x, y, x + w, y + h };
            MapWindowPoints(HWND_DESKTOP, hwndOp, reinterpret_cast<LPPOINT>(&rc), 2);
            controls.push_back({ role, name, rc });
        }
        return false;
    });
    pRootAcc->Release();

    for (const auto& c : controls) {
        if (c.role == ROLE_SYSTEM_PUSHBUTTON && _wcsicmp(c.name.c_str(), tryAgainLabel.c_str()) == 0) {
            layout.tryAgainVisible = true;
            layout.rcTryAgain = c.rcClient;
            layout.rcLeftmostAction = c.rcClient;
            break;
        }
    }
    if (!layout.tryAgainVisible) return layout;

    std::vector<RECT> rowButtons;
    for (const auto& c : controls) {
        if (c.role == ROLE_SYSTEM_PUSHBUTTON &&
            c.rcClient.top < layout.rcTryAgain.bottom &&
            c.rcClient.bottom > layout.rcTryAgain.top)
        {
            rowButtons.push_back(c.rcClient);
        }
    }

    if (!rowButtons.empty()) {
        std::sort(rowButtons.begin(), rowButtons.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });
        layout.rcLeftmostAction = rowButtons.front();
        layout.cancelWidthPx = rowButtons.back().right - rowButtons.back().left;
        if (rowButtons.size() >= 2) {
            int gap = rowButtons[1].left - rowButtons[0].right;
            if (gap >= 4 && gap <= defaultGapPx * 2) layout.gapPx = gap;
        }
    }

    for (const auto& c : controls) {
        if (c.role == ROLE_SYSTEM_CHECKBUTTON &&
            c.rcClient.top < layout.rcTryAgain.bottom &&
            c.rcClient.bottom > layout.rcTryAgain.top &&
            c.rcClient.right <= layout.rcLeftmostAction.left)
        {
            layout.minAllowedLeftPx = std::max<int>(layout.minAllowedLeftPx, c.rcClient.right + layout.gapPx);
        }
    }

    return layout;
}

bool ClickNativeTryAgainButton(HWND hwndOp, const std::wstring& tryAgainLabel) {
    if (!IsWindow(hwndOp)) return false;

    HWND hwndDui = FindWindowExW(hwndOp, nullptr, L"DirectUIHWND", nullptr);
    HWND hwndTarget = hwndDui ? hwndDui : hwndOp;

    IAccessible* pRootAcc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(hwndTarget, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&pRootAcc))) && pRootAcc) {
        bool invoked = VisitAccessibleElements(pRootAcc, [&](IAccessible* accObj, const VARIANT& varChild, long role, const std::wstring& name) {
            if (role == ROLE_SYSTEM_PUSHBUTTON && _wcsicmp(name.c_str(), tryAgainLabel.c_str()) == 0) {
                return SUCCEEDED(accObj->accDoDefaultAction(varChild));
            }
            return false;
        });
        pRootAcc->Release();
        if (invoked) return true;
    }

    SetFocus(hwndTarget);
    PostMessageW(hwndTarget, WM_KEYDOWN, VK_RETURN, 0x001C0001);
    PostMessageW(hwndTarget, WM_KEYUP, VK_RETURN, 0xC01C0001);
    return true;
}

struct OpStatusUiContext {
    HWND hwndOp = nullptr;
    WNDPROC origOpWndProc = nullptr;
    HWND hwndBar = nullptr;
    UINT dpi = 96;
    int fallbackBarHeight = 38;
    bool isVisible = false;
    bool useFallbackBar = false;
    bool fallbackExpanded = false;
    bool isHandleOnlyPrimary = false;

    std::wstring primaryText = L"Kill";
    FileInUseUiStrings uiStrings;

    RECT rcTargetInOp = {};
    RECT rcBtnPrimary = {};
    bool hovered = false;
    bool pressed = false;

    bool layoutAppliedOnce = false;
    bool lastVisible = false;
    bool lastUseFallbackBar = false;
    RECT lastTargetInOp = {};
    std::wstring lastPrimaryText;
    int lastClientW = 0;
    int lastClientH = 0;
    ConflictSnapshot boundConflict;

    bool isDarkDialog = false;
    bool useCustomDarkDraw = false;
    bool resourcesBuilt = false;
    bool inRebuildResources = false;
    bool inLayoutUpdate = false;
    int appliedThemeMode = -1;
    COLORREF dialogBgColor = RGB(255, 255, 255);
    bool lastIsDarkDialog = false;
    COLORREF lastDialogBgColor = RGB(255, 255, 255);

    HFONT hFontBtn = nullptr;
    HTHEME hThemeBtn = nullptr;

    int Scale(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }

    void FreeResources() {
        if (hFontBtn)  { DeleteObject(hFontBtn); hFontBtn = nullptr; }
        if (hThemeBtn) { CloseThemeData(hThemeBtn); hThemeBtn = nullptr; }
    }

    void RebuildResources(HWND hwndForTheme, bool darkDialog = false) {
        if (inRebuildResources) return;
        inRebuildResources = true;

        FreeResources();
        useCustomDarkDraw = false;

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        bool gotMetrics = false;
        using PFN_SysParamDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
        if (auto pFn = reinterpret_cast<PFN_SysParamDpi>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")))
        {
            gotMetrics = pFn(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi) != FALSE;
        }
        if (!gotMetrics) gotMetrics = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) != FALSE;

        if (gotMetrics) {
            ncm.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
            hFontBtn = CreateFontIndirectW(&ncm.lfMessageFont);
        }
        if (!hFontBtn) {
            LOGFONTW lf = {};
            lf.lfHeight = -Scale(12);
            lf.lfWeight = FW_NORMAL;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            hFontBtn = CreateFontIndirectW(&lf);
        }

        if (hwndForTheme) {
            int wantedTheme = (darkDialog && !IsHighContrastActive()) ? 1 : 0;
            if (wantedTheme != appliedThemeMode) {
                appliedThemeMode = wantedTheme;
                SetWindowTheme(hwndForTheme, wantedTheme ? L"DarkMode_Explorer" : nullptr, nullptr);
            }
            if (wantedTheme == 1) {
                hThemeBtn = OpenThemeData(hwndForTheme, L"DarkMode_Explorer::Button");
                if (!hThemeBtn) hThemeBtn = OpenThemeData(hwndForTheme, L"Button");
                if (!ThemeDrawsDarkPushButton(hThemeBtn)) {
                    if (hThemeBtn) { CloseThemeData(hThemeBtn); hThemeBtn = nullptr; }
                    useCustomDarkDraw = true;
                }
            } else {
                hThemeBtn = OpenThemeData(hwndForTheme, L"Button");
            }
        }
        resourcesBuilt = true;
        inRebuildResources = false;
    }

    void EnsureFont() {
        if (!resourcesBuilt || !hFontBtn) RebuildResources(hwndBar, isDarkDialog);
    }

    void UpdateButtonRect(int w, int h) {
        if (useFallbackBar) {
            int padX = Scale(16), padY = Scale(6);
            int btnW = std::min(w - padX * 2, Scale(110));
            rcBtnPrimary = { w - padX - btnW, padY, w - padX, h - padY };
        } else {
            rcBtnPrimary = { 0, 0, w, h };
        }
    }

    ~OpStatusUiContext() { FreeResources(); }
};

void DrawNativeFallbackButton(HDC hdc, const RECT& rc, bool hovered, bool pressed, bool dark, int radius, COLORREF dialogBg) {
    COLORREF fill, border;
    if (dark) {
        auto clamp = [](int v) -> BYTE { return static_cast<BYTE>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        int d = pressed ? -4 : (hovered ? 18 : 2);
        fill = RGB(clamp(GetRValue(dialogBg) + d), clamp(GetGValue(dialogBg) + d), clamp(GetBValue(dialogBg) + d));
        border = pressed ? RGB(96, 96, 96) : (hovered ? RGB(148, 148, 148) : RGB(118, 118, 118));
    } else {
        fill = pressed ? RGB(204, 228, 247) : (hovered ? RGB(229, 241, 251) : RGB(253, 253, 253));
        border = (hovered || pressed) ? RGB(0, 120, 215) : RGB(208, 208, 208);
    }

    HBRUSH hBrush = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldB = SelectObject(hdc, hBrush);
    HGDIOBJ oldP = SelectObject(hdc, hPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldP);
    SelectObject(hdc, oldB);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

void PaintBarDoubleBuffered(HDC hdcTarget, const RECT& rcClient, OpStatusUiContext& ctx) {
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;
    if (w <= 0 || h <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcTarget, w, h);
    HGDIOBJ oldBmp = SelectObject(hdc, hBmp);

    HBRUSH hBg = CreateSolidBrush(ctx.dialogBgColor);
    RECT rcLocal = { 0, 0, w, h };
    FillRect(hdc, &rcLocal, hBg);
    DeleteObject(hBg);

    ctx.EnsureFont();
    ctx.UpdateButtonRect(w, h);

    int stateId = ctx.pressed ? PBS_PRESSED : (ctx.hovered ? PBS_HOT : PBS_NORMAL);
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(hdc, ctx.hFontBtn);

    if (ctx.isDarkDialog && ctx.useCustomDarkDraw) {
        DrawNativeFallbackButton(hdc, ctx.rcBtnPrimary, ctx.hovered, ctx.pressed, true, ctx.Scale(5), ctx.dialogBgColor);
        SetTextColor(hdc, ctx.pressed ? RGB(207, 207, 207) : RGB(255, 255, 255));
    } else if (ctx.hThemeBtn) {
        RECT rcTheme = ctx.rcBtnPrimary;
        DrawThemeBackground(ctx.hThemeBtn, hdc, BP_PUSHBUTTON, stateId, &rcTheme, nullptr);
        COLORREF textColor = ctx.isDarkDialog ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNTEXT);
        GetThemeColor(ctx.hThemeBtn, BP_PUSHBUTTON, stateId, TMT_TEXTCOLOR, &textColor);
        SetTextColor(hdc, textColor);
    } else {
        DrawNativeFallbackButton(hdc, ctx.rcBtnPrimary, ctx.hovered, ctx.pressed, ctx.isDarkDialog, ctx.Scale(5), ctx.dialogBgColor);
        SetTextColor(hdc, ctx.isDarkDialog ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNTEXT));
    }

    RECT rcText = ctx.rcBtnPrimary;
    DrawTextW(hdc, ctx.primaryText.c_str(), -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    BitBlt(hdcTarget, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdc);
}

void ClearBoundConflict(OpStatusUiContext& ctx) {
    if (ctx.boundConflict.seq != 0) {
        ClearConflictIfSeq(ctx.boundConflict.seq);
        ctx.boundConflict = {};
    }
}

void UpdateLayoutOnUiThread(OpStatusUiContext& ctx, bool forceShowCheck) {
    if (ctx.inLayoutUpdate || !IsWindow(ctx.hwndOp) || !IsWindow(ctx.hwndBar)) return;
    if (forceShowCheck && IsIconic(ctx.hwndOp)) return;

    ctx.inLayoutUpdate = true;

    HWND hwndDui = FindWindowExW(ctx.hwndOp, nullptr, L"DirectUIHWND", nullptr);
    if (!hwndDui) {
        ctx.inLayoutUpdate = false;
        return;
    }

    LONG_PTR duiStyle = GetWindowLongPtrW(hwndDui, GWL_STYLE);
    if ((duiStyle & WS_CLIPSIBLINGS) == 0) {
        SetWindowLongPtrW(hwndDui, GWL_STYLE, duiStyle | WS_CLIPSIBLINGS);
    }

    bool windowVisible = IsWindowVisible(ctx.hwndOp) != FALSE;
    if (!windowVisible) {
        ClearBoundConflict(ctx);
    }

    ConflictSnapshot pending;
    bool hasPending = forceShowCheck && windowVisible && TryGetPendingConflict(GetTickCount(), pending);
    bool hasBound = (ctx.boundConflict.seq != 0);

    NativeButtonRowLayout layout;
    bool wantVisible = false;
    if (forceShowCheck && windowVisible && (hasBound || hasPending)) {
        layout = InspectNativeButtonRow(
            ctx.hwndOp, hwndDui, ctx.uiStrings.tryAgainLabel, ctx.Scale(12), ctx.Scale(20));

        if (!layout.tryAgainVisible) {
            AppendOptionalTraceLine(L"UpdateLayoutOnUiThread: tryAgainVisible=false");
            ClearBoundConflict(ctx);
            hasBound = false;
        } else {
            std::wstring windowTitle = ReadWindowTitleText(ctx.hwndOp);
            if (hasPending && pending.seq > ctx.boundConflict.seq) {
                bool matched = IsFileInUseInterruptDialog(windowTitle, layout.visibleTexts, pending, ctx.uiStrings);
                wchar_t msg[256];
                swprintf_s(msg, L"UpdateLayoutOnUiThread: pendingSeq=%llu texts=%zu matched=%d title=%.80ls",
                           pending.seq, layout.visibleTexts.size(), matched ? 1 : 0, windowTitle.c_str());
                AppendOptionalTraceLine(msg);
                if (matched) {
                    ConflictSnapshot claimed;
                    if (TryClaimPendingConflict(pending.seq, ctx.hwndOp, GetTickCount(), claimed)) {
                        if (hasBound) {
                            ClearConflictIfSeq(ctx.boundConflict.seq);
                        }
                        ctx.boundConflict = std::move(claimed);
                        hasBound = true;
                    }
                }
            }

            if (hasBound) {
                if (IsFileInUseInterruptDialog(windowTitle, layout.visibleTexts, ctx.boundConflict, ctx.uiStrings)) {
                    wantVisible = true;
                } else {
                    ClearBoundConflict(ctx);
                    hasBound = false;
                }
            }
        }
    }

    if (wantVisible) {
        COLORREF detectedBg = ctx.dialogBgColor;
        bool detectedDark = ctx.isDarkDialog;
        if (DetectDialogBackgroundColor(hwndDui, detectedBg, detectedDark)) {
            if (detectedDark != ctx.isDarkDialog) {
                ctx.isDarkDialog = detectedDark;
                ctx.RebuildResources(ctx.hwndBar, ctx.isDarkDialog);
            }
            ctx.dialogBgColor = detectedBg;
        }

        ctx.isHandleOnlyPrimary = !ctx.boundConflict.locks.empty() &&
            ctx.boundConflict.handleOnlyPrimary
                ? true
                : LockFinder::ShouldUseHandleOnlyUnlock(ctx.boundConflict.locks);
        ctx.primaryText = ctx.isHandleOnlyPrimary ? L"Unlock" : L"Kill";

        ctx.EnsureFont();
        int textW = 0;
        if (HDC hdcScreen = GetDC(ctx.hwndBar)) {
            HGDIOBJ oldF = SelectObject(hdcScreen, ctx.hFontBtn);
            SIZE sz = {};
            GetTextExtentPoint32W(hdcScreen, ctx.primaryText.c_str(), static_cast<int>(ctx.primaryText.size()), &sz);
            textW = sz.cx;
            SelectObject(hdcScreen, oldF);
            ReleaseDC(ctx.hwndBar, hdcScreen);
        }

        int baseW = (layout.cancelWidthPx > 0) ? std::min(layout.cancelWidthPx, ctx.Scale(92)) : ctx.Scale(88);
        int desiredW = std::max(baseW, textW + ctx.Scale(32));
        int minInlineW = std::max(ctx.Scale(52), textW + ctx.Scale(16));
        int btnRight = layout.rcLeftmostAction.left - layout.gapPx;
        int availW = btnRight - layout.minAllowedLeftPx;

        if (availW >= minInlineW) {
            int actualW = std::min(desiredW, availW);
            ctx.useFallbackBar = false;
            ctx.rcTargetInOp = { btnRight - actualW, layout.rcTryAgain.top, btnRight, layout.rcTryAgain.bottom };
        } else {
            ctx.useFallbackBar = true;
        }
    }

    ctx.isVisible = wantVisible;

    if (wantVisible && ctx.useFallbackBar && !ctx.fallbackExpanded) {
        ctx.fallbackExpanded = true;
        RECT rcWin = {};
        GetWindowRect(ctx.hwndOp, &rcWin);
        SetWindowPos(ctx.hwndOp, nullptr, 0, 0, rcWin.right - rcWin.left,
                     (rcWin.bottom - rcWin.top) + ctx.fallbackBarHeight,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if ((!wantVisible || !ctx.useFallbackBar) && ctx.fallbackExpanded) {
        ctx.fallbackExpanded = false;
        ShowWindow(ctx.hwndBar, SW_HIDE);
        RECT rcWin = {};
        GetWindowRect(ctx.hwndOp, &rcWin);
        SetWindowPos(ctx.hwndOp, nullptr, 0, 0, rcWin.right - rcWin.left,
                     std::max(150, static_cast<int>(rcWin.bottom - rcWin.top) - ctx.fallbackBarHeight),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT rcClient = {};
    GetClientRect(ctx.hwndOp, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;

    bool themeChanged = (ctx.isDarkDialog != ctx.lastIsDarkDialog) || (ctx.dialogBgColor != ctx.lastDialogBgColor);
    bool targetRectChanged = !EqualRect(&ctx.rcTargetInOp, &ctx.lastTargetInOp);
    bool barVisibleNow = IsWindowVisible(ctx.hwndBar) != FALSE;
    bool stateChanged = !ctx.layoutAppliedOnce || themeChanged ||
        (wantVisible != ctx.lastVisible) || (ctx.useFallbackBar != ctx.lastUseFallbackBar) ||
        (clientW != ctx.lastClientW) || (clientH != ctx.lastClientH) ||
        (wantVisible && (!barVisibleNow || targetRectChanged || ctx.primaryText != ctx.lastPrimaryText)) ||
        (!wantVisible && barVisibleNow);

    if (wantVisible) {
        if (stateChanged) {
            if (ctx.useFallbackBar && clientH > ctx.fallbackBarHeight + 80) {
                SetWindowPos(hwndDui, nullptr, 0, 0, clientW, clientH - ctx.fallbackBarHeight, SWP_NOZORDER | SWP_NOACTIVATE);
                SetWindowPos(ctx.hwndBar, HWND_TOP, 0, clientH - ctx.fallbackBarHeight, clientW, ctx.fallbackBarHeight,
                             SWP_SHOWWINDOW | SWP_NOACTIVATE);
            } else {
                if (clientW != ctx.lastClientW || clientH != ctx.lastClientH || ctx.lastUseFallbackBar) {
                    SetWindowPos(hwndDui, nullptr, 0, 0, clientW, clientH, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                SetWindowPos(ctx.hwndBar, HWND_TOP, ctx.rcTargetInOp.left, ctx.rcTargetInOp.top,
                             ctx.rcTargetInOp.right - ctx.rcTargetInOp.left,
                             ctx.rcTargetInOp.bottom - ctx.rcTargetInOp.top,
                             SWP_SHOWWINDOW | SWP_NOACTIVATE);
            }
            InvalidateRect(ctx.hwndBar, nullptr, FALSE);
        } else if (GetWindow(ctx.hwndOp, GW_CHILD) != ctx.hwndBar) {
            SetWindowPos(ctx.hwndBar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else {
        if (stateChanged) {
            ShowWindow(ctx.hwndBar, SW_HIDE);
            SetWindowPos(hwndDui, nullptr, 0, 0, clientW, clientH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    ctx.layoutAppliedOnce = true;
    ctx.lastVisible = wantVisible;
    ctx.lastUseFallbackBar = ctx.useFallbackBar;
    ctx.lastTargetInOp = ctx.rcTargetInOp;
    ctx.lastPrimaryText = ctx.primaryText;
    ctx.lastClientW = clientW;
    ctx.lastClientH = clientH;
    ctx.lastIsDarkDialog = ctx.isDarkDialog;
    ctx.lastDialogBgColor = ctx.dialogBgColor;
    ctx.inLayoutUpdate = false;
}

LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<OpStatusUiContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE:
        ctx = static_cast<OpStatusUiContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return 0;

    case WM_SIZE:
        if (ctx) ctx->UpdateButtonRect(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_THEMECHANGED:
        if (ctx && !ctx->inRebuildResources) {
            HWND hwndDui = FindWindowExW(ctx->hwndOp, nullptr, L"DirectUIHWND", nullptr);
            DetectDialogBackgroundColor(hwndDui, ctx->dialogBgColor, ctx->isDarkDialog);
            ctx->RebuildResources(hwnd, ctx->isDarkDialog);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PRINTCLIENT:
        if (ctx && wParam) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PaintBarDoubleBuffered(reinterpret_cast<HDC>(wParam), rc, *ctx);
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (ctx) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PaintBarDoubleBuffered(hdc, rc, *ctx);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!ctx) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool hit = PtInRect(&ctx->rcBtnPrimary, pt) != FALSE;
        if (hit != ctx->hovered) {
            ctx->hovered = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (ctx && ctx->hovered) {
            ctx->hovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (ctx && ctx->pressed) {
            ctx->pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!ctx) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&ctx->rcBtnPrimary, pt)) {
            ctx->pressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!ctx) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool hit = PtInRect(&ctx->rcBtnPrimary, pt) != FALSE;
        bool wasPressed = ctx->pressed;
        ctx->pressed = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);

        if (wasPressed && hit) {
            std::wstring path = ctx->boundConflict.targetPath;
            uint64_t seq = ctx->boundConflict.seq;
            UnlockMode mode = ctx->isHandleOnlyPrimary
                ? UnlockMode::CloseHandlesOnly
                : UnlockMode::KillProcessAndCloseHandles;

            ctx->boundConflict = {};
            if (seq != 0) {
                ClearConflictIfSeq(seq);
            }

            if (!path.empty()) {
                // Re-scan live locks on click so stale/exited/recycled PIDs are never targeted.
                LockFinder::UnlockTarget(path, {}, mode);
            }
            UpdateLayoutOnUiThread(*ctx, false);
            SetTimer(ctx->hwndOp, kTimerIdAutoRetry, 35, nullptr);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::mutex g_barClassMutex;
bool g_barClassRegistered = false;

void EnsureBarClassRegistered() {
    std::lock_guard<std::mutex> lock(g_barClassMutex);
    if (g_barClassRegistered) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &BarWndProc;
    wc.hInstance = g_hModule ? g_hModule : GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBarWindowClass;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        g_barClassRegistered = true;
    }
}

LRESULT CALLBACK OpStatusSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = static_cast<OpStatusUiContext*>(GetPropW(hwnd, kSubclassProp));
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);
    WNDPROC origProc = ctx->origOpWndProc;

    if (msg == g_msgAttachOnUiThread && g_msgAttachOnUiThread != 0) {
        UpdateLayoutOnUiThread(*ctx, true);
        return 0;
    }
    if (msg == g_msgDetachOnUiThread && g_msgDetachOnUiThread != 0) {
        KillTimer(hwnd, kTimerIdCheckInterrupt);
        KillTimer(hwnd, kTimerIdAutoRetry);
        ClearBoundConflict(*ctx);
        if (ctx->fallbackExpanded) UpdateLayoutOnUiThread(*ctx, false);
        if (ctx->hwndBar && IsWindow(ctx->hwndBar)) {
            DestroyWindow(ctx->hwndBar);
            ctx->hwndBar = nullptr;
        }
        UnregisterAttachedWindow(hwnd);
        RemovePropW(hwnd, kSubclassProp);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origProc));
        delete ctx;
        return 0;
    }

    switch (msg) {
    case WM_SHOWWINDOW: {
        LRESULT res = CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
        if (wParam == FALSE) {
            ClearBoundConflict(*ctx);
            UpdateLayoutOnUiThread(*ctx, false);
        }
        return res;
    }

    case WM_SIZE: {
        LRESULT res = CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
        if (ctx->isVisible && IsWindow(ctx->hwndBar)) UpdateLayoutOnUiThread(*ctx, true);
        return res;
    }

    case WM_DPICHANGED: {
        LRESULT res = CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
        ctx->dpi = HIWORD(wParam);
        ctx->fallbackBarHeight = ctx->Scale(38);
        ctx->RebuildResources(ctx->hwndBar, ctx->isDarkDialog);
        if (ctx->isVisible) UpdateLayoutOnUiThread(*ctx, true);
        return res;
    }

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE: {
        LRESULT res = CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
        if (ctx->isVisible) {
            UpdateLayoutOnUiThread(*ctx, true);
            InvalidateRect(ctx->hwndBar, nullptr, FALSE);
        }
        return res;
    }

    case WM_TIMER:
        if (wParam == kTimerIdCheckInterrupt) {
            UpdateLayoutOnUiThread(*ctx, true);
            return 0;
        }
        if (wParam == kTimerIdAutoRetry) {
            KillTimer(hwnd, kTimerIdAutoRetry);
            ClickNativeTryAgainButton(hwnd, ctx->uiStrings.tryAgainLabel);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        KillTimer(hwnd, kTimerIdCheckInterrupt);
        KillTimer(hwnd, kTimerIdAutoRetry);
        ClearBoundConflict(*ctx);
        UnregisterAttachedWindow(hwnd);
        RemovePropW(hwnd, kSubclassProp);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origProc));
        delete ctx;
        return CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
    }

    return CallWindowProcW(origProc, hwnd, msg, wParam, lParam);
}

void AttachToOpStatusOnUiThread(HWND hwndOp) {
    if (!IsWindow(hwndOp)) return;

    if (GetPropW(hwndOp, kSubclassProp)) {
        SendMessageW(hwndOp, g_msgAttachOnUiThread, 0, 0);
        return;
    }

    EnsureBarClassRegistered();
    auto* ctx = new (std::nothrow) OpStatusUiContext();
    if (!ctx) return;

    ctx->hwndOp = hwndOp;
    if (auto pGetDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
    {
        if (UINT d = pGetDpi(hwndOp)) ctx->dpi = d;
    }
    ctx->fallbackBarHeight = ctx->Scale(38);
    ctx->uiStrings = LoadFileInUseUiStrings();

    LONG_PTR opStyle = GetWindowLongPtrW(hwndOp, GWL_STYLE);
    if ((opStyle & WS_CLIPCHILDREN) == 0) {
        SetWindowLongPtrW(hwndOp, GWL_STYLE, opStyle | WS_CLIPCHILDREN);
    }

    ctx->hwndBar = CreateWindowExW(
        0, kBarWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 88, 24, hwndOp, nullptr, g_hModule, ctx);
    if (!ctx->hwndBar) {
        wchar_t msg[128];
        swprintf_s(msg, L"AttachToOpStatusOnUiThread: CreateWindowExW failed err=%lu", GetLastError());
        AppendOptionalTraceLine(msg);
        delete ctx;
        return;
    }
    AppendOptionalTraceLine(L"AttachToOpStatusOnUiThread: hwndBar created");
    ctx->RebuildResources(ctx->hwndBar);

    SetPropW(hwndOp, kSubclassProp, ctx);
    ctx->origOpWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwndOp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OpStatusSubclassProc)));
    RegisterAttachedWindow(hwndOp);

    SetTimer(hwndOp, kTimerIdCheckInterrupt, 150, nullptr);
    SendMessageW(hwndOp, g_msgAttachOnUiThread, 0, 0);
}

LRESULT CALLBACK CallWndHookOnOpThread(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam) {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (cwp->message == g_msgAttachOnUiThread && g_msgAttachOnUiThread != 0) {
            AttachToOpStatusOnUiThread(cwp->hwnd);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

DWORD WINAPI TriggerThreadProc(LPVOID param) {
    HANDLE hWake = static_cast<HANDLE>(param);
    DWORD myPid = GetCurrentProcessId();

    while (g_running.load()) {
        ConflictSnapshot pending;
        DWORD waitMs = TryGetPendingConflict(GetTickCount(), pending) ? 150 : INFINITE;
        WaitForSingleObject(hWake, waitMs);
        if (!g_running.load()) break;

        if (!TryGetPendingConflict(GetTickCount(), pending)) continue;

        HWND hwndOp = nullptr;
        while (g_running.load() && (hwndOp = FindWindowExW(nullptr, hwndOp, L"OperationStatusWindow", nullptr)) != nullptr) {
            DWORD winPid = 0;
            DWORD opThreadId = GetWindowThreadProcessId(hwndOp, &winPid);
            if (winPid == myPid && IsWindowVisible(hwndOp) && opThreadId != 0) {
                if (GetPropW(hwndOp, kSubclassProp)) {
                    PostMessageW(hwndOp, g_msgAttachOnUiThread, 0, 0);
                } else {
                    HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, &CallWndHookOnOpThread, g_hModule, opThreadId);
                    if (!hHook) {
                        wchar_t msg[128];
                        swprintf_s(msg, L"TriggerThreadProc: SetWindowsHookExW failed err=%lu tid=%lu", GetLastError(), opThreadId);
                        AppendOptionalTraceLine(msg);
                    } else {
                        DWORD_PTR res = 0;
                        LRESULT sendRes = SendMessageTimeoutW(hwndOp, g_msgAttachOnUiThread, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &res);
                        if (!sendRes) {
                            wchar_t msg[128];
                            swprintf_s(msg, L"TriggerThreadProc: SendMessageTimeoutW failed err=%lu", GetLastError());
                            AppendOptionalTraceLine(msg);
                        }
                        UnhookWindowsHookEx(hHook);
                    }
                }
            }
        }
    }
    return 0;
}

} // namespace

void Initialize(HMODULE hModule) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_running.load()) return;

    g_hModule = hModule;
    if (!g_msgAttachOnUiThread) g_msgAttachOnUiThread = RegisterWindowMessageW(L"HandsOff_AttachOnUiThreadMsg");
    if (!g_msgDetachOnUiThread) g_msgDetachOnUiThread = RegisterWindowMessageW(L"HandsOff_DetachOnUiThreadMsg");

    std::wstring wakeName = L"Local\\HandsOffWake_" + std::to_wstring(GetCurrentProcessId());
    HANDLE hWake = CreateEventW(nullptr, FALSE, FALSE, wakeName.c_str());
    if (!hWake) return;

    {
        std::lock_guard<std::mutex> wakeLock(g_wakeEventMutex);
        g_hWakeEvent = hWake;
    }
    g_running.store(true);

    HANDLE hThread = CreateThread(nullptr, 0, &TriggerThreadProc, hWake, 0, nullptr);
    if (!hThread) {
        g_running.store(false);
        {
            std::lock_guard<std::mutex> wakeLock(g_wakeEventMutex);
            g_hWakeEvent = nullptr;
        }
        CloseHandle(hWake);
        return;
    }
    g_hTriggerThread = hThread;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!g_running.exchange(false)) return;

    HANDLE hWake = nullptr;
    {
        std::lock_guard<std::mutex> wakeLock(g_wakeEventMutex);
        hWake = g_hWakeEvent;
        g_hWakeEvent = nullptr;
        if (hWake) SetEvent(hWake);
    }

    bool threadExited = true;
    if (g_hTriggerThread) {
        threadExited = (WaitForSingleObject(g_hTriggerThread, 2000) == WAIT_OBJECT_0);
        CloseHandle(g_hTriggerThread);
        g_hTriggerThread = nullptr;
    }
    if (hWake && threadExited) {
        CloseHandle(hWake);
    }

    if (g_msgDetachOnUiThread) {
        std::vector<HWND> windows;
        {
            std::lock_guard<std::mutex> winLock(g_attachedWindowsMutex);
            windows = g_attachedWindows;
        }
        for (HWND hwndOp : windows) {
            if (IsWindow(hwndOp)) {
                DWORD_PTR res = 0;
                SendMessageTimeoutW(hwndOp, g_msgDetachOnUiThread, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, &res);
            }
        }
    }

    {
        std::lock_guard<std::mutex> clsLock(g_barClassMutex);
        if (g_barClassRegistered) {
            HINSTANCE hInst = g_hModule ? g_hModule : GetModuleHandleW(nullptr);
            if (UnregisterClassW(kBarWindowClass, hInst) || GetLastError() == ERROR_CLASS_DOES_NOT_EXIST) {
                g_barClassRegistered = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> conflictLock(g_conflictMutex);
        g_activeConflict = {};
    }
}

bool IsRunning() {
    return g_running.load();
}

void OnConflictDetected(const std::wstring& targetPath, const std::vector<LockedProcessInfo>& locks) {
    std::wstring summary = LockFinder::FormatBlockerSummary(locks);
    bool handleOnly = LockFinder::ShouldUseHandleOnlyUnlock(locks);
    DWORD nowTick = GetTickCount();

    {
        std::lock_guard<std::mutex> lock(g_conflictMutex);
        uint64_t assignedSeq = 0;
        if (g_sharedConflict.EnsureOpen()) {
            ScopedNamedMutex sharedLock(g_sharedConflict.hMutex);
            auto* s = g_sharedConflict.view;
            assignedSeq = ++s->nextSeq;
            s->seq = assignedSeq;
            s->detectedTick = nowTick;
            s->handleOnlyPrimary = handleOnly ? 1u : 0u;
            s->firstLockPid = locks.empty() ? 0u : locks[0].pid;
            s->claimedByHwnd = 0;
            wcsncpy_s(s->targetPath, targetPath.c_str(), _TRUNCATE);
            wcsncpy_s(s->blockerSummary, summary.c_str(), _TRUNCATE);
            if (!locks.empty()) {
                wcsncpy_s(s->firstLockProcessName, locks[0].processName.c_str(), _TRUNCATE);
            } else {
                s->firstLockProcessName[0] = L'\0';
            }
        } else {
            assignedSeq = ++g_nextConflictSeq;
        }

        g_nextConflictSeq = std::max(g_nextConflictSeq, assignedSeq);
        g_activeConflict.seq = assignedSeq;
        g_activeConflict.targetPath = targetPath;
        g_activeConflict.locks = locks;
        g_activeConflict.blockerSummary = std::move(summary);
        g_activeConflict.detectedTick = nowTick;
        g_activeConflict.claimedByHwnd = nullptr;
        g_activeConflict.handleOnlyPrimary = handleOnly;
    }

    {
        std::lock_guard<std::mutex> wakeLock(g_wakeEventMutex);
        if (g_running.load() && g_hWakeEvent) SetEvent(g_hWakeEvent);
    }
    WakeAllExplorerTriggerThreads();
}

std::wstring NormalizeDialogText(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    bool inSpace = true;
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            if (!inSpace) {
                out.push_back(L' ');
                inSpace = true;
            }
        } else {
            out.push_back(ch);
            inSpace = false;
        }
    }
    if (!out.empty() && out.back() == L' ') {
        out.pop_back();
    }
    return out;
}

FileInUseUiStrings LoadFileInUseUiStrings() {
    FileInUseUiStrings s;
    s.tryAgainLabel = LoadShell32CleanString(0x81CF);
    if (s.tryAgainLabel.empty()) s.tryAgainLabel = L"Try Again";

    s.fileInUseTitle = NormalizeAndLowerDialogText(LoadShell32CleanString(0x4142));
    s.folderInUseTitle = NormalizeAndLowerDialogText(LoadShell32CleanString(0x4143));

    constexpr UINT kTemplateIds[] = {
        0x41AB, 0x41AC, 0x41AD, 0x41AE,
        0x41AF, 0x41B0, 0x41B1, 0x41B2
    };
    for (UINT id : kTemplateIds) {
        FileInUseTemplate tmpl = ParseFileInUseTemplate(LoadShell32RawString(id));
        if (!tmpl.prefix.empty() || !tmpl.suffix.empty()) {
            s.templates.push_back(std::move(tmpl));
        }
    }
    return s;
}

bool IsFileInUseInterruptDialog(
    const std::wstring& windowTitle,
    const std::vector<std::wstring>& visibleTexts,
    const ConflictSnapshot& conflict,
    const FileInUseUiStrings& uiStrings)
{
    if (conflict.seq == 0 || conflict.targetPath.empty()) return false;

    std::wstring normTitle = NormalizeAndLowerDialogText(windowTitle);
    std::vector<std::wstring> normTexts;
    normTexts.reserve(visibleTexts.size());
    for (const auto& t : visibleTexts) {
        std::wstring n = NormalizeAndLowerDialogText(t);
        if (!n.empty()) normTexts.push_back(std::move(n));
    }

    bool titleIsFileInUse =
        (!uiStrings.fileInUseTitle.empty() && normTitle == uiStrings.fileInUseTitle) ||
        (!uiStrings.folderInUseTitle.empty() && normTitle == uiStrings.folderInUseTitle);

    bool textHasTitle = false;
    bool textMatchesTemplate = false;
    for (const auto& t : normTexts) {
        if ((!uiStrings.fileInUseTitle.empty() && t == uiStrings.fileInUseTitle) ||
            (!uiStrings.folderInUseTitle.empty() && t == uiStrings.folderInUseTitle))
        {
            textHasTitle = true;
        }
        for (const auto& tmpl : uiStrings.templates) {
            if (MatchesTemplate(t, tmpl)) {
                textMatchesTemplate = true;
                break;
            }
        }
        if (textHasTitle && textMatchesTemplate) break;
    }

    if (!titleIsFileInUse && !textHasTitle && !textMatchesTemplate) {
        return false;
    }

    if (!normTexts.empty()) {
        return MatchesConflictSummary(normTexts, conflict);
    }

    return titleIsFileInUse;
}

bool TryGetPendingConflict(DWORD nowTick, ConflictSnapshot& outSnapshot) {
    std::lock_guard<std::mutex> lock(g_conflictMutex);
    if (g_sharedConflict.EnsureOpen()) {
        ScopedNamedMutex sharedLock(g_sharedConflict.hMutex);
        auto* s = g_sharedConflict.view;
        if (s->seq == 0) {
            g_activeConflict = {};
            return false;
        }
        if (s->claimedByHwnd != 0) {
            HWND claimedHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(s->claimedByHwnd));
            if (!IsWindow(claimedHwnd)) {
                s->seq = 0;
                s->claimedByHwnd = 0;
                g_activeConflict = {};
            }
            return false;
        }
        if (static_cast<DWORD>(nowTick - s->detectedTick) > kPendingConflictTtlMs) {
            s->seq = 0;
            g_activeConflict = {};
            return false;
        }
        if (g_activeConflict.seq == s->seq && g_activeConflict.claimedByHwnd == nullptr) {
            outSnapshot = g_activeConflict;
        } else {
            outSnapshot = SnapshotFromShared(*s);
        }
        return true;
    }

    if (g_activeConflict.seq == 0) return false;

    if (g_activeConflict.claimedByHwnd != nullptr) {
        if (!IsWindow(g_activeConflict.claimedByHwnd)) {
            g_activeConflict = {};
        }
        return false;
    }

    if (static_cast<DWORD>(nowTick - g_activeConflict.detectedTick) > kPendingConflictTtlMs) {
        g_activeConflict = {};
        return false;
    }

    outSnapshot = g_activeConflict;
    return true;
}

bool TryClaimPendingConflict(uint64_t seq, HWND hwndOwner, DWORD nowTick, ConflictSnapshot& outSnapshot) {
    if (seq == 0 || hwndOwner == nullptr) return false;

    std::lock_guard<std::mutex> lock(g_conflictMutex);
    if (g_sharedConflict.EnsureOpen()) {
        ScopedNamedMutex sharedLock(g_sharedConflict.hMutex);
        auto* s = g_sharedConflict.view;
        if (s->seq != seq || s->claimedByHwnd != 0) {
            return false;
        }
        if (static_cast<DWORD>(nowTick - s->detectedTick) > kPendingConflictTtlMs) {
            s->seq = 0;
            g_activeConflict = {};
            return false;
        }
        s->claimedByHwnd = static_cast<uint64_t>(reinterpret_cast<ULONG_PTR>(hwndOwner));
        if (g_activeConflict.seq == seq) {
            g_activeConflict.claimedByHwnd = hwndOwner;
            outSnapshot = g_activeConflict;
        } else {
            outSnapshot = SnapshotFromShared(*s);
        }
        return true;
    }

    if (g_activeConflict.seq != seq || g_activeConflict.claimedByHwnd != nullptr) {
        return false;
    }
    if (static_cast<DWORD>(nowTick - g_activeConflict.detectedTick) > kPendingConflictTtlMs) {
        g_activeConflict = {};
        return false;
    }

    g_activeConflict.claimedByHwnd = hwndOwner;
    outSnapshot = g_activeConflict;
    return true;
}

void ClearConflictIfSeq(uint64_t seq) {
    if (seq == 0) return;
    std::lock_guard<std::mutex> lock(g_conflictMutex);
    if (g_sharedConflict.EnsureOpen()) {
        ScopedNamedMutex sharedLock(g_sharedConflict.hMutex);
        if (g_sharedConflict.view->seq == seq) {
            g_sharedConflict.view->seq = 0;
            g_sharedConflict.view->claimedByHwnd = 0;
        }
    }
    if (g_activeConflict.seq == seq) {
        g_activeConflict = {};
    }
}

void ResetConflictStateForTests() {
    std::lock_guard<std::mutex> lock(g_conflictMutex);
    if (g_sharedConflict.EnsureOpen()) {
        ScopedNamedMutex sharedLock(g_sharedConflict.hMutex);
        g_sharedConflict.view->seq = 0;
        g_sharedConflict.view->claimedByHwnd = 0;
    }
    g_activeConflict = {};
}

} // namespace HandsOff::DialogEnhancer
