// SPDX-License-Identifier: MIT
//
// REAPER's own windows opened from RAV (Epic 11, Story 11-5): the Video panel's
// "Matrix" and "Render" buttons (one row pinned at the top of the panel). They run REAPER's actions:
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

// Spec 11-fb-5 -- whether REAPER's Video window is open: the toggle state of
//   50125  Video: Show/hide video window
// resolved like the actions above (by that exact English name, case-insensitive, when REAPER
// does not know the id). *known = false when no such toggle action was found, or before
// InitReaperActionLookup (retried on a later call): the caller falls back on something else
// and the return value is false. Cheap: called every frame.
bool ReaperVideoWindowOpen(bool* known);

}  // namespace rav

#endif  // _WIN32
