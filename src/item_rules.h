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

// Reads the active take's record. False when the item has no RAV take.
bool ReadItemRules(MediaItem* item, ItemRulesRead* out);

// Reads the record (an unreadable or absent one = no rules), lets `edit` change it, and
// writes it back in one undo point named `undo_desc`. `edit` returns false to cancel
// (nothing written). False when nothing was written because of an error (`err` says why).
bool ModifyItemRules(MediaItem* item, const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                     std::string* err);

// Writes the whole record (one undo point).
bool WriteItemRules(MediaItem* item, const ItemRules& rules, const char* undo_desc, std::string* err);

// The bone names of the item's animation file (empty when it does not load).
std::vector<std::string> ItemBoneNames(MediaItem* item);

// Binds `blocks` on the item's skeleton with the remembered role mapping (role_map_store.h).
// False when a role or bone is missing (`missing` names them), or the file or its bones do
// not load (`missing` then says so).
bool CheckItemBinding(MediaItem* item, const std::vector<Block>& blocks, std::string* missing);

// Sets a preset up on the item: its options and blocks, and the copy (one undo point,
// "RAV: Load preset <name>"). The rules are written even when a role is missing:
// `missing` then names them (the caller says so; Apply skips the item).
bool SetUpPresetOnItem(MediaItem* item, const std::string& preset_id, std::string* missing, std::string* err);

// Legacy band: Update (the preset as it is now replaces the item's rules) or Keep current.
bool UpdateItemFromPreset(MediaItem* item, std::string* err);
bool KeepItemCurrent(MediaItem* item, std::string* err);

// REAPER's resource path (presets, roles.txt live under <it>/ReaAnimViewer).
std::string RulesResourceRoot();

}  // namespace rav

#endif  // _WIN32
