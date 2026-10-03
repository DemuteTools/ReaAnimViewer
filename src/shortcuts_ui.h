// SPDX-License-Identifier: MIT
//
// The keyboard icon, the Shortcuts popup and its record modal (spec 11-fb-3). Dear ImGui,
// called between NewFrame and Render by the viewer (src/viewer_window.cpp).

#pragma once

#ifdef _WIN32

#include <imgui.h>

namespace rav {

// The keyboard icon, its right edge at right_x and its centre at centre_y; a click
// toggles the Shortcuts popup (drawn here too). `icon` 0 = drawn glyph.
void DrawShortcutsButton(float right_x, float centre_y, ImTextureID icon);

// The popup is open (the accelerator hook then hands Esc to the viewer).
bool ShortcutsPopupOpen();

// Esc pressed while the popup is open (not recording): it closes on the next frame.
void RequestCloseShortcutsPopup();

}  // namespace rav

#endif  // _WIN32
