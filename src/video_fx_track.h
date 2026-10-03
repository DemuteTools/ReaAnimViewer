// SPDX-License-Identifier: MIT
//
// The RAV video FX on REAPER tracks (Epic 11): find it, add it. Main thread only.

#pragma once

#include "reaper_api.h"

namespace rav {

// Index of the RAV video FX in the track's FX chain (the first one), -1 if none.
// Matched by CLAP id (fx_ident), else by its original name; another plug-in whose name
// merely starts with "RAV Video FX" (e.g. the 11-0 spike) never matches.
int FindVideoFxOnTrack(MediaTrack* track);

// Main thread. Makes REAPER re-ask every RAV video FX of the project for its frames
// (bumps the FX's refresh parameter): call it when the picture changes in a way REAPER
// cannot see (display settings, output size, background). No undo point of its own.
void RefreshVideoFxPictures();

enum class AddVideoFxResult { Added, AlreadyThere, NotInstalled };

// Adds the RAV video FX at the end of the track's chain unless it is already there.
// Opens no window. `out_index` (optional) gets its index, or -1.
AddVideoFxResult AddVideoFxToTrack(MediaTrack* track, int* out_index);

// Body of the action "RAV: Add video FX to selected track": every selected track,
// one undo point, a message box when no track is selected or the FX is not installed.
void AddVideoFxToSelectedTracks();

// The message box shown when REAPER does not know the plug-in (Story 11-4 reuses it for
// the Video panel's "Add video FX to track"). Modal: never call it inside a render tick.
void ShowVideoFxNotInstalledMessage();

}  // namespace rav
