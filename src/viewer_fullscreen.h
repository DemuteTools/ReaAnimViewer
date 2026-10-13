// SPDX-License-Identifier: MIT
//
// Story 6-5-9 -- the viewer's full screen, its Win32 half. The SAME window (HWND, CS_OWNDC DC,
// pixel format, WGL context, ImGui contexts) leaves REAPER's docker (DockWindowRemove), becomes a
// borderless top-level popup owned by REAPER's main window over the whole monitor it was on, then
// goes back to the docker as a child of REAPER's main window (DockWindowAddEx with the same ident:
// the docker REAPER remembers, the path every reopen takes). It is never destroyed to change mode
// (a docked window is never destroyed on a hide either: see viewer_window.cpp's header). REAPER
// itself reparents this same HWND on every undock / redock, so a reparent keeps the GL context.
// Main thread only, never inside the render tick (the viewer posts a message from a click).

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <imgui.h>

namespace rav {

// True while the viewer is out of the docker, full screen.
bool FullScreenActive();

// Takes `hwnd` (the docked viewer, or a hidden one) out of the docker and over the monitor it is on
// (REAPER's main window's monitor when it is hidden), with the keyboard focus. False when nothing
// changed (already full screen, no window) or it went back to the docker after a failure
// (`dock_name` / `dock_ident`: the strings of its first open).
bool EnterFullScreen(HWND hwnd, HWND reaper_main, const char* dock_name, const char* dock_ident);

// Gives `hwnd` back to the docker (`dock_name` / `dock_ident`: the strings of its first open), with
// the keyboard focus. Nothing when it is not full screen.
void LeaveFullScreen(HWND hwnd, HWND reaper_main, const char* dock_name, const char* dock_ident);

// Fits the full screen window to its monitor again (a display or DPI change). Nothing when docked.
void RefitFullScreen(HWND hwnd);

// The window is closing while full screen (the viewer's close, or REAPER destroying its owner):
// the tool windows go back to REAPER's main window and the state is cleared. No docker call.
void ForgetFullScreen();

// A click on the full screen window while REAPER shows a modal window (its main window is then
// disabled): that window comes to the front instead. True when it did (eat the click).
bool FullScreenForwardToReaperModal(HWND hwnd, HWND reaper_main);

// The Full screen button (a drawn glyph: four corners out to enter, in to leave), its right edge at
// right_x and its centre at centre_y; `key` is the shortcut's label for the tooltip. True on a click.
bool DrawFullScreenButton(float right_x, float centre_y, const char* key);

}  // namespace rav

#endif  // _WIN32
