// SPDX-License-Identifier: MIT
//
// REAPER's own windows opened from RAV (Epic 11, Story 11-5): the Video panel's
// "Matrix", "Render" and "Render Current" buttons (one row at the bottom of Output). They run REAPER's actions:
//   41888  View: Show region render matrix window
//   40015  File: Render project to disk...
// Each id is used whenever REAPER knows it (kbd_getTextFromCmd gives it a name, English or
// translated by a LangPack); only an id REAPER does not know is looked up by its English
// name in the Main section (kbd_enumerateActions).
// Those lookup functions are resolved optionally through GetFunc, so a REAPER build
// without them still loads the extension (the known id is then used as is).
// The user picks source and format in REAPER. RAV writes the Output size into the video
// format (spec 11-fb-13), and changes the render bounds and source only for Render Current
// (spec 11-fb-16): set to the clip shown and the master mix while the Render dialog is open,
// then put back as they were.
// Main thread only.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

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

// ---- lot 13-60: any REAPER action, for a Shortcuts row linked to it -----------------------------
// Through the same optional lookups (plus NamedCommandLookup / ReverseNamedCommandLookup and the
// action shortcut functions).

struct ReaperActionInfo {
    std::string id;       // as reaper-kb.ini writes it: "990", "_WOL_ADJENVTRHMOUSE" ("" = not an id)
    std::string name;     // REAPER's name for it ("" = none)
    int         cmd = 0;  // its command number now (0 = REAPER does not know it)
    bool        checked = false;  // false: no lookup function to check it with
};
// A typed id ("990", "_WOL_ADJENVTRHMOUSE", "WOL_ADJENVTRHMOUSE", a named action's number). A named id
// can get a number and no name: REAPER keeps a number for an action a toolbar or menu names even when
// its extension or script is not loaded (cmd != 0, name "").
ReaperActionInfo LookupReaperAction(const std::string& text);
// InitReaperActionLookup has run (before it, nothing can be looked up).
bool ReaperActionLookupReady();

struct ReaperActionEntry {
    std::string id;
    std::string name;
};
// Every action of the Main section (kbd_enumerateActions), in REAPER's order; empty without it.
std::vector<ReaperActionEntry> ListReaperActions();

// The shortcuts REAPER lists for an action (keys, wheel, MIDI..., its built-in defaults too), as
// REAPER words them; *known = false when REAPER cannot say.
std::vector<std::string> ReaperActionShortcuts(int cmd, bool* known);

}  // namespace rav

#endif  // _WIN32
