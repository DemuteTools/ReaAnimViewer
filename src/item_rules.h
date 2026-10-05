// SPDX-License-Identifier: MIT
//
// An item's rules in REAPER (Epic 10, story 10-2): the record (rule_record.h) lives in
// the item's ACTIVE take, in P_EXT:RAV_RULES. REAPER copies take ext-data when an item
// is duplicated and keeps it in the project when RAV is not installed.
//
// Every write is one REAPER undo point named after the gesture: the record is set first,
// then Undo_OnStateChangeEx(desc, UNDO_STATE_ITEMS, -1) records it, only when REAPER
// accepted the set (a refused write leaves no undo point). A write that changes nothing writes nothing: an absent or
// unreadable record left at "no rules" stays as it is.
// A record that does not read is left as it is until the user writes (it then starts
// from no rules).
//
// Presets and the role mapping come from REAPER's resource path (preset_store.h,
// role_map_store.h). Nothing here runs Analyse or detection.
//
// Main thread only. No-throw.

#pragma once

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "reaper_api.h"
#include "rule_record.h"

namespace rav {

struct SceneSkeleton;

constexpr const char kItemRulesKey[] = "P_EXT:RAV_RULES";
// The largest record read or written (the P_EXT read needs a buffer that big).
constexpr size_t kItemRulesMaxBytes = 1u << 20;

struct ItemRulesRead {
    bool        present = false;  // the take has a non-empty record
    bool        valid = false;    // ... and it reads (`rules` holds it)
    ItemRules   rules;
    std::string raw;
};

// The item's RAV animation take: its active take when its source is a RAV animation
// (also through a section / reversed wrapper). Null otherwise.
MediaItem_Take* RavTakeOf(MediaItem* item);

// The animation file under a take (through a section / reversed wrapper). "" when the take
// is not a RAV animation.
std::string AnimPathOf(MediaItem_Take* take);

// Reads the active take's record. False when the item has no RAV take.
bool ReadItemRules(MediaItem* item, ItemRulesRead* out);

// Reads the record (an unreadable or absent one = no rules), lets `edit` change it, and
// writes it back in one undo point named `undo_desc`. `edit` returns false to cancel
// (nothing written). False when nothing was written because of an error (`err` says why).
bool ModifyItemRules(MediaItem* item, const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                     std::string* err);

// The same without an undo point of its own: for a write inside a caller's
// Undo_BeginBlock2 / Undo_EndBlock2 (Apply). A null `undo_desc` there = no undo point.
bool ModifyItemRulesNoUndo(MediaItem* item, const std::function<bool(ItemRules&)>& edit, std::string* err);

// Writes the whole record (one undo point).
bool WriteItemRules(MediaItem* item, const ItemRules& rules, const char* undo_desc, std::string* err);

// A skeleton's bone names and parents (one per bone, -1 = root), what binding reads
// (BindBoneRefs: a joint angle needs the parents).
struct SkeletonBones {
    std::vector<std::string> names;
    std::vector<int>         parents;
};
SkeletonBones SkeletonBonesOf(const SceneSkeleton& skel);
// The item's animation file's (empty when it does not load).
SkeletonBones ItemSkeletonBones(MediaItem* item);

// Binds `blocks` on the item's skeleton with the remembered role mapping (role_map_store.h).
// False when a role or bone is missing (`missing` names them), or the file or its bones do
// not load (`missing` then says so).
bool CheckItemBinding(MediaItem* item, const std::vector<Block>& blocks, std::string* missing);

// Sets a preset up on the item: its options and blocks, and the copy (one undo point,
// "RAV: Load preset <name>"). The rules are written even when a role is missing:
// `missing` then names them (the caller says so; Apply skips the item).
bool SetUpPresetOnItem(MediaItem* item, const std::string& preset_id, std::string* missing, std::string* err);

// Story 10-3b -- Save / Save as: the item's options, analyse settings and blocks are written
// to a user preset (`preset_id` non-empty: that preset is overwritten and its version goes
// up; empty: a new preset named `name`), then the item adopts it (AdoptSavedPreset: it is no
// longer edited), one undo point "RAV: Save preset <name>". Writing the file is no undo
// point: Ctrl+Z restores the item's previous copy, never the file. `saved_id` / `saved_name`
// get the preset as written. False with `err` when the file or the item could not be written
// (the file stays written when only the item write failed).
bool SaveItemAsPreset(MediaItem* item, const std::string& preset_id, const std::string& name, std::string* saved_id,
                      std::string* saved_name, std::string* err);

// Legacy band: Update (the preset as it is now replaces the item's rules) or Keep current.
bool UpdateItemFromPreset(MediaItem* item, std::string* err);
bool KeepItemCurrent(MediaItem* item, std::string* err);

// REAPER's resource path (presets, roles.txt live under <it>/ReaAnimViewer).
std::string RulesResourceRoot();

}  // namespace rav

#endif  // _WIN32
