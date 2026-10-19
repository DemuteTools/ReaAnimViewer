// SPDX-License-Identifier: MIT
//
// The Attach manager (13-68e; mockup ux-designs/epic-13-sound-library/maquette-attach-manager-13-68e.html): a small
// window of its own (FloatUiWindow), opened at the mouse by "RAV: Attach manager - Attach (at mouse)" and
// "... - Detach (at mouse)" on their tab, outside RAV's docked panel. It stays open and follows REAPER's time
// selection, item selection and undo; Esc or its close button closes it; its title row moves it.
//   Attach: the Marker (every marker kind and side of the time selection, each at its first marker, and the clicked
//   RAV marker label), Attach / Add to stack (chosen from the item selection, a two-button switch), Scope (This
//   animation / All with <kind>) and Side (Both sides / <side> only), both remembered, and one run button: the 13-68
//   gesture (sound_spread_selection.h), one undo point.
//   Detach: a Library dropdown (All libraries, then each Attached library) and Detach This marker / Whole library;
//   the list (a selected animation, else the selected items, else the time selection, else everything) of the
//   markers holding attached / placed sounds, grouped by animation, or of the library's variations; select / detach
//   buttons in a fixed column, Select all / Detach all, each detach asking Keep in place / Delete / Cancel.
// What it shows is attach_manager_plan.h's (pure, host-tested). Its gestures run from the viewer's window procedure
// (AttachManagerRunPending), never inside its frame. Main thread only.

#pragma once

#ifdef _WIN32

#include <windows.h>

namespace rav {

enum class AttachManagerTab { Attach, Detach };

// Opens the window (or brings it forward) at the mouse, on `tab`.
void OpenAttachManager(AttachManagerTab tab);
bool AttachManagerIsOpen();
// The viewer, once per render tick after its own frame (its GL context current; also while its panel is hidden, so
// the window lives outside it): the window's frame in its own OS window and ImGui context. Restores both contexts.
void DrawAttachManagerFloat(HWND viewer, HDC viewer_dc, HGLRC rc);
// The viewer closes (its GL and ImGui contexts current): the window and its context are freed.
void DestroyAttachManagerFloat();
// The key routing (the viewer's accelerator hook): a message for the window, and whether it types text.
bool AttachManagerFloatOwns(HWND hwnd);
HWND AttachManagerFloatHwnd();
bool AttachManagerFloatWantsText();
// A gesture was clicked: the viewer posts itself a message and runs it from its window procedure.
bool AttachManagerHasPending();
void AttachManagerRunPending();
// The viewer closes: a queued gesture is dropped (nothing written).
void AttachManagerDropPending();

}  // namespace rav

#endif  // _WIN32
