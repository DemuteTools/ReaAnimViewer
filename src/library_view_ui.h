// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, D8) -- the Library view: one full-area window under the view toggle, four
// columns (Libraries and Switch Groups | the library tree | the selected node | the Media
// Explorer). 13-2 drew the empty shell; 13-7a owns this file: resizable columns (splitters,
// double-click resets), column 1 and the Media Explorer collapse to 28 px strips, widths and
// collapsed states kept as prefs, column 3 = the batch panel when several rows are selected,
// and the toast that offers Undo after each Library gesture.

#pragma once

#ifdef _WIN32

#include <string>

namespace rav {

// The view's area in window pixels (D8: 8, 44, w - 16, h - 52).
void DrawLibraryView(float x, float y, float w, float h);

// A short message at the bottom centre for ~5 s; `undo` adds an Undo button (REAPER's undo).
void ShowLibraryToast(const std::string& text, bool undo = true);

// Decision 7 (13-7a): whether a key down goes to the viewer while the Library view's window
// has the focus (Delete, F2, Space, Esc, Ctrl+B / D / X / V). Ctrl+Z stays REAPER's undo.
bool LibraryViewTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// True when the Library view's window had the keyboard focus last frame.
bool LibraryViewFocused();

// Column 1 collapsed to its strip (the tree header then shows a library switcher).
bool LibraryColumn1Collapsed();
void SetLibraryColumn1Collapsed(bool collapsed);

}  // namespace rav

#endif  // _WIN32
