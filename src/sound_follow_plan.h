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
#include <set>
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
    // Fix C 2026-10-12: its rule is enabled. A disabled rule's events are paused, not gone:
    // Populate places nothing on them, the follow keeps their sounds muted (FollowInput::disabled_rids).
    bool enabled = true;
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
// "RAVSNDFOLLOW 1 pos=<s> pt=<s> [mute=1] [hand=<why>] [pass=<id>]".
constexpr const char kFollowStoreKey[] = "P_EXT:RAV_SNDFOLLOW";
struct FollowStore {
    double    pos = 0.0;         // the item's position RAV last saw / set
    double    pt = 0.0;          // its event's project time then (on its pass)
    bool      mute = false;      // muted by the follow (event outside the item, or its rule disabled)
    // Fix C 2026-10-12: the user changed the mute state by hand while the follow wanted it muted:
    // left as it is until that reason ends (the event back inside, the rule enabled again). The
    // reason it was given for: kMuteOutside or kMuteDisabled (0 = none); another reason mutes again.
    int       hand = 0;
    bool      has_pass = false;  // the pass below is known (an older line has none)
    long long pass = 0;          // FollowPass::id of the pass the sound sits on
};
std::string FormatFollowStore(const FollowStore& s);
bool        ParseFollowStore(const std::string& text, FollowStore* out);

// Why the follow wants a sound silent (FollowStore::hand).
constexpr int kMuteOutside = 1;   // its event is outside the item
constexpr int kMuteDisabled = 2;  // its rule is disabled

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
    std::string tag_rid;            // its placed tag's rule id and node (finds a copied sound item)
    std::string tag_node;
    // Its placed tag's pile anchor (13-8, RAVSND 2: ao, sy): one pile on one pass (PlacedAnchorOf).
    bool        tag_has_anchor = false;
    double      tag_ao = 0.0;
    double      tag_sy = 0.0;
};

constexpr double kStrayPosTolS = 1e-3;  // a copied sound item this close to its source + offset is its copy

// Fix C review 2 (2026-10-12): what the follow last saw of an owner that is no longer in the
// project (cut, deleted, moved away): its position and its placed items' facts then. A copy
// pasted from it finds the sound items pasted with it by these positions.
struct FollowOwnerMemory {
    std::string                  owner;  // its GUID
    double                       pos = 0.0;
    std::vector<FollowItemFacts> facts;
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
    // node uid -> its Blend offset (ms), BlendOffsetMs; 13-31: "<rid>\x1f<node uid>" -> the offset on
    // that rule's events (its role), read first.
    std::vector<std::pair<std::string, double>> blend_ms;
    // Fix C 2026-10-12.
    std::vector<std::string> disabled_rids;  // rules of the item that are disabled: their sounds muted, kept
    double owner_pos = 0.0;                  // the animation item's position
    // Other animation items' positions (guid -> D_POSITION): the source of a copy.
    std::vector<std::pair<std::string, double>> owner_positions;
    // Tagged sound items in no record (copied with the animation item, or pasted after a cut).
    std::vector<FollowItemFacts> strays;
    // Fix C review 2: the animation item's length (D_LENGTH), and owners no longer in the project
    // as last seen. A copy whose source is gone (pasted a second time after a cut, or copied then
    // deleted) adopts / removes the sound items pasted with it, tagged for that gone source.
    double                         owner_len = 0.0;
    std::vector<FollowOwnerMemory> remembered;
    // A move (cut and paste: a new GUID): the GUID it had, its placed items' facts as last seen
    // and how far it moved. Its record is kept (no detach, no new draw).
    std::string                  moved_from;
    std::vector<FollowItemFacts> moved_items;
    double                       moved_delta = 0.0;
    // GUIDs it had before that (a paste undone then pasted again): items tagged for them are its too.
    std::vector<std::string>     moved_also;
    // 13-56: the animation item loops its source (B_LOOPSRC). A populated sound on a loop pass other
    // than the first (FollowPass id != 0) whose pass no longer shows its event (the item shortened,
    // its rate or offset changed) is removed, its placement dropped from the record, like a gone
    // event's (F5), unless it is the last live one of its node in the event (LastOfNodeKept: muted).
    // A spread sound there, one on the first pass, and one on an item that does not loop are muted as
    // before (unmuted when the pass comes back). A populated event with a pass that has no sound of a
    // node gets FollowPlan::populate_passes.
    bool                         owner_loops = false;
    // 13-59: New variation per loop pass (sound_populate_plan.h LoopPassDraw): a pass may play other
    // nodes than the first (a Random container drew another child), so a pass needs a fill only when
    // it has no live populated sound of the event at all.
    bool                         pass_draws = false;
};

