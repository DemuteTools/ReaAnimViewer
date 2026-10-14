// SPDX-License-Identifier: MIT
//
// See viewer_fullscreen.h. The order of each step follows SetParent's documentation: to make a
// child top-level, SetParent(NULL) first, then WS_CHILD off / WS_POPUP on; to make it a child
// again, the styles first, then SetParent. SWP_FRAMECHANGED makes Windows take the new styles in.

#include "viewer_fullscreen.h"

#ifdef _WIN32

#include <shellapi.h>  // SHAppBarMessage: the auto-hide taskbar

#include <algorithm>
#include <string>

#include "console_log.h"
#include "float_ui_window.h"
#include "reaper_api.h"
#include "ui_theme.h"
#include "viewer_fullscreen_plan.h"

namespace rav {
namespace {

bool     g_active = false;
LONG_PTR g_docked_ex = 0;  // the extended style it had in the docker, given back on leave

void ToPlan(const RECT& r, fullscreen::ScreenRect* out)
{
    out->left = r.left;
    out->top = r.top;
    out->right = r.right;
    out->bottom = r.bottom;
}

// True when an auto-hide app bar (the taskbar set to hide itself) sits on `edge` (ABE_*) of the
// monitor whose rect is `mon`.
bool AutoHideBarOn(UINT edge, const RECT& mon)
{
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.uEdge = edge;
    abd.rc = mon;
    return reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &abd)) != nullptr;
}

// Story 6-5-10 -- where the full screen window goes on `mon`: its work area (the taskbar stays
// visible wherever it is docked), off the edges an auto-hide taskbar slides out of.
bool MonitorRect(HMONITOR mon, fullscreen::Placement* out)
{
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    fullscreen::ScreenRect m, w;
    ToPlan(mi.rcMonitor, &m);
    ToPlan(mi.rcWork, &w);
    fullscreen::AutoHideEdges ah;
    ah.left = AutoHideBarOn(ABE_LEFT, mi.rcMonitor);
    ah.top = AutoHideBarOn(ABE_TOP, mi.rcMonitor);
    ah.right = AutoHideBarOn(ABE_RIGHT, mi.rcMonitor);
    ah.bottom = AutoHideBarOn(ABE_BOTTOM, mi.rcMonitor);
    *out = fullscreen::CoverWorkArea(m, w, ah);
    return true;
}

// Back in the docker: the child of REAPER's main window it was created as, handed to the docker.
void Redock(HWND hwnd, HWND reaper_main, const char* dock_name, const char* dock_ident)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      static_cast<LONG_PTR>(fullscreen::DockedStyle(static_cast<std::uint32_t>(style))));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_docked_ex);
    SetParent(hwnd, reaper_main);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    DockWindowAddEx(hwnd, dock_name, dock_ident, true);
    DockWindowActivate(hwnd);
}

// 16 x 16 mock-up box, the Library glyphs' stroke (1.2 px at 1x). which 0: corners out (enter),
// 1: corners in (leave).
void DrawFullScreenGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col)
{
    const float k = s / 16.0f;
    const float w = std::max(1.0f, 1.2f * k);
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    auto corner = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        dl->PathLineTo(P(ax, ay));
        dl->PathLineTo(P(bx, by));
        dl->PathLineTo(P(cx, cy));
        dl->PathStroke(col, ImDrawFlags_None, w);
    };
    if (which == 0) {
        corner(2.5f, 6.0f, 2.5f, 2.5f, 6.0f, 2.5f);
        corner(10.0f, 2.5f, 13.5f, 2.5f, 13.5f, 6.0f);
        corner(13.5f, 10.0f, 13.5f, 13.5f, 10.0f, 13.5f);
        corner(6.0f, 13.5f, 2.5f, 13.5f, 2.5f, 10.0f);
    } else {
        corner(6.0f, 2.5f, 6.0f, 6.0f, 2.5f, 6.0f);
        corner(10.0f, 2.5f, 10.0f, 6.0f, 13.5f, 6.0f);
        corner(13.5f, 10.0f, 10.0f, 10.0f, 10.0f, 13.5f);
        corner(2.5f, 10.0f, 6.0f, 10.0f, 6.0f, 13.5f);
    }
}

}  // namespace

bool FullScreenActive()
{
    return g_active;
}

