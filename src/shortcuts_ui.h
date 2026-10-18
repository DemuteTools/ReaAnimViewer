// SPDX-License-Identifier: MIT
//
// The keyboard icon, the Shortcuts popup and its record modal (spec 11-fb-3). Dear ImGui,
// called between NewFrame and Render by the viewer (src/viewer_window.cpp). 13-64: the popup is a
// large panel over the whole viewer (a modal: dimmed backdrop; Esc, its close button or a click on the
// backdrop closes it), with Find by key and the Link modal's search by name / ID / shortcut.

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

// 13-73: opens the popup on its next frame (a "Shortcuts" button elsewhere), its name search set to `search` (the rows
// it should show; null / "" = all).
void RequestOpenShortcutsPopup(const char* search);

// 13-64: Find by key (or the Link modal's key button) waits for a key or a wheel: while armed, the
// accelerator hook hands the viewer every key and wheel (as while a key is recorded), and the window
// procedure passes them here. Esc disarms. A modifier alone is not a gesture: still waiting.
bool ShortcutsKeyCaptureArmed();
void ShortcutsCaptureKey(unsigned vk, bool ctrl, bool shift, bool alt);
void ShortcutsCaptureWheel(bool horizontal, bool ctrl, bool shift, bool alt);
// Focus lost or panel parked: disarmed.
void CancelShortcutsKeyCapture();

}  // namespace rav

#endif  // _WIN32