enum class FollowKind { Move, Remove, Detach, Forget, Mute, Unmute, Store, Retag };

struct FollowAction {
    FollowKind  kind = FollowKind::Move;
    std::string item_guid;
    double      project_t = 0.0;  // Move: the item's new position
    FollowStore store;            // Store: the line to write
    double      tag_t = 0.0;      // Retag: the event's new clip time
    std::string tag_owner;        // Retag: the new owner GUID ("" = unchanged)
};

struct FollowPlan {
    std::vector<FollowAction> actions;
    ItemSounds                record;  // the record after the plan
    bool                      record_changed = false;
    std::vector<ReportLine>   report;
    bool                      populate_copy = false;  // F8: the item is a copy, populate it
    // 13-56: the item loops and a populated event (a live Populated placement, no Attached or Spread
    // one, its rule enabled) has a loop pass where a node it plays has no sound, and no sound of
    // that node was deleted by hand (Populate never places such a node again): the caller fills the
    // passes (Populate on the populated events, no undo point).
    bool                      populate_passes = false;
};

FollowPlan PlanItemFollow(const FollowInput& in);
// 13-56 review: the populated sounds of event `ev` (matched to `e`) on a cut loop pass that are the
// last live populated sound of their node in the event (one per node): PlanItemFollow mutes them
// instead of removing them, so stretching the item again fills the event's passes (and unmutes that
// one when its pass comes back).
std::set<std::string> LastOfNodeKept(const EventSounds& ev, const FollowEvent& e, const FollowInput& in);

// F9: the owner is gone; what its placements (as cached) must do. `also_owners`: other GUIDs the
// owner had (a move, fix C 2026-10-12): items still tagged for one of them are handled too.
FollowPlan PlanOwnerGone(const std::string& owner_guid, const std::vector<SoundPlacement>& placed,
                         const std::vector<FollowItemFacts>& items, const std::vector<std::string>& also_owners = {});

// Fix C 2026-10-12: `facts` as they are once `plan` is applied (positions moved, mutes, stores,
// tags, removals): what the follow keeps of an owner's items after its tick.
void FactsAfterPlan(const FollowPlan& plan, std::vector<FollowItemFacts>* facts);

// Fix C 2026-10-12: an undo point name that is a cut (REAPER's "Cut items", ...): the word "cut",
// any case. A deleted animation item's sounds wait for its paste only after a cut.
// Fix C review 2: also a name containing one of `localized` (REAPER's cut names in its language
// pack, LocalizeString), any case, or the French word "couper".
bool IsCutUndoName(const char* desc, const std::vector<std::string>& localized = {});
// Fix C review 2: an undo point name that is a selection change (REAPER's preference "include
// selection in undo"): contains "select" (any case), the French "selection" with its accent, or one of
// `localized`. A cut keeps waiting for its paste over it.
bool IsSelectionUndoName(const char* desc, const std::vector<std::string>& localized = {});

// Fix C review 2: the animation items in the project now (`keys`, upper-case GUIDs) against those
// seen before. Returns, for each key, whether it appeared (not seen before, after the first scan);
// `seen` becomes `keys`, so an item gone and back (undo, redo) appears again.
std::vector<bool> UpdateSeenOwners(std::set<std::string>* seen, const std::vector<std::string>& keys, bool scanned);

// Fix C 2026-10-12: cut and paste. An animation item gone (cut) and one that appeared are the
// same item moved when they have the same rules record and file, the appeared one's record lists
// the same placed items as the gone one's did, and none of its placed items present now is tagged
// for another owner than the gone one (a duplicate of another instance is never a move). The
// pairs (gone index, appeared index), each side matched once only.
struct MoveCandidate {
    std::string guid;
    std::string rules_raw;
    std::string path;
    std::vector<std::string> placed;  // its record's placed item GUIDs (any case)
    // Gone: the owner GUIDs its items may be tagged for (its own, and those it had before).
    // Appeared: the tag owners of its placed items present now.
    std::vector<std::string> owners;
};
std::vector<std::pair<size_t, size_t>> MatchMovedOwners(const std::vector<MoveCandidate>& gone,
                                                        const std::vector<MoveCandidate>& appeared);

// Case-insensitive GUID compare (REAPER's braces, any case).
bool SameFollowGuid(const std::string& a, const std::string& b);
// A 64-bit hash of a GUID (case-insensitive): the copy's draw seed (F8).
uint64_t FollowGuidHash(const std::string& guid);

// Kept from 13-2: what the placements of `rec` must do for these events; `present` = the placed
// GUIDs still in the project (positions not followed).
FollowPlan PlanFollow(const ItemSounds& rec, const std::vector<FollowEvent>& events,
                      const std::vector<std::string>& present);

}  // namespace rav
