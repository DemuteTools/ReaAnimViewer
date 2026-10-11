// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, D8) -- the Library view: one full-area window under the view toggle, four
// columns (Libraries and Switch Groups | the library tree | the selected node | the Media
// Explorer). 13-2 draws the empty shell and calls each column's sections; 13-7a owns this file
// after 13-2 (splitters, collapse).

#pragma once

#ifdef _WIN32

namespace rav {

// The view's area in window pixels (D8: 8, 44, w - 16, h - 52).
void DrawLibraryView(float x, float y, float w, float h);

}  // namespace rav

#endif  // _WIN32
