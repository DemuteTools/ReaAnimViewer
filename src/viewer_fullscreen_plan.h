// SPDX-License-Identifier: MIT
//
// Story 6-5-9 -- the viewer's full screen, its pure half (no REAPER, no Windows): the window
// style bits of each mode, where the full screen window goes, and when Esc leaves it. The
// Win32 half is src/viewer_fullscreen.cpp; the host test is tests/viewer_fullscreen_plan_test.cpp.

#pragma once

#include <cstdint>

namespace rav {
namespace fullscreen {

// The Windows style bits (WS_*, WS_EX_*), spelled here so this half stays Windows-free.
constexpr std::uint32_t kWsPopup = 0x80000000u, kWsChild = 0x40000000u, kWsVisible = 0x10000000u,
                        kWsCaption = 0x00C00000u, kWsSysMenu = 0x00080000u, kWsThickFrame = 0x00040000u,
                        kWsMinimizeBox = 0x00020000u, kWsMaximizeBox = 0x00010000u;
constexpr std::uint32_t kWsExDlgModalFrame = 0x00000001u, kWsExWindowEdge = 0x00000100u,
                        kWsExClientEdge = 0x00000200u, kWsExStaticEdge = 0x00020000u;

// Every frame / edge bit: the full screen window has none (borderless), and the docked child
// had none either.
constexpr std::uint32_t kFrameBits = kWsCaption | kWsSysMenu | kWsThickFrame | kWsMinimizeBox | kWsMaximizeBox;
constexpr std::uint32_t kEdgeExBits = kWsExDlgModalFrame | kWsExWindowEdge | kWsExClientEdge | kWsExStaticEdge;

// The style of the full screen window: a borderless top-level popup, hidden until it is placed
// (the rest of the style, WS_CLIPCHILDREN / WS_CLIPSIBLINGS, is kept).
inline std::uint32_t FullScreenStyle(std::uint32_t docked)
{
    return (docked & ~(kWsChild | kWsVisible | kFrameBits)) | kWsPopup;
}

inline std::uint32_t FullScreenExStyle(std::uint32_t docked_ex)
{
    return docked_ex & ~kEdgeExBits;
}

// The style it goes back to the docker with: the child it was created as (hidden: the docker
// shows it, as on the first open).
inline std::uint32_t DockedStyle(std::uint32_t full)
{
    return (full & ~(kWsPopup | kWsVisible | kFrameBits)) | kWsChild;
}

struct ScreenRect {
    long left = 0, top = 0, right = 0, bottom = 0;
};

struct Placement {
    int x = 0, y = 0, w = 1, h = 1;
};

// The whole monitor, at least 1 x 1.
inline Placement CoverMonitor(const ScreenRect& monitor)
{
    Placement p;
    p.x = static_cast<int>(monitor.left);
    p.y = static_cast<int>(monitor.top);
    p.w = monitor.right > monitor.left ? static_cast<int>(monitor.right - monitor.left) : 1;
    p.h = monitor.bottom > monitor.top ? static_cast<int>(monitor.bottom - monitor.top) : 1;
    return p;
}

// The monitor edges an auto-hide taskbar (or another auto-hide app bar) sits on.
struct AutoHideEdges {
    bool left = false, top = false, right = false, bottom = false;
};

// How far the full screen window stays off an auto-hide bar's edge: the strip the mouse touches
// to make the bar slide out (a window covering the whole monitor would be taken by Windows as a
// full screen game and keep the bar hidden).
constexpr long kAutoHideGap = 2;

// Story 6-5-10 -- the full screen window never covers the taskbar, wherever it is: it covers the
// monitor's work area (the monitor minus the docked taskbar and app bars), and stays kAutoHideGap
// pixels off each edge an auto-hide bar sits on (the work area does not leave it out). A work area
// outside the monitor or empty (a bad reading) falls back to the whole monitor.
inline Placement CoverWorkArea(const ScreenRect& monitor, const ScreenRect& work, const AutoHideEdges& autohide)
{
    ScreenRect r = work;
    const bool usable = r.right > r.left && r.bottom > r.top && r.left >= monitor.left && r.top >= monitor.top &&
                        r.right <= monitor.right && r.bottom <= monitor.bottom;
    if (!usable) r = monitor;
    // Only where the work area reaches the monitor's edge (a docked bar already leaves that edge free).
    if (autohide.left && r.left == monitor.left) r.left += kAutoHideGap;
    if (autohide.top && r.top == monitor.top) r.top += kAutoHideGap;
    if (autohide.right && r.right == monitor.right) r.right -= kAutoHideGap;
    if (autohide.bottom && r.bottom == monitor.bottom) r.bottom -= kAutoHideGap;
    return CoverMonitor(r);
}

// What else wants an Esc pressed in the viewer. Each of them keeps it, as before full screen.
// (The Sound assignment window having the focus is not here: it takes every viewer key before
// this rule is asked, viewer_window.cpp WM_KEYDOWN.)
struct EscState {
    bool full_screen = false;
    bool shortcuts_popup = false;   // the Shortcuts popup closes
    bool delete_confirm = false;    // the delete confirmation cancels
    bool tagging_gesture = false;   // a Tagging view drag cancels
    bool text_input = false;        // a text field (or the preset menu) takes every key
    bool recording = false;         // a new key is being recorded (Esc cancels it)
    bool library_takes_esc = false; // the Library view uses Esc itself
    bool imgui_popup = false;       // an ImGui menu / combo / popup was open when Esc went down: it
                                    // closes that popup only (one Esc, one thing)
};

// True when this Esc leaves full screen.
inline bool EscLeavesFullScreen(const EscState& s)
{
    if (!s.full_screen) return false;
    return !(s.shortcuts_popup || s.delete_confirm || s.tagging_gesture || s.text_input || s.recording ||
             s.library_takes_esc || s.imgui_popup);
}

}  // namespace fullscreen
}  // namespace rav
