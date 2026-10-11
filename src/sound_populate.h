// SPDX-License-Identifier: MIT
//
// Populate (13-8, REAPER): places each event's sound for the given animation items, one undo point.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-8
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
// The decisions are sound_populate_plan.h's (pure, host-tested).

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

// Populates the items (the selected animation items): one undo point "RAV: Populate sounds".
SoundReport PopulateItems(const std::vector<MediaItem*>& items);

// ---- 13-8 additions ---------------------------------------------------------------------------

// Resolves this module's extra REAPER functions (D7).
void InitSoundPopulate(void* (*get_func)(const char* name));

// PopulateItems, with the number of events it handled (`events`, optional).
SoundReport PopulateItemsCounted(const std::vector<MediaItem*>& items, int* events);
// The items Populate acts on: the RAV items of `items` plus every other member of their pools.
std::vector<MediaItem*> PopulateTargets(const std::vector<MediaItem*>& items);
// The selected items that are RAV animation items.
std::vector<MediaItem*> SelectedRavItems();
// A new seed for the targets, then the populated events re-placed: one undo point
// "RAV: Reshuffle sounds" (no Replaced lines).
SoundReport ReshuffleItems(const std::vector<MediaItem*>& items);
// Sets the library of rule `block` of the item (and its pool; "" = None), then re-places that
// rule's populated events: one undo point "RAV: Set rule library".
SoundReport SetRuleLibrary(MediaItem* item, int block, const std::string& library_uid);
enum class SoundValueLevel { Item, Rule, Event };
// Sets a Switch value at one level (Rule / Event: rule `block`, Event: clip time `t`; "" = inherit),
// pool-wide, then re-places the populated events it reaches: one undo point "RAV: Set sound value".
SoundReport SetSoundValue(MediaItem* item, SoundValueLevel level, int block, double t, const std::string& group_uid,
                          const std::string& value_uid);
// The events Populate would place on these items (their targets): events whose rule has a
// library that is not blocked.
int CountPopulateEvents(const std::vector<MediaItem*>& items);
// The item of the current project with this GUID (nullptr when none).
MediaItem* FindItemByGuid(const std::string& guid);
// The item's GUID ("" when none).
std::string ItemGuidOf(MediaItem* item);
// Selects this item alone in REAPER.
void SelectOnlyItem(MediaItem* item);

}  // namespace rav

#endif  // _WIN32
