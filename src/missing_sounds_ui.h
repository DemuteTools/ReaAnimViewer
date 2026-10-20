// SPDX-License-Identifier: MIT
//
// The Missing sounds window (spec missing-sounds-window; mockup
// ux-designs/epic-13-sound-library/maquette-missing-sounds-v2.html): one place to fix every missing sound of every
// library. A small window of its own (FloatUiWindow), opened by a library's red missing count or its menu's
// "Fix missing sounds...", that library's rows first (an accent bar), every other library's below.
//   Header: "Missing sounds · N to fix" (or "all fixed") and its close button; Esc closes it too.
//   Search a folder... (every missing sound of every library, the folder and its subfolders), "Last searched:".
//   A table: Asset name + its status line | Library | the row's buttons in a fixed right column (Locate...,
//   Use selected item; a sound found by its name only: Listen and Locate...). A fixed row stays (muted name, green
//   "Fixed · ...") until the window closes. A name clicked shows that sound in the Library's tree.
// Every repair is one undo point (library_user_ui.h, library_node_ui.h); the window only reads and picks. Main
// thread only.

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>

namespace rav {

// Opens the window (or brings it forward) at the mouse, the library `focus_lib`'s rows first ("" = none).
void OpenMissingSounds(const std::string& focus_lib);
bool MissingSoundsIsOpen();
// The viewer, once per render tick after its own frame (its GL and ImGui contexts current): the window's frame in
// its own OS window and ImGui context, then its picked gestures. Restores both contexts.
void DrawMissingSoundsFloat(HWND viewer, HDC viewer_dc, HGLRC rc);
// The viewer's panel is hidden (no tick runs): the window hides with it.
void HideMissingSoundsFloat();
// The viewer closes (its GL and ImGui contexts current): the window and its context are freed.
void DestroyMissingSoundsFloat();
// The key routing (the viewer's accelerator hook).
bool MissingSoundsFloatOwns(HWND hwnd);
HWND MissingSoundsFloatHwnd();
bool MissingSoundsFloatWantsText();

}  // namespace rav

#endif  // _WIN32
