// SPDX-License-Identifier: MIT
//
// A toast on REAPER's main window (13-68, P9): a short report shown at the bottom right of the main
// window, also when no RAV window is open. A native layered popup owned by the main window (so above
// REAPER, never above other applications): it never takes the focus nor the mouse (clicks go through), fades out after a few seconds, in the
// house theme's colours (ui_theme.h kRaised / kStrokeStrong / kText). A new toast replaces the one
// showing. Main thread only. No-throw. Off Windows (host builds) it does nothing.

#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rav {

#ifdef _WIN32
// The module and REAPER's main window (plugin load).
void InitReaperToast(HINSTANCE instance, HWND main);
#endif
// Closes the toast showing and frees its window class (plugin unload).
void QuitReaperToast();
// Shows `text` (UTF-8, "\n" between lines). False when it could not be shown.
bool ShowReaperToast(const std::string& text);

}  // namespace rav
