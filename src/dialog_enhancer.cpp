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

std::mutex g_conflictMutex;
std::wstring g_activeTargetPath;
std::vector<LockedProcessInfo> g_activeLocks;
DWORD g_lastConflictTick = 0;

std::mutex g_lifecycleMutex;
std::mutex g_wakeEventMutex;
HMODULE g_hModule = nullptr;
std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_conflictSeq{ 0 };
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

std::wstring LoadShell32CleanString(UINT resourceId) {
    HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
    if (!hShell32) return {};
    wchar_t buf[256] = {};
    int len = LoadStringW(hShell32, resourceId, buf, 255);
    return (len > 0) ? StripAcceleratorAmpersands(buf) : L"";
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
bool VisitAccessibleButtons(IAccessible* pAcc, Callback&& onButton, int depth = 0) {
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

        if (role != ROLE_SYSTEM_PUSHBUTTON && role != ROLE_SYSTEM_CHECKBUTTON) return false;

        std::wstring name;
        BSTR bstrName = nullptr;
        if (SUCCEEDED(accObj->get_accName(varChild, &bstrName)) && bstrName) {
            name = StripAcceleratorAmpersands(bstrName);
            SysFreeString(bstrName);
        }
        return onButton(accObj, varChild, role, name);
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
                        stopped = VisitAccessibleButtons(pChildAcc, onButton, depth + 1);
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
    VisitAccessibleButtons(pRootAcc, [&](IAccessible* accObj, const VARIANT& varChild, long role, const std::wstring& name) {
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
        bool invoked = VisitAccessibleButtons(pRootAcc, [&](IAccessible* accObj, const VARIANT& varChild, long role, const std::wstring& name) {
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
    std::wstring tryAgainLabel;

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
    DWORD suppressShowUntilTick = 0;
    uint64_t lastSeenConflictSeq = 0;

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

void UpdateLayoutOnUiThread(OpStatusUiContext& ctx, bool forceShowCheck) {
    if (ctx.inLayoutUpdate || !IsWindow(ctx.hwndOp) || !IsWindow(ctx.hwndBar)) return;
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

    uint64_t curSeq = g_conflictSeq.load();
    if (curSeq != ctx.lastSeenConflictSeq) {
        ctx.lastSeenConflictSeq = curSeq;
        ctx.suppressShowUntilTick = 0;
    }
    if (forceShowCheck && ctx.suppressShowUntilTick != 0) {
        if (static_cast<int>(GetTickCount() - ctx.suppressShowUntilTick) < 0) {
            forceShowCheck = false;
        } else {
            ctx.suppressShowUntilTick = 0;
        }
    }

    NativeButtonRowLayout layout = InspectNativeButtonRow(
        ctx.hwndOp, hwndDui, ctx.tryAgainLabel, ctx.Scale(12), ctx.Scale(20));

    bool wantVisible = forceShowCheck && layout.tryAgainVisible;
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

        std::vector<LockedProcessInfo> locks;
        {
            std::lock_guard<std::mutex> lock(g_conflictMutex);
            locks = g_activeLocks;
        }
        ctx.isHandleOnlyPrimary = LockFinder::ShouldUseHandleOnlyUnlock(locks);
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
            std::wstring path;
            std::vector<LockedProcessInfo> locks;
            {
                std::lock_guard<std::mutex> lock(g_conflictMutex);
                path = g_activeTargetPath;
                locks = g_activeLocks;
            }
            if (!path.empty()) {
                UnlockMode mode = ctx->isHandleOnlyPrimary
                    ? UnlockMode::CloseHandlesOnly
                    : UnlockMode::KillProcessAndCloseHandles;
                LockFinder::UnlockTarget(path, locks, mode);
            }
            ctx->suppressShowUntilTick = GetTickCount() + 250;
            ctx->lastSeenConflictSeq = g_conflictSeq.load();
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
            ClickNativeTryAgainButton(hwnd, ctx->tryAgainLabel);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        KillTimer(hwnd, kTimerIdCheckInterrupt);
        KillTimer(hwnd, kTimerIdAutoRetry);
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
    ctx->tryAgainLabel = LoadShell32CleanString(0x81CF);
    if (ctx->tryAgainLabel.empty()) ctx->tryAgainLabel = L"Try Again";

    LONG_PTR opStyle = GetWindowLongPtrW(hwndOp, GWL_STYLE);
    if ((opStyle & WS_CLIPCHILDREN) == 0) {
        SetWindowLongPtrW(hwndOp, GWL_STYLE, opStyle | WS_CLIPCHILDREN);
    }

    ctx->hwndBar = CreateWindowExW(
        0, kBarWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 88, 24, hwndOp, nullptr, g_hModule, ctx);
    if (!ctx->hwndBar) {
        delete ctx;
        return;
    }
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
        WaitForSingleObject(hWake, 400);
        if (!g_running.load()) break;

        DWORD lastTick = 0;
        {
            std::lock_guard<std::mutex> lock(g_conflictMutex);
            lastTick = g_lastConflictTick;
        }
        if (lastTick == 0 || (GetTickCount() - lastTick > 60000)) continue;

        HWND hwndOp = nullptr;
        while (g_running.load() && (hwndOp = FindWindowExW(nullptr, hwndOp, L"OperationStatusWindow", nullptr)) != nullptr) {
            DWORD winPid = 0;
            DWORD opThreadId = GetWindowThreadProcessId(hwndOp, &winPid);
            if (winPid == myPid && IsWindowVisible(hwndOp) && opThreadId != 0) {
                if (GetPropW(hwndOp, kSubclassProp)) {
                    PostMessageW(hwndOp, g_msgAttachOnUiThread, 0, 0);
                } else if (HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, &CallWndHookOnOpThread, nullptr, opThreadId)) {
                    DWORD_PTR res = 0;
                    SendMessageTimeoutW(hwndOp, g_msgAttachOnUiThread, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &res);
                    UnhookWindowsHookEx(hHook);
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

    HANDLE hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
}

bool IsRunning() {
    return g_running.load();
}

void OnConflictDetected(const std::wstring& targetPath, const std::vector<LockedProcessInfo>& locks) {
    {
        std::lock_guard<std::mutex> lock(g_conflictMutex);
        g_activeTargetPath = targetPath;
        g_activeLocks = locks;
        g_lastConflictTick = GetTickCount();
    }
    ++g_conflictSeq;
    std::lock_guard<std::mutex> wakeLock(g_wakeEventMutex);
    if (g_running.load() && g_hWakeEvent) SetEvent(g_hWakeEvent);
}

} // namespace HandsOff::DialogEnhancer
