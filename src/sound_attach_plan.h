// SPDX-License-Identifier: MIT
//
// Attach plan (13-10, pure): which selected items form a pile, which event a pile attaches to,
// where Spread puts each member, and where Promote inserts a new sound in a library.
// No REAPER: host-tested (tests/sound_attach_plan_test.cpp). The gestures: sound_attach.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// What attach reads of a selected item.
struct AttachItem {
    std::string guid;
    std::string track_guid;
    int         track = 0;  // IP_TRACKNUMBER (1-based)
    double      pos = 0.0;
    double      len = 0.0;
    double      snap = 0.0;  // D_SNAPOFFSET
    int         takes = 0;
};

// One event a pile may attach to: a rule's event at one of its project times.
struct AttachEvent {
    int         block = -1;     // the rule (index into ItemRules::blocks)
    std::string rid;            // filled by the gesture once the rules have ids
    double      clip_t = 0.0;   // clip seconds (the event's identity with rid)
    double      project_t = 0.0;
};

// Why a selected item is not attached ("" = eligible): a RAV animation item, a folder item, an
// item RAV placed (populated / spread). An attached item is eligible (it re-attaches, D3).
std::string AttachSkipReason(bool rav_item, bool folder, bool has_tag, Ownership own);

// The items grouped by overlapping time spans (touching counts), each group's indices sorted by
// position; groups in time order.
std::vector<std::vector<int>> OverlapGroups(const std::vector<AttachItem>& items);
// The topmost of `indices`: lowest track number, then earliest position. -1 when empty.
int TopmostOf(const std::vector<AttachItem>& items, const std::vector<int>& indices);
// A pile's anchor: its top item's position + snap offset.
inline double PileAnchor(const AttachItem& top) { return top.pos + top.snap; }
// The event nearest to `anchor` (project s), of rule `block` (-1 = any); tie: the earlier one.
// -1 when none.
int NearestEvent(const std::vector<AttachEvent>& events, double anchor, int block = -1);
// True when a pile anchored at `anchor` sits on the event (+-30 ms).
bool PileOnEvent(double anchor, double event_project_t);
// A Spread member's PlaceRequest::offset_s: the engine lands it at event - snap + offset, so it
// keeps its place in the pile relative to the anchor, plus the nudge.
double SpreadMemberOffset(double anchor, double pos, double snap, double nudge);
// Where a promoted copy lands: after the project end (+1 s), relative positions kept
// (`first_pos` = the earliest member's position).
double PromoteCopyPosition(double project_end, double first_pos, double member_pos);
// A Blend child's offset (ms): its sync point after the top member's.
double BlendChildOffsetMs(double member_sync, double top_sync);
// The take a Spread event plays (DrawPick over all `takes`, avoid the last 2). -1 when takes < 2.
int SpreadTakePick(int takes, const std::vector<int>& history, uint64_t seed, uint64_t salt);

// The attach plan (D4): the piles of `items` (one pile of all when `block` < 0, else the overlap
// groups), each with its nearest event of `block` (-1 = none) and the offset_s every member
// records: anchor - event project time.
struct AttachPilePlan {
    std::vector<int> members;
    int              event = -1;
    double           anchor = 0.0;
    double           offset_s = 0.0;
};
std::vector<AttachPilePlan> PlanAttach(const std::vector<AttachItem>& items, const std::vector<AttachEvent>& events,
                                       int block);
// The record placement and the RAV_SND tag of a member attached to `ev` (owner = the animation
// item's GUID; the event is keyed by its clip time).
SoundPlacement AttachPlacementOf(const AttachItem& item, double offset_s);
PlacedTag      AttachTagOf(const std::string& owner, const AttachEvent& ev);

// Spread targets (D5): the events of `block` in time order, but the one the pile sits on (anchor
// +-30 ms) and, when non-null, the clip event the pile's top item is attached to.
std::vector<AttachEvent> SpreadTargets(const std::vector<AttachEvent>& events, int block, double anchor,
                                       const double* attached_clip_t);
// One spread placement taken out of the record, with its event's clip time.
struct TakenCopy {
    SoundPlacement p;
    double         t = 0.0;
};
// D5 / B7: takes every target event's spread placements out of `rec` (populated and attached ones
// stay). `nudges[k]` = target k's nudge: its first old spread copy's offset_s, else that of an
// earlier target of the same clip event, else 0.
std::vector<TakenCopy> TakeSpreadCopies(ItemSounds& rec, const std::string& rid, const std::vector<AttachEvent>& targets,
                                        std::vector<double>* nudges);
// D6: the take count driving the shuffle: the first member with >= 2 takes (0 = no shuffle).
int SpreadPickTakes(const std::vector<AttachItem>& items);
// D6 / B7: one pick per target (SpreadTakePick, history = earlier events' picks); the project
// times of one clip event share its pick. All -1 when `takes` < 2.
std::vector<int> SpreadPicks(const std::vector<AttachEvent>& targets, int takes, uint64_t seed, const std::string& rid);

// The record edits. Removes `guid`'s placement from every event (an event left holding nothing
// is dropped). True when something was removed.
bool DetachFromRecord(ItemSounds& rec, const std::string& guid);
// Adds a placement to the event (rid, t), creating the event when absent.
void AddToRecord(ItemSounds& rec, const std::string& rid, double t, const SoundPlacement& p);

// Promote (D8): inserts `nodes` (nodes[0] = the new top Sound or Blend, parent ignored; the
// others its children, parent = nodes[0].uid) into the library. Root Switch: under the value
// `value_uid` (no case: a new case; a Random child: into it; another child: a new Random
// `new_uid` named `random_name` holding the old child + the new). Root Random / Blend: a new
// child. Root Sound: a new Random root `new_uid` holding the old root + the new. No root: the new
// node becomes the root. False (and `error`) when it cannot be inserted.
bool AddPromotedToLibrary(SoundLibrary& lib, const std::vector<SoundNode>& nodes, const std::string& value_uid,
                          const std::string& new_uid, std::string* error, const std::string& random_name = "");

}  // namespace rav
