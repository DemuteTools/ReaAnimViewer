// SPDX-License-Identifier: MIT
//
// The Attach manager (13-68e v5; mockup ux-designs/epic-13-sound-library/maquette-attach-manager-13-68e-v5.html): a
// small window of its own (FloatUiWindow), opened at the mouse by "RAV: Attach manager (at mouse)", outside RAV's
// docked panel. It stays open and follows REAPER's time selection, item selection and undo; Esc or its close button
// closes it; its title row moves it.
//   Apply to: Item / Anim / Project, the project's setting (also what the gestures made in REAPER reach: the
//   watcher, sound_spread_selection.h AttachWatchTick).
//   Marker (every marker kind and side of the time selection, each at its first marker, and the clicked RAV marker
//   label) and the animation it is on.
//   Selection: Attach / Add to pile (chosen from the item selection, a two-button switch), what it does, why it is
//   off, and its run button: one undo point.
//   L and R: whether the marker's kind plays one sound on both sides or two groups, Separate / Merge L and R.
// What it shows is attach_manager_plan.h's (pure, host-tested). Its gestures run from the viewer's window procedure
// (AttachManagerRunPending), never inside its frame. Main thread only.

#pragma once

#ifdef _WIN32

#include <windows.h>

namespace rav {

// Opens the window (or brings it forward) at the mouse.
void OpenAttachManager();
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
