// SPDX-License-Identifier: MIT
//
// Follow plan (13-9, pure): how placed sounds follow their animation item's events.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-9
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// One convergence step per animation item (spec 13-9 F1-F9): the REAPER side (sound_follow.cpp)
// gathers the facts, PlanItemFollow decides, the REAPER side applies. Every input is state that
// undo restores, so a plan applied and planned again yields no action and an unchanged record.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sound_model.h"

namespace rav {

struct ItemRules;
struct ItemClipMap;

// An event of the animation item now (rule id, clip time, project time).
struct FollowEvent {
    std::string rid;
    double      t = 0.0;
    double      project_t = 0.0;  // meaningful only when in_item
    bool        in_item = true;   // on the item's first pass
    bool        user = false;     // a User event (else Detected)
};

// The committed events (F1): Detected entries not masked by a Suppress of the same rule within
// kSuppressWindowS, plus User entries; the rule valid, enabled, with a rid. Clip-time order.
std::vector<FollowEvent> CommittedFollowEvents(const ItemRules& rules, const ItemClipMap& map);

// The follow store on each placed item (P_EXT:RAV_SNDFOLLOW, F3):
// "RAVSNDFOLLOW 1 pos=<s> pt=<s> [mute=1]".
constexpr const char kFollowStoreKey[] = "P_EXT:RAV_SNDFOLLOW";
struct FollowStore {
    double pos = 0.0;    // the item's position RAV last saw / set
    double pt = 0.0;     // its event's project time then
    bool   mute = false; // muted by the follow (event outside the item)
};
std::string FormatFollowStore(const FollowStore& s);
bool        ParseFollowStore(const std::string& text, FollowStore* out);

constexpr double kFollowPosTolS = 1e-4;  // a placed item this close to its stored pos did not move
constexpr double kFollowPtTolS = 1e-6;   // an event this close to its stored pt did not move

// What the follow reads of one placed item.
struct FollowItemFacts {
    std::string item_guid;
    bool        present = false;  // in the project now
    double      pos = 0.0;        // D_POSITION
    std::string track_guid;
    bool        has_store = false;
    FollowStore store;
    bool        muted = false;    // B_MUTE
    bool        has_tag = false;
    std::string tag_owner;
    double      tag_t = 0.0;
};

struct FollowInput {
    std::string                  owner_guid;
    ItemSounds                   rec;
    std::vector<FollowEvent>     events;
    std::vector<FollowItemFacts> items;      // facts of the placed GUIDs (missing = not present)
    std::vector<std::string>     rule_rids;  // the rids of the item's rules (any state)
    std::vector<std::pair<std::string, std::string>> library_names;  // uid -> name (report text)
    bool                         appeared = false;  // first seen after the project was scanned
    bool                         keep_draw = false;
    uint64_t                     copy_seed = 0;
};

enum class FollowKind { Move, Remove, Detach, Forget, Mute, Unmute, Store, Retag };

struct FollowAction {
    FollowKind  kind = FollowKind::Move;
    std::string item_guid;
    double      project_t = 0.0;  // Move: the item's new position
    FollowStore store;            // Store: the line to write
    double      tag_t = 0.0;      // Retag: the event's new clip time
};

struct FollowPlan {
    std::vector<FollowAction> actions;
    ItemSounds                record;  // the record after the plan
    bool                      record_changed = false;
    std::vector<ReportLine>   report;
    bool                      populate_copy = false;  // F8: the item is a copy, populate it
};

FollowPlan PlanItemFollow(const FollowInput& in);

// F9: the owner is gone; what its placements (as cached) must do.
FollowPlan PlanOwnerGone(const std::string& owner_guid, const std::vector<SoundPlacement>& placed,
                         const std::vector<FollowItemFacts>& items);

// Case-insensitive GUID compare (REAPER's braces, any case).
bool SameFollowGuid(const std::string& a, const std::string& b);
// A 64-bit hash of a GUID (case-insensitive): the copy's draw seed (F8).
uint64_t FollowGuidHash(const std::string& guid);

// Kept from 13-2: what the placements of `rec` must do for these events; `present` = the placed
// GUIDs still in the project (positions not followed).
FollowPlan PlanFollow(const ItemSounds& rec, const std::vector<FollowEvent>& events,
                      const std::vector<std::string>& present);

}  // namespace rav
