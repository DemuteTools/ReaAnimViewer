// SPDX-License-Identifier: MIT
//
// The "Sync with global" window (story 13-77c, redesign v5; mockup ux-designs/epic-13-sound-library/maquette-sync-fork-v5.html):
// a Fork-style diff. One 24 px row per change under collapsible container groups; columns CHANGE | GLOBAL value | state
// glyph | THIS PROJECT value, a box on each side: tick the version you keep (the kept side tinted, the replaced one struck
// through). A change made on one side only is pre-ticked on that side; a conflict needs a pick; an addition has ONE box,
// on the side that added it. "Keep all global / mine" on the header and per group (tri-state) settle everything. The
// selection logic is library_sync_select.h (pure); this window only draws it and calls RequestSyncApply (one undo point).
// A window of its own (FloatUiWindow), one at a time. After Apply the applied rows disappear; it closes when none remain.
// Main thread only.

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>

namespace rav {

// Opens the window (or brings it forward) on the library `lib_uid`; `focus_node` (a container uid, "" = none) is
// unfolded and scrolled to. Opening for another library replaces the one shown.
void OpenSyncWithFile(const std::string& lib_uid, const std::string& focus_node = std::string());
bool SyncWithFileIsOpen();
// 13-77 feedback: the window is open on that library (the tree draws its change tags only then).
bool SyncWithFileIsOpenFor(const std::string& lib_uid);
// The viewer, once per render tick after its own frame (its GL and ImGui contexts current).
void DrawSyncWithFileFloat(HWND viewer, HDC viewer_dc, HGLRC rc);
// The viewer's panel is hidden: the window hides with it.
void HideSyncWithFileFloat();
// The viewer closes: the window and its context are freed.
void DestroySyncWithFileFloat();
// The key routing (the viewer's accelerator hook).
bool SyncWithFileFloatOwns(HWND hwnd);
HWND SyncWithFileFloatHwnd();
bool SyncWithFileFloatWantsText();

}  // namespace rav

#endif  // _WIN32
