// SPDX-License-Identifier: MIT
//
// See reaper_toast.h (13-68, P9).

#include "reaper_toast.h"

#if defined(_WIN32) && !defined(RAV_HOST_TEST)

#include <algorithm>
#include <string>

namespace rav {
namespace {

// The house theme (ui_theme.h): kRaised, kStrokeStrong, kText.
constexpr COLORREF kToastBg = RGB(0x20, 0x23, 0x2A);
constexpr COLORREF kToastEdge = RGB(0x3A, 0x3F, 0x49);
constexpr COLORREF kToastText = RGB(0xE7, 0xE9, 0xEE);
constexpr wchar_t  kToastClass[] = L"RAV_ReaperToast";
constexpr UINT_PTR kToastTimer = 1;
constexpr UINT     kTickMs = 40;
constexpr DWORD    kFadeMs = 600;
constexpr BYTE     kAlpha = 242;  // ~95 %

HINSTANCE g_instance = nullptr;
HWND      g_main = nullptr;
HWND      g_toast = nullptr;
bool      g_class = false;
HFONT     g_font = nullptr;
std::wstring g_text;
ULONGLONG g_shown_at = 0;
DWORD     g_hold_ms = 4000;
int       g_pad_x = 14, g_pad_y = 8;

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// The main window's DPI scale (96 dpi = 1).
float ToastScale(HWND wnd)
{
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    UINT dpi = 0;
    if (dpi_for_window && wnd) dpi = dpi_for_window(wnd);
    if (dpi == 0) {
        HDC dc = GetDC(nullptr);
        dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
        if (dc) ReleaseDC(nullptr, dc);
    }
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

void ToastPaint(HWND wnd)
{
    PAINTSTRUCT ps;
    HDC         dc = BeginPaint(wnd, &ps);
    if (!dc) return;
    RECT rc;
    GetClientRect(wnd, &rc);
    HBRUSH bg = CreateSolidBrush(kToastBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn && GetWindowRgn(wnd, rgn) != ERROR) {
        HBRUSH edge = CreateSolidBrush(kToastEdge);
        FrameRgn(dc, rgn, edge, 1, 1);
        DeleteObject(edge);
    }
    if (rgn) DeleteObject(rgn);
    HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kToastText);
    RECT tr = {rc.left + g_pad_x, rc.top + g_pad_y, rc.right - g_pad_x, rc.bottom - g_pad_y};
    DrawTextW(dc, g_text.c_str(), static_cast<int>(g_text.size()), &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
    EndPaint(wnd, &ps);
}

LRESULT CALLBACK ToastProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        ToastPaint(wnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == kToastTimer) {
            const ULONGLONG age = GetTickCount64() - g_shown_at;
            if (age >= static_cast<ULONGLONG>(g_hold_ms) + kFadeMs) {
                DestroyWindow(wnd);
            } else if (age > g_hold_ms) {
                const double left = 1.0 - static_cast<double>(age - g_hold_ms) / static_cast<double>(kFadeMs);
                SetLayeredWindowAttributes(wnd, 0, static_cast<BYTE>(kAlpha * std::max(0.0, left)), LWA_ALPHA);
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(wnd, kToastTimer);
        if (g_toast == wnd) g_toast = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

bool EnsureClass()
{
    if (g_class) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ToastProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kToastClass;
    g_class = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return g_class;
}

}  // namespace

void InitReaperToast(HINSTANCE instance, HWND main)
{
    g_instance = instance;
    g_main = main;
}

void QuitReaperToast()
{
    try {
        if (g_toast) DestroyWindow(g_toast);
        g_toast = nullptr;
        if (g_font) DeleteObject(g_font);
        g_font = nullptr;
        if (g_class) UnregisterClassW(kToastClass, g_instance);
        g_class = false;
    } catch (...) {
    }
}

bool ShowReaperToast(const std::string& text)
{
    try {
        if (!g_main || !IsWindow(g_main) || IsIconic(g_main) || text.empty() || !EnsureClass()) return false;
        if (g_toast) DestroyWindow(g_toast);  // the new one replaces it
        g_text = Widen(text);
        const float scale = ToastScale(g_main);
        g_pad_x = static_cast<int>(14 * scale + 0.5f);
        g_pad_y = static_cast<int>(8 * scale + 0.5f);
        if (g_font) DeleteObject(g_font);
        g_font = CreateFontW(-static_cast<int>(13 * scale + 0.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                             L"Segoe UI");
        // Its size: the text wrapped at ~460 px.
        RECT tr = {0, 0, static_cast<int>(460 * scale), 0};
        HDC  dc = GetDC(g_main);
        if (dc) {
            HGDIOBJ old = g_font ? SelectObject(dc, g_font) : nullptr;
            DrawTextW(dc, g_text.c_str(), static_cast<int>(g_text.size()), &tr,
                      DT_CALCRECT | DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            if (old) SelectObject(dc, old);
            ReleaseDC(g_main, dc);
        }
        const int w = (tr.right - tr.left) + 2 * g_pad_x;
        const int h = (tr.bottom - tr.top) + 2 * g_pad_y;
        // Bottom right of the main window's client area.
        RECT cr;
        GetClientRect(g_main, &cr);
        POINT br = {cr.right, cr.bottom};
        ClientToScreen(g_main, &br);
        const int margin = static_cast<int>(16 * scale + 0.5f);
        const int x = br.x - margin - w, y = br.y - margin - h;
        HWND wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                   kToastClass, L"", WS_POPUP, x, y, w, h, g_main, nullptr, g_instance, nullptr);
        if (!wnd) return false;
        const int r = static_cast<int>(7 * scale + 0.5f) * 2;
        if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r)) SetWindowRgn(wnd, rgn, FALSE);  // owned by the window
        SetLayeredWindowAttributes(wnd, 0, kAlpha, LWA_ALPHA);
        g_toast = wnd;
        g_shown_at = GetTickCount64();
        g_hold_ms = static_cast<DWORD>(std::min<size_t>(9000, 3500 + 35 * g_text.size()));
        SetTimer(wnd, kToastTimer, kTickMs, nullptr);
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
        UpdateWindow(wnd);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace rav

#else  // host builds: no toast

namespace rav {

void QuitReaperToast() {}
bool ShowReaperToast(const std::string&) { return false; }

}  // namespace rav

#endif