bool EnterFullScreen(HWND hwnd, HWND reaper_main, const char* dock_name, const char* dock_ident)
{
    if (g_active || !hwnd || !IsWindow(hwnd)) return false;
    // The monitor the viewer shows on; a hidden one (closed docker tab) takes REAPER's.
    HMONITOR mon = MonitorFromWindow(IsWindowVisible(hwnd) ? hwnd : (reaper_main ? reaper_main : hwnd),
                                     MONITOR_DEFAULTTONEAREST);
    fullscreen::Placement at;
    if (!MonitorRect(mon, &at)) {
        LogWarn("full screen: the monitor could not be read; the viewer stays in its docker");
        return false;
    }
    g_docked_ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // Made top-level FIRST, hidden while its styles change (no flash of a framed child), and only
    // then taken out of the docker: if REAPER closes a floating docker frame that RAV leaves empty,
    // the frame's destruction must not take our window with it (it is no longer its child).
    ShowWindow(hwnd, SW_HIDE);
    SetLastError(0);
    if (!SetParent(hwnd, nullptr) && GetLastError() != 0) {
        LogWarn("full screen: the window could not leave the docker (error %lu); it stays docked",
                static_cast<unsigned long>(GetLastError()));
        DockWindowActivate(hwnd);  // still in its docker: shown again there
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      static_cast<LONG_PTR>(fullscreen::FullScreenStyle(static_cast<std::uint32_t>(style))));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      static_cast<LONG_PTR>(fullscreen::FullScreenExStyle(static_cast<std::uint32_t>(g_docked_ex))));
    DockWindowRemove(hwnd);  // out of the docker's list (as on a close)
    if (!IsWindow(hwnd)) {   // defensive: destroyed meanwhile (WM_DESTROY has cleared the viewer)
        LogWarn("full screen: the viewer was closed while it left the docker");
        return false;
    }
    // Should the docker have taken it back as its child or shown it, top-level and hidden again.
    if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) {
        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr);
        const LONG_PTR again = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          static_cast<LONG_PTR>(fullscreen::FullScreenStyle(static_cast<std::uint32_t>(again))));
    }
    // Owned by REAPER's main window (once it is a popup, GWLP_HWNDPARENT is its owner): it stays in
    // front of REAPER, has no taskbar button, minimizes with it and goes when REAPER closes.
    if (reaper_main) SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(reaper_main));
    g_active = true;
    // The tool windows (Sound assignment) stay in front of it: owned by it while it lasts.
    FloatUiWindow::SetFullScreenOwner(hwnd);
    SetWindowPos(hwnd, HWND_TOP, at.x, at.y, at.w, at.h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    LogInfo("viewer full screen (%d x %d)", at.w, at.h);
    return true;
}

void LeaveFullScreen(HWND hwnd, HWND reaper_main, const char* dock_name, const char* dock_ident)
{
    if (!g_active) return;
    g_active = false;
    FloatUiWindow::SetFullScreenOwner(nullptr);  // before it stops being a top-level window
    if (!hwnd || !IsWindow(hwnd)) return;
    ShowWindow(hwnd, SW_HIDE);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);  // no owner: a child has a parent instead
    Redock(hwnd, reaper_main, dock_name, dock_ident);
    // The keys keep reaching the viewer (Esc / F11 came from it): the docker's frame (REAPER's main
    // window, or a floating docker) comes to the front with the focus on the viewer.
    if (HWND root = GetAncestor(hwnd, GA_ROOT)) SetForegroundWindow(root);
    SetFocus(hwnd);
    LogInfo("viewer back in its docker");
}

void RefitFullScreen(HWND hwnd)
{
    if (!g_active || !hwnd || !IsWindow(hwnd)) return;
    fullscreen::Placement at;
    if (!MonitorRect(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &at)) return;
    RECT now = {};
    if (GetWindowRect(hwnd, &now) && now.left == at.x && now.top == at.y && now.right - now.left == at.w &&
        now.bottom - now.top == at.h)
        return;  // already there (a settings broadcast that changed nothing of ours)
    SetWindowPos(hwnd, nullptr, at.x, at.y, at.w, at.h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ForgetFullScreen()
{
    if (!g_active) return;
    g_active = false;
    FloatUiWindow::SetFullScreenOwner(nullptr);
}

bool FullScreenForwardToReaperModal(HWND hwnd, HWND reaper_main)
{
    if (!g_active || !reaper_main || IsWindowEnabled(reaper_main)) return false;
    HWND modal = GetLastActivePopup(reaper_main);
    if (!modal || modal == reaper_main || modal == hwnd || !IsWindowVisible(modal)) return false;
    SetForegroundWindow(modal);
    return true;
}

bool DrawFullScreenButton(float right_x, float centre_y, const char* key)
{
    constexpr float kIconSize = 18.0f, kIconPad = 3.0f;  // the keyboard icon's 24 px button
    const float btn = ui::Dp(kIconSize + 2.0f * kIconPad);
    bool        pressed = false;
    if (ui::BeginOverlay("##fullscreenbtn", ImVec2(right_x, centre_y - btn * 0.5f), ImVec2(1.0f, 0.0f))) {
        ui::IconOnlyOpts o;
        o.w = btn;
        o.icon_px = ui::Dp(16.0f);
        const bool        has_key = key && key[0];
        const std::string tip = g_active ? std::string("Leave full screen (Esc") + (has_key ? std::string(" or ") + key : "") + ")"
                                         : std::string("Full screen") + (has_key ? std::string(" (") + key + ")" : "");
        pressed = ui::IconOnlyButton("##fullscreen", ui::Icon{&DrawFullScreenGlyph, g_active ? 1 : 0, ui::kMuted},
                                     tip.c_str(), o);
    }
    ui::EndOverlay();
    return pressed;
}

}  // namespace rav

#endif  // _WIN32
