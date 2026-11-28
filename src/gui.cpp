#include "gui.h"
#include <dwmapi.h>
#include <windowsx.h>

namespace HandsOff {

namespace {

constexpr const wchar_t* kGuiWindowClass = L"HandsOffMinimalGuiWnd";
constexpr UINT_PTR kTimerPollId = 1;
constexpr int kClientWidth = 296;
constexpr int kClientHeightIdle = 104;
constexpr int kClientHeightFeedback = 124;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

enum class ButtonId { None = 0, Primary = 1, Secondary = 2 };

struct ButtonColors {
    COLORREF fill[3];
    COLORREF border[3];
    COLORREF text[3];
};

struct ThemeColors {
    COLORREF canvasBg;
    COLORREF frameBorder;
    COLORREF cardBg;
    COLORREF cardBorder;
    COLORREF dotActive;
    COLORREF dotPending;
    COLORREF dotInactive;
    COLORREF statusText;
    COLORREF metaText;
    COLORREF footerText;
    COLORREF footerError;
    ButtonColors btn[3]; // 0=Install, 1=Uninstall, 2=Reapply
};

constexpr ThemeColors kDarkTheme = {
    RGB(24, 25, 29), RGB(44, 47, 56),
    RGB(31, 33, 39), RGB(44, 47, 56),
    RGB(72, 199, 142), RGB(229, 181, 79), RGB(110, 118, 133),
    RGB(228, 232, 240), RGB(131, 139, 155),
    RGB(131, 139, 155), RGB(222, 116, 110),
    {
        { { RGB(43, 91, 148), RGB(56, 111, 176), RGB(34, 72, 117) },
          { RGB(62, 114, 176), RGB(82, 140, 208), RGB(53, 98, 152) },
          { RGB(240, 244, 250), RGB(255, 255, 255), RGB(220, 228, 240) } },
        { { RGB(40, 35, 40), RGB(61, 42, 48), RGB(32, 27, 31) },
          { RGB(72, 55, 60), RGB(112, 71, 79), RGB(84, 57, 64) },
          { RGB(223, 190, 195), RGB(242, 216, 220), RGB(204, 170, 176) } },
        { { RGB(36, 39, 48), RGB(49, 54, 67), RGB(28, 31, 38) },
          { RGB(56, 61, 75), RGB(82, 90, 111), RGB(49, 54, 67) },
          { RGB(203, 209, 223), RGB(232, 236, 245), RGB(178, 186, 201) } }
    }
};

constexpr ThemeColors kLightTheme = {
    RGB(246, 247, 249), RGB(214, 219, 228),
    RGB(255, 255, 255), RGB(224, 228, 235),
    RGB(22, 163, 98), RGB(202, 138, 4), RGB(142, 150, 164),
    RGB(28, 32, 38), RGB(102, 110, 125),
    RGB(102, 110, 125), RGB(185, 48, 48),
    {
        { { RGB(0, 103, 192), RGB(20, 118, 204), RGB(0, 86, 162) },
          { RGB(0, 90, 170), RGB(16, 106, 186), RGB(0, 74, 142) },
          { RGB(255, 255, 255), RGB(255, 255, 255), RGB(235, 243, 252) } },
        { { RGB(253, 244, 245), RGB(250, 231, 233), RGB(244, 216, 220) },
          { RGB(228, 194, 198), RGB(212, 162, 168), RGB(196, 142, 149) },
          { RGB(154, 48, 58), RGB(134, 34, 44), RGB(118, 28, 38) } },
        { { RGB(255, 255, 255), RGB(241, 243, 247), RGB(232, 235, 240) },
          { RGB(214, 219, 228), RGB(188, 195, 208), RGB(174, 182, 196) },
          { RGB(36, 41, 49), RGB(22, 26, 32), RGB(64, 70, 82) } }
    }
};

struct GuiState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool isDarkMode = true;
    PatchStatusInfo status;
    std::wstring feedbackText;
    bool feedbackIsError = false;

