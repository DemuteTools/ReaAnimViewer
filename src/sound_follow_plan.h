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
struct ShownEvent;
struct Block;

// One place on the timeline where an event's clip time shows (one per loop pass inside the
// item). `id` = the clip loop the pass plays (its source time is id * clip_len + t): it stays the
// same when the item moves, is trimmed at its start or changes rate, so a placed sound keeps its
// pass. 0 when the item does not loop.
struct FollowPass {
    long long id = 0;
    double    project_t = 0.0;
};
// The passes of clip time `clip_t` inside the item, in time order: the same project times as
// ClipToProjectTimes (tagging_signal.h), which Populate places on.
std::vector<FollowPass> FollowPassesOf(const ItemClipMap& map, double clip_t);

// An event of the animation item now (rule id, clip time, project time).
struct FollowEvent {
    std::string rid;
    double      t = 0.0;
    double      project_t = 0.0;  // meaningful only when in_item
    bool        in_item = true;   // on the item's first pass
    bool        user = false;     // a User event (else Detected)
    // Every pass of the event inside the item (SoundEventsOf fills it). Empty = only the first
    // pass above counts (id 0), as CommittedFollowEvents gives it.
    std::vector<FollowPass> passes;
};

// The events sounds are placed on (13-8 Populate) and followed on (13-9), from the item's live
// event list (BuildEventList over DetectItem's detections and the record's entries, the list the
// marker mirror and Commit use): its Detected and User events whose rule has a rid, in the list's
// order, each with all its passes inside the item (in_item / project_t = the first of them).
// Fix 2026-10-12: Populate and the follow both read this one list, so a follow tick never sees
// fewer events than Populate placed sounds on.
std::vector<FollowEvent> SoundEventsOf(const std::vector<ShownEvent>& shown, const std::vector<Block>& blocks,
                                       const ItemClipMap& map);

// Kept from the first 13-9 cut, no longer what the follow reads (SoundEventsOf is).
// The committed events (F1): Detected entries not masked by a Suppress of the same rule within
// kSuppressWindowS, plus User entries; the rule valid, enabled, with a rid. Clip-time order.
std::vector<FollowEvent> CommittedFollowEvents(const ItemRules& rules, const ItemClipMap& map);

// The follow store on each placed item (P_EXT:RAV_SNDFOLLOW, F3):
// "RAVSNDFOLLOW 1 pos=<s> pt=<s> [mute=1] [pass=<id>]".
constexpr const char kFollowStoreKey[] = "P_EXT:RAV_SNDFOLLOW";
struct FollowStore {
    double    pos = 0.0;         // the item's position RAV last saw / set
    double    pt = 0.0;          // its event's project time then (on its pass)
    bool      mute = false;      // muted by the follow (event outside the item)
    bool      has_pass = false;  // the pass below is known (an older line has none)
    long long pass = 0;          // FollowPass::id of the pass the sound sits on
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
    double      sync_offset = 0.0;  // D_SNAPOFFSET (finds its pass at first sight, as Populate does)
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
    std::vector<std::pair<std::string, double>> blend_ms;  // node uid -> its Blend offset (ms), BlendOffsetMs
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
