// SPDX-License-Identifier: MIT
//
// Follow (13-9, REAPER): placed sounds follow their animation item, from the marker mirror's tick
// (tag_markers.cpp MarkerMirrorTick).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-9
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include "reaper_api.h"

namespace rav {

void InitSoundFollow(void* (*get_func)(const char* name));
// Called by the 10-4b mirror for each project whose state changed. Never throws.
void SoundFollowTick(ReaProject* proj);
// The menu's Sounds section (Tagging tab, after "Markers written").
void DrawSoundsMenuSection();

}  // namespace rav

#endif  // _WIN32
