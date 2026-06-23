// SPDX-License-Identifier: MIT
#pragma once

#include "reaper_api.h"

namespace rav {

// Opens (or brings to front) the viewer window. Idempotent: a second call
// while the window is already open just raises the existing one.
void OpenViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main);

// Destroys the window and releases the GL context. Safe to call when the
// window is not open.
void CloseViewerWindow();

}  // namespace rav