    RECT rcPrimaryBtn = {};
    RECT rcSecondaryBtn = {};
    ButtonId hoveredBtn = ButtonId::None;
    ButtonId pressedBtn = ButtonId::None;

    HFONT fontMeta = nullptr;
    HFONT fontStatus = nullptr;
    HFONT fontButton = nullptr;
    HFONT fontFooter = nullptr;

    int Scale(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
    int DesiredClientHeight() const { return feedbackText.empty() ? kClientHeightIdle : kClientHeightFeedback; }

    void DestroyFonts() {
        for (HFONT* f : { &fontMeta, &fontStatus, &fontButton, &fontFooter }) {
            if (*f) { DeleteObject(*f); *f = nullptr; }
        }
    }

    void RebuildFonts() {
        DestroyFonts();
        if (dpi == 0) return;
        auto makeFont = [&](int sizePx, int weight) {
            LOGFONTW lf = {};
            lf.lfHeight = -Scale(sizePx);
            lf.lfWeight = weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            lf.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            return CreateFontIndirectW(&lf);
        };
        fontMeta   = makeFont(11, FW_NORMAL);
        fontStatus = makeFont(13, FW_SEMIBOLD);
        fontButton = makeFont(12, FW_SEMIBOLD);
        fontFooter = makeFont(11, FW_NORMAL);
    }

    ~GuiState() { DestroyFonts(); }
};

void ApplyWin11WindowAttributes(HWND hwnd, bool dark) {
    const auto& pal = dark ? kDarkTheme : kLightTheme;
    BOOL darkAttr = dark ? TRUE : FALSE;
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkAttr, sizeof(darkAttr));
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &pal.canvasBg, sizeof(pal.canvasBg));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &pal.frameBorder, sizeof(pal.frameBorder));
}

