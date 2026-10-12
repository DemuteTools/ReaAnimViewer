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
#include "sound_assign.h"
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
// Sets the library of rule `block` of the item (and its pool; "" = inherit, kLibNoneWord = None, else a
// library uid), then re-places that rule's populated events: one undo point "RAV: Set rule library".
SoundReport SetRuleLibrary(MediaItem* item, int block, std::string library_uid);
enum class SoundValueLevel { Item, Rule, Event };
// Sets a Switch value at one level (Rule / Event: rule `block`, Event: clip time `t`; "" = inherit),
// pool-wide, then re-places the populated events it reaches: one undo point "RAV: Set sound value".
SoundReport SetSoundValue(MediaItem* item, SoundValueLevel level, int block, double t, std::string group_uid,
                          std::string value_uid);
// The library for these auto marker keys (sound_auto_assign.h) on the item's rules of those keys
// (pool-wide; "" = inherit, kLibNoneWord = None), their populated events re-placed: one undo point
// "RAV: Set auto marker library". Writes no store entry (13-15a: Project / Character own defaults).
SoundReport SetAutoMarkerLibrary(MediaItem* item, std::vector<std::string> keys, std::string library_uid);
// The same for one rule-level Switch value ("" = inherit): one undo point "RAV: Set sound value".
SoundReport SetAutoMarkerValue(MediaItem* item, std::vector<std::string> keys, std::string group_uid,
                               std::string value_uid);
// 13-8 fix: these take their uids by value: a caller's reference into the sound store
// would dangle once the gesture replaces the store (ModifySoundStore).

// ---- 13-15a: layered assignment (sound_assign.h) -------------------------------------------------
//
// Each gesture is one undo point; it edits the store, then re-places the populated events of every
// RAV item whose effective library or values changed (ChangedRids). Nothing happens (no undo point)
// when nothing changes. Keys are body parts / part.types ("feet", "feet.step"). A character is given
// as a CharacterRef: its uid when stored, else its rig (skeleton + model file + default name), which is
// stored at the first edit.
SoundReport SetProjectAssignLibrary(std::vector<std::string> keys, LibState state, std::string library_uid);
SoundReport SetProjectAssignValue(std::vector<std::string> keys, std::string group_uid, std::string value_uid);
SoundReport SetCharacterAssignLibrary(CharacterRef character, std::vector<std::string> keys, LibState state,
                                      std::string library_uid);
SoundReport SetCharacterAssignValue(CharacterRef character, std::vector<std::string> keys, std::string group_uid,
                                    std::string value_uid);
// Adopts the library's kit for `part` at the Project (an empty `character`) or the Character; a part already
// set at that level is left alone unless `replace`. "RAV: Use library kit".
SoundReport AdoptKit(std::string library_uid, std::string part, CharacterRef character, bool replace);
SoundReport RenameCharacter(std::string character_uid, std::string name);
// One character per model file (`files` empty = the character's own files hint).
SoundReport SplitCharacter(std::string character_uid, std::vector<std::string> files);
// The body parts a library suggests itself for; `offered` -1 leaves kit_offered, 0 / 1 sets it.
SoundReport SetLibraryKit(std::string library_uid, std::vector<std::string> parts, int offered = -1);

// The rig of an item: its skeleton signature (SkeletonKey, cached per animation file for the session;
// "" when the file does not load), its model file name, its bone count.
struct ItemRigInfo {
    std::string skeleton;
    std::string model_file;
    int         bones = 0;
};
ItemRigInfo ItemRig(MediaItem* item);
// The rigs present in the project's RAV items, with how many items play each model file.
struct RigFileCount {
    std::string file;
    int         items = 0;
};
struct ProjectRig {
    std::string               skeleton;
    int                       bones = 0;
    int                       items = 0;
    std::vector<RigFileCount> files;
};
std::vector<ProjectRig> ProjectRigs();

// The events Populate would place on these items (their targets): events whose rule resolves to a
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
