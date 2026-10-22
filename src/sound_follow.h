// SPDX-License-Identifier: MIT
//
// Follow (13-9, REAPER): placed sounds follow their animation item, from the marker mirror's tick
// (tag_markers.cpp MarkerMirrorTick).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-9
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <initializer_list>
#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

void InitSoundFollow(void* (*get_func)(const char* name));
// Called by the 10-4b mirror for each project whose state changed. Never throws.
// Freeze fix #1: true when it planned nothing yet because detections are left to run (they are
// spread over ticks, ~30 ms each, at least one a tick): the mirror calls it again on its next tick.
bool SoundFollowTick(ReaProject* proj);
// The menu's Sounds section (Tagging tab, after "Markers written").
void DrawSoundsMenuSection();
// Story 10-10f (moved from that section to the Tagging footer): the selected animation items that have a sound
// record, and (placed_out) how many sounds Populate placed on them.
std::vector<MediaItem*> RepopulateItems(ReaProject* proj, int* placed_out);
// Repopulate: removes those items' populated sounds (even moved ones), forgets the ones deleted by hand, then
// places and draws again; attached and spread sounds stay. One undo point. Nothing without such an item.
void Repopulate(ReaProject* proj);
// Story 13-17 (N6): the menu's Library tab, its Tracks section: where placed sounds land (the
// Tracks option, moved from Tagging > Sounds) and the separator of joined track names.
void DrawLibraryTracksSection();

// 13-68e v5 (the Attach manager's watcher): REAPER's localized forms of these undo names (those that differ from the
// English; empty without a language pack), and the follow's cut names.
std::vector<std::string>        LocalizedUndoNames(std::initializer_list<const char*> names);
const std::vector<std::string>& FollowCutNames();

// The follow report of a project (13-9 F12): session memory, the last 200 lines, oldest first.
std::vector<ReportLine> FollowReport(ReaProject* proj);
void                    ClearFollowReport(ReaProject* proj);

}  // namespace rav

#endif  // _WIN32