bool AdjustWindowRectForDpiCompat(RECT* rc, DWORD style, DWORD exStyle, UINT dpi) {
    using PFN_AdjustDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (auto pAdj = reinterpret_cast<PFN_AdjustDpi>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")))
    {
        return pAdj(rc, style, FALSE, exStyle, dpi) != FALSE;
    }
    return AdjustWindowRectEx(rc, style, FALSE, exStyle) != FALSE;
}

void SyncWindowClientSize(HWND hwnd, const GuiState& st) {
    if (!IsWindow(hwnd)) return;
    RECT rcDesired = { 0, 0, st.Scale(kClientWidth), st.Scale(st.DesiredClientHeight()) };
    auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectForDpiCompat(&rcDesired, style, exStyle, st.dpi);

    RECT rcCur = {};
    GetWindowRect(hwnd, &rcCur);
    int newW = rcDesired.right - rcDesired.left;
    int newH = rcDesired.bottom - rcDesired.top;
    if ((rcCur.right - rcCur.left) != newW || (rcCur.bottom - rcCur.top) != newH) {
        SetWindowPos(hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void DrawRoundedBox(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
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

void DrawCircleDot(HDC hdc, int cx, int cy, int r, COLORREF color) {
    HBRUSH hBrush = CreateSolidBrush(color);
    HPEN hPen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldB = SelectObject(hdc, hBrush);
    HGDIOBJ oldP = SelectObject(hdc, hPen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldP);
    SelectObject(hdc, oldB);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

void DrawButton(HDC hdc, const RECT& rc, int radius, const ButtonColors& colors,
                ButtonId id, ButtonId hovered, ButtonId pressed, const wchar_t* label)
{
    int idx = (pressed == id) ? 2 : (hovered == id ? 1 : 0);
    DrawRoundedBox(hdc, rc, radius, colors.fill[idx], colors.border[idx]);
    SetTextColor(hdc, colors.text[idx]);
    RECT rcText = rc;
    DrawTextW(hdc, label, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void RenderGui(HDC hdcTarget, const RECT& rcClient, GuiState& st) {
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;
    if (w <= 0 || h <= 0) return;

    const auto& pal = st.isDarkMode ? kDarkTheme : kLightTheme;
    HDC hdc = CreateCompatibleDC(hdcTarget);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcTarget, w, h);
    HGDIOBJ oldBmp = SelectObject(hdc, hBmp);
    SetBkMode(hdc, TRANSPARENT);

    HBRUSH hBg = CreateSolidBrush(pal.canvasBg);
    RECT rcLocal = { 0, 0, w, h };
    FillRect(hdc, &rcLocal, hBg);
    DeleteObject(hBg);

    int padX = st.Scale(16);
    int curY = st.Scale(14);
    int radius = st.Scale(10);

    bool isPatched = st.status.isLoadedInExplorer || st.status.isComRegistered;
    bool isFullyLive = st.status.isLoadedInExplorer && st.status.isComRegistered;

    int cardH = st.Scale(36);
    RECT rcCard = { padX, curY, w - padX, curY + cardH };
    DrawRoundedBox(hdc, rcCard, radius, pal.cardBg, pal.cardBorder);

    int innerX = rcCard.left + st.Scale(12);
    int innerRight = rcCard.right - st.Scale(12);
    int cardCy = (rcCard.top + rcCard.bottom) / 2;

    COLORREF dotColor = isFullyLive ? pal.dotActive : (isPatched ? pal.dotPending : pal.dotInactive);
    DrawCircleDot(hdc, innerX + st.Scale(3), cardCy, st.Scale(3), dotColor);

    const wchar_t* statusWord = isFullyLive ? L"Active" : (isPatched ? L"Pending" : L"Inactive");
    SelectObject(hdc, st.fontStatus);
    SetTextColor(hdc, pal.statusText);
    RECT rcStatus = { innerX + st.Scale(14), rcCard.top, innerX + st.Scale(92), rcCard.bottom };
    DrawTextW(hdc, statusWord, -1, &rcStatus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    std::wstring meta;
    if (isFullyLive) {
        meta = st.status.explorerPid ? (L"explorer.exe \x00B7 " + UintToWString(st.status.explorerPid)) : L"explorer.exe";
    } else if (isPatched) {
        meta = L"Restart Explorer";
    }
    if (!meta.empty()) {
        SelectObject(hdc, st.fontMeta);
        SetTextColor(hdc, pal.metaText);
        RECT rcMeta = { innerX + st.Scale(82), rcCard.top, innerRight, rcCard.bottom };
        DrawTextW(hdc, meta.c_str(), -1, &rcMeta, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    curY += cardH + st.Scale(10);
    int btnH = st.Scale(30);
    SelectObject(hdc, st.fontButton);

    if (!isPatched) {
        st.rcPrimaryBtn = { padX, curY, w - padX, curY + btnH };
        SetRectEmpty(&st.rcSecondaryBtn);
        DrawButton(hdc, st.rcPrimaryBtn, radius, pal.btn[0], ButtonId::Primary, st.hoveredBtn, st.pressedBtn, L"Install");
    } else {
        int gap = st.Scale(8);
        int halfW = (w - padX * 2 - gap) / 2;
        st.rcPrimaryBtn = { padX, curY, padX + halfW, curY + btnH };
        st.rcSecondaryBtn = { padX + halfW + gap, curY, w - padX, curY + btnH };
        DrawButton(hdc, st.rcPrimaryBtn, radius, pal.btn[1], ButtonId::Primary, st.hoveredBtn, st.pressedBtn, L"Uninstall");
        DrawButton(hdc, st.rcSecondaryBtn, radius, pal.btn[2], ButtonId::Secondary, st.hoveredBtn, st.pressedBtn, L"Reapply");
    }

    curY += btnH + st.Scale(8);
    if (!st.feedbackText.empty()) {
        SelectObject(hdc, st.fontFooter);
        SetTextColor(hdc, st.feedbackIsError ? pal.footerError : pal.footerText);
        RECT rcFooter = { padX, curY, w - padX, h - st.Scale(10) };
        DrawTextW(hdc, st.feedbackText.c_str(), -1, &rcFooter,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    BitBlt(hdcTarget, rcClient.left, rcClient.top, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdc);
}

ButtonId HitTestButtons(const GuiState& st, POINT pt) {
    if (PtInRect(&st.rcPrimaryBtn, pt)) return ButtonId::Primary;
    if (!IsRectEmpty(&st.rcSecondaryBtn) && PtInRect(&st.rcSecondaryBtn, pt)) return ButtonId::Secondary;
    return ButtonId::None;
}

LRESULT CALLBACK GuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<GuiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE:
        st = static_cast<GuiState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->isDarkMode = IsSystemDarkMode(true);
        ApplyWin11WindowAttributes(hwnd, st->isDarkMode);
        st->status = QueryPatchStatus();
        SetTimer(hwnd, kTimerPollId, 1500, nullptr);
        return 0;

    case WM_SETTINGCHANGE:
        if (st && lParam && _wcsicmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            bool darkNow = IsSystemDarkMode(true);
            if (darkNow != st->isDarkMode) {
                st->isDarkMode = darkNow;
                ApplyWin11WindowAttributes(hwnd, st->isDarkMode);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_DPICHANGED: {
        if (!st) break;
        st->dpi = HIWORD(wParam);
        st->RebuildFonts();
        auto* rcNew = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rcNew->left, rcNew->top,
                     rcNew->right - rcNew->left, rcNew->bottom - rcNew->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (st && wParam == kTimerPollId) {
            PatchStatusInfo fresh = QueryPatchStatus();
            if (fresh.isComRegistered != st->status.isComRegistered ||
                fresh.isLoadedInExplorer != st->status.isLoadedInExplorer ||
                fresh.explorerPid != st->status.explorerPid)
            {
                st->status = fresh;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (st) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            RenderGui(hdc, rc, *st);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (st && LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (HitTestButtons(*st, pt) != ButtonId::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_MOUSEMOVE: {
        if (!st) break;
        ButtonId hit = HitTestButtons(*st, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        if (hit != st->hoveredBtn) {
            st->hoveredBtn = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (st && st->hoveredBtn != ButtonId::None) {
            st->hoveredBtn = ButtonId::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (st && st->pressedBtn != ButtonId::None) {
            st->pressedBtn = ButtonId::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!st) break;
        ButtonId hit = HitTestButtons(*st, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        if (hit != ButtonId::None) {
            st->pressedBtn = hit;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!st) break;
        ButtonId releasedOn = HitTestButtons(*st, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        ButtonId wasPressed = st->pressedBtn;
        st->pressedBtn = ButtonId::None;
        if (GetCapture() == hwnd) ReleaseCapture();

        if (wasPressed != ButtonId::None && wasPressed == releasedOn) {
            bool isPatched = st->status.isLoadedInExplorer || st->status.isComRegistered;
            PatchOperationResult res = (wasPressed == ButtonId::Primary && isPatched)
                ? RollbackPatch()
                : ApplyPatch();

            st->feedbackText = res.statusMessage;
            st->feedbackIsError = !res.success;
            st->status = QueryPatchStatus();
            st->hoveredBtn = ButtonId::None;
            SyncWindowClientSize(hwnd, *st);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerPollId);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int RunGuiWindow(HINSTANCE hInstance) {
    if (auto pSetDpi = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
    {
        pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &GuiWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_SHIELD);
    wc.lpszClassName = kGuiWindowClass;
    RegisterClassExW(&wc);

    GuiState state;
    if (HDC hdc = GetDC(nullptr)) {
        if (UINT d = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX))) state.dpi = d;
        ReleaseDC(nullptr, hdc);
    }
    state.RebuildFonts();

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rcWin = { 0, 0, state.Scale(kClientWidth), state.Scale(state.DesiredClientHeight()) };
    AdjustWindowRectForDpiCompat(&rcWin, style, 0, state.dpi);

    int winW = rcWin.right - rcWin.left;
    int winH = rcWin.bottom - rcWin.top;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        0, kGuiWindowClass, L"HandsOff", style,
        posX, posY, winW, winH, nullptr, nullptr, hInstance, &state);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace HandsOff
