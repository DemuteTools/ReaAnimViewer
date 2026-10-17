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

// Toggle entry point for the `RAV: Viewer - Open viewer` action: closes the panel if it
// is currently visible, otherwise opens (or re-shows) it.
void ToggleViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main);

// True when the panel exists and is visible — the on/off state Reaper queries
// for the toggle action's toolbar/menu checkmark.
bool ViewerWindowIsVisible();

// Story 6-5-9 -- the `RAV: Viewer - Full screen (toggle)` action: full screen leaves it (back to
// the docker); otherwise the viewer opens if needed and goes full screen (src/viewer_fullscreen.h).
void ToggleViewerFullScreen(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main);

// True while the viewer is full screen (the action's toolbar / menu checkmark).
bool ViewerIsFullScreen();

// The action's command id, so the viewer refreshes its toolbar button when the button, F11 or
// Esc change the mode (0 = none).
void SetViewerFullScreenCommandId(int command_id);

// Lot 13-60b -- a KEYS row's action run from REAPER (src/shortcuts.h ShortcutDef::action): what its
// key does in the viewer, when the viewer is open and the row's view is shown; otherwise nothing.
void RunViewerShortcut(int id);

// Story 11-4 — the keyboard hook that gives the focused viewer its own keys (V: RAV view /
// Video view, P: Video panel, and every key while a panel text field is active).
// plugin_main registers it ("accelerator") at load and unregisters it at unload with this
// same pointer.
accelerator_register_t* ViewerAcceleratorRegistration();

}  // namespace rav
