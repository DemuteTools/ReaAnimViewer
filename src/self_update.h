// SPDX-License-Identifier: MIT
//
// Self-update from the Demute Reaper Toolkit copy. The Toolkit can only install
// scripts, so it downloads reaper_animviewer.dll next to RAV_Launcher.lua in
// Scripts/ReaAnimViewer/Scripts/. When that copy is newer than the running build,
// the extension replaces its own file in UserPlugins:
//   - at REAPER quit (normal path: one restart after a Toolkit update, no message);
//   - at the first timer tick after startup, as a fallback when the quit swap did
//     not happen (crash), with one message asking to restart.
// Never runs for a "dev" build or for a DLL owned by ReaPack. Never throws (AR18).

#pragma once

#include "reaper_api.h"

namespace rav {

// Call from the plugin entry point once the Reaper API is loaded.
void SelfUpdateInit(REAPER_PLUGIN_HINSTANCE module,
                    void* (*get_func)(const char*),
                    int (*register_fn)(const char*, void*));

// Call first in the rec == nullptr unload path (REAPER is quitting).
void SelfUpdateOnQuit();

}  // namespace rav
