// SPDX-License-Identifier: MIT
#pragma once

#include "reaper_api.h"

namespace rav {

// Opens (or, if hidden by a docker close, re-shows) the viewer panel. Idempotent:
// a call while the panel is already visible just brings its docker tab forward.
void OpenViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main);

// Destroys the window and releases the GL context. Safe to call when the
// window is not open.
void CloseViewerWindow();

// Toggle entry point for the `RAV: Open Viewer` action: closes the panel if it
// is currently visible, otherwise opens (or re-shows) it.
void ToggleViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main);

// True when the panel exists and is visible — the on/off state Reaper queries
// for the toggle action's toolbar/menu checkmark.
bool ViewerWindowIsVisible();

}  // namespace rav
