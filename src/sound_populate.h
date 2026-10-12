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
// Antho 2026-10-12: what it did (placed / replaced / already in place / deleted by hand, not added back),
// also given as Info lines of the report.
struct PopulateCounts {
    int placed = 0, replaced = 0, removed = 0, kept = 0, deleted_kept = 0;  // sounds (a pile counts once)
};
// `revive_deleted` (the Populate button, Antho 2026-10-12): the sounds deleted by hand are placed again; the
// other callers (follow, copies) leave them deleted.
SoundReport PopulateItemsCounted(const std::vector<MediaItem*>& items, int* events, PopulateCounts* counts = nullptr,
                                 bool revive_deleted = false);
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
// ---- 13-15b: the assignment window's Item scope --------------------------------------------------
//
// Several rules of the item in ONE undo point, pool-wide, their populated events re-placed. `rids` are the
// rules' ids; `blocks` are the item's rules that have no id yet (block indices): they get one first.
// "RAV: Set rule library" / "RAV: Set sound value". Nothing happens (no undo point) when nothing changes
// (the rids are not checked: an empty list does nothing).
SoundReport SetRulesAssignLibrary(MediaItem* item, std::vector<std::string> rids, LibState state, std::string library_uid,
                                  std::vector<int> blocks = {}, bool* changed = nullptr);
SoundReport SetRulesAssignValue(MediaItem* item, std::vector<std::string> rids, std::string group_uid,
                                std::string value_uid, std::vector<int> blocks = {}, bool* changed = nullptr);
// (`changed`, when given: false when the edit changed nothing, so no undo point was made.)
// The RAV items of the project (the assignment window's item picker).
std::vector<MediaItem*> ProjectRavItems();

// 13-8 fix: these take their uids by value: a caller's reference into the sound store
// would dangle once the gesture replaces the store (ModifySoundStore).

// ---- 13-15a: layered assignment (sound_assign.h) -------------------------------------------------
//
// Each gesture is one undo point; it edits the store, then re-places the populated events of every
// RAV item whose effective library or values changed (ChangedRids). Nothing happens (no undo point)
// when nothing changes. Keys are body parts / part.types ("feet", "feet.step"). A character is given
// as a CharacterRef: its uid when stored, else its rig (skeleton + model + default name), which is
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
// One character per model the project's items of that rig play (ProjectRigs), each named from its
// first file and starting with the character's settings. "RAV: Split character".
SoundReport SplitCharacter(std::string character_uid);
// 13-15c: the same for a character that may have no record yet (a rig the project plays, stored at the edit).
SoundReport RenameCharacterRef(CharacterRef character, std::string name);
// Stores the rig's character if it has no record, then splits it per model, in one undo point "RAV: Split character".
SoundReport SplitRig(CharacterRef character);
// The Library view's kit offer for a library: `accept` adopts its kit parts that are free at the Project level
// (the ones set are left alone), and either way the kit counts as offered, in one undo point "RAV: Use library kit"
// (declined: "RAV: Set library kit").
SoundReport OfferKit(std::string library_uid, bool accept);
// The body parts a library suggests itself for; `offered` -1 leaves kit_offered, 0 / 1 sets it.
SoundReport SetLibraryKit(std::string library_uid, std::vector<std::string> parts, int offered = -1);

// The rig of an item: its skeleton signature (SkeletonKey) and its model's signature (the meshes'
// shapes, ModelSignature: the clips of one model, each in its own file, share it), both cached per
// animation file and refreshed when the file changes ("" when the file does not load); its file name
// and bone count.
struct ItemRigInfo {
    std::string skeleton;
    std::string model;
    std::string file;
    int         bones = 0;
};
ItemRigInfo ItemRig(MediaItem* item);
// The rigs present in the project's RAV items: per model, its files and how many items play it.
struct RigModelCount {
    std::string              model;
    std::vector<std::string> files;  // first seen first
    int                      items = 0;
};
struct ProjectRig {
    std::string                skeleton;
    int                        bones = 0;
    int                        items = 0;
    std::vector<RigModelCount> models;
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
