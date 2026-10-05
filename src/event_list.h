// SPDX-License-Identifier: MIT
//
// The item's event list (Epic 10, story 10-4): what the Tagging view shows and what Apply
// writes, from the live detections and the record's entries (rule_record.h).
//
//   - Displayed list = each rule's live detections, each `Suppressed` when a suppression of
//     that rule lies within +-30 ms (inclusive), else `Detected`; plus the suppressions with
//     no detection of their rule that near (`Orphan`); plus the user's own events (`User`,
//     fixed in time: detection never moves them).
//   - Apply writes the Detected and User events of the rules that are on, at their project
//     time on the item's FIRST pass of the clip only (RAV plays the clip once then holds its
//     last frame), inside the item's bounds.
//   - Deleting or duplicating a rule remaps the entries' rule indices (a deleted rule's user
//     events and suppressions go with it).
//   - The event list belongs to the take, like REAPER's take markers and P_EXT: a changed
//     source path (relinked, moved, replaced, re-exported) never clears it. Loading a preset
//     does (ClearCorrections).
//   - "Markers up to date" = the last Apply ran with the same option and wrote the same
//     markers (a signature of the planned markers).
//
// Pure C++17: no REAPER, no ImGui, no Windows. Host-tested (tests/event_list_test.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bone_events.h"
#include "rule_record.h"
#include "tagging_signal.h"

namespace rav {

constexpr double kSuppressWindowS = 0.030;  // a suppression masks detections this close (inclusive)
constexpr double kMarkerTwinTolS = 0.001;   // a foreign marker this close with the same name = already present

enum class ShownKind { Detected, Suppressed, Orphan, User };

struct ShownEvent {
    double    t = 0.0;  // clip seconds
    int       block = 0;
    ShownKind kind = ShownKind::Detected;
    double    strength = 0.0;  // Detected / Suppressed: detection's; User: as stored
    double    speed = 0.0;
    int       entry = -1;      // Suppressed: the suppression; Orphan / User: the entry (index into ItemRules::events)
    int       detection = -1;  // Detected / Suppressed: index into the detections given
};

// The displayed list, sorted by time (then rule). `detections` = the live detections (any
// order, Event::block = the rule), `entries` = the record's. Entries of an unknown kind, of
// the Detected kind (Apply's snapshot) or of a rule out of [0, n_blocks) are not shown.
std::vector<ShownEvent> BuildEventList(const std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                                       size_t n_blocks);

// New entries (every optional field written).
EventEntry MakeUserEvent(int block, double t, double strength, double speed);
EventEntry MakeSuppression(int block, double t);

// ---- Rule delete / duplicate ------------------------------------------------------------------

// old index -> new index (-1 = gone) when rule `deleted` of n is deleted.
std::vector<int> BlockMapForDelete(size_t n_blocks, int deleted);
// ... when a rule is inserted at `at` (the rules from `at` on move one down).
std::vector<int> BlockMapForInsert(size_t n_blocks, int at);
// Applies the map to the entries of known kinds: an entry of a gone rule is erased. An index
// outside the map is left as it is.
void RemapEventBlocks(std::vector<EventEntry>& entries, const std::vector<int>& old_to_new);

// ---- The first pass of the clip on the timeline ------------------------------------------------

// The item's first pass of the clip (clip time 0 .. clip_len), clipped to the item. False
// when the item shows nothing of it.
bool FirstClipPass(const ItemClipMap& m, ClipPass* out);
// Clip time -> project time on the first pass, inside the item's bounds. False outside.
bool FirstPassProjectTime(const ItemClipMap& m, double clip_t, double* project_t);
// Project time -> clip time on the first pass. False outside it.
bool FirstPassClipTime(const ItemClipMap& m, double project_t, double* clip_t);

// ---- Apply ------------------------------------------------------------------------------------

struct PlannedMarker {
    double      clip_t = 0.0;     // = the take marker's source position
    double      project_t = 0.0;  // the project marker's position
    int         block = 0;
    std::string name;             // the rule's marker name, on one line, trimmed
    uint32_t    color = 0;        // the rule's colour (Block::color: 0 = none)
    bool        user = false;     // a user event (else a detection)
    double      strength = 0.0;
    double      speed = 0.0;
};

// What Apply writes for this list: its Detected and User events whose rule exists and is on,
// on the first pass, sorted by time.
std::vector<PlannedMarker> PlanMarkers(const std::vector<ShownEvent>& list, const std::vector<Block>& blocks,
                                       const ItemClipMap& map);

// A stable signature of the planned markers for a mode (the project times count only when
// project markers are written). 16 hex digits.
std::string MarkerSignature(const std::vector<PlannedMarker>& planned, MarkerMode mode);

// True when the last Apply wrote these markers with this option.
bool MarkersUpToDate(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);

// Apply's record: the Detected entries are replaced by the detections written (their values
// kept), `applied` set. The owned-marker lists are the caller's.
void RecordApplied(ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);

// A marker already there (not RAV's): its position and name.
struct ExistingMarker {
    double      t = 0.0;
    std::string name;
};
// The index of a marker with exactly this name within +-tol of t, or -1.
int FindTwinMarker(const std::vector<ExistingMarker>& existing, double t, const std::string& name,
                   double tol = kMarkerTwinTolS);

// ---- Preset loaded --------------------------------------------------------------------------------

// Loading a preset clears the item's corrections: the entries of known kinds (detections as
// applied, user events, suppressions) and the applied state. The owned-marker lists (so the
// next Apply still removes RAV's old markers) and unknown-kind entries stay.
void ClearCorrections(ItemRules& rules);

}  // namespace rav
