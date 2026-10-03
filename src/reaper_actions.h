// SPDX-License-Identifier: MIT
//
// REAPER's own windows opened from RAV (Epic 11, Story 11-5): the Video panel's
// "Region Render Matrix..." and "Render dialog..." buttons. They run REAPER's actions:
//   41888  View: Show region render matrix window
//   40015  File: Render project to disk...
// Each id is used whenever REAPER knows it (kbd_getTextFromCmd gives it a name, English or
// translated by a LangPack); only an id REAPER does not know is looked up by its English
// name in the Main section (kbd_enumerateActions).
// Those lookup functions are resolved optionally through GetFunc, so a REAPER build
// without them still loads the extension (the known id is then used as is).
// RAV never changes the render settings: the user picks source and format in REAPER.
// Main thread only.

#pragma once

#ifdef _WIN32

namespace rav {

// Called once at load with REAPER's GetFunc (resolves the optional lookup functions).
void InitReaperActionLookup(void* (*get_func)(const char* name));

// Shows REAPER's Region Render Matrix window (brought to the front when already open).
// False when no such action was found (the caller tells the user where it is).
bool OpenRegionRenderMatrix();

// Opens REAPER's Render to File dialog (modal: returns when the user closes it).
bool OpenRenderDialog();

}  // namespace rav

#endif  // _WIN32
