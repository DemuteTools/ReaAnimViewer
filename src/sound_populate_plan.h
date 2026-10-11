// SPDX-License-Identifier: MIT
//
// Populate plan (13-8, pure, no REAPER): the decisions behind Populate, Reshuffle and the Sound
// sections -- orphan rules, the value layers of an event, the draw hooks, what each event slot
// (pass x resolved sound) does on a re-Populate, the footer count and the report line.
// Epic 13 (D20). Owned by story 13-8. Shared types: sound_model.h.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sound_model.h"
#include "sound_switch.h"

namespace rav {

// ---- P1: rules that no longer exist ----------------------------------------------------------

struct OrphanPrune {
    std::vector<RuleSounds>  rules;          // the assignments removed (their rid is gone)
    std::vector<std::string> remove_guids;   // Populated placements of events whose rid is gone
};
// Removes every RuleSounds whose rid is not in `live_rids`, and the Populated / Deleted placements
// of the events whose rid is gone (an event left with nothing is dropped; Attached / Spread ones
// stay, for 13-9).
OrphanPrune PruneOrphanRules(ItemSounds& item, const std::set<std::string>& live_rids);
// The report text of a removed assignment: `Rule gone: library "<name>" unassigned`.
std::string RuleGoneText(const std::string& library_name);

// ---- Values ----------------------------------------------------------------------------------

// The item's values, the rule's, and the event's (rid, t) overrides.
ValueLayers LayersFor(const ItemSounds& item, const std::string& rid, double t);
// Sets `group` to `value` in the list ("" removes it). True when the list changed.
bool SetValueIn(std::vector<ValueSet>& values, const std::string& group, const std::string& value);

// The Switch Groups a library uses: each group of a reachable Switch, once, in walk order (root
// first, children in line order), with the first reachable Switch on it.
struct GroupUse {
    std::string      group_uid;
    const SoundNode* first_switch = nullptr;
};
std::vector<GroupUse> GroupsUsed(const SoundStore& store, const SoundLibrary& lib);

// ---- P10: the draw ---------------------------------------------------------------------------

// The salt of one node of one event: EventSalt(rid, t) ^ FNV-1a(node uid).
uint64_t NodeSalt(const std::string& rid, double t, const std::string& node_uid);

// One item's draw state across its events (time order): the history of picks per node.
struct DrawState {
    const SoundLibrary*                     lib = nullptr;
    uint64_t                                seed = 0;
    std::map<std::string, std::vector<int>> history;  // node uid -> picks so far
};
// One event's draw: reads `prev` (its stored draws), writes `out` (the draws this resolution used,
// a hand pick kept hand), and `no_active` gets every Sound with frames but none on.
struct EventDraw {
    std::string                  rid;
    double                       t = 0.0;
    const std::vector<SoundDraw>* prev = nullptr;
    std::vector<SoundDraw>       out;
    std::set<std::string>        no_active;
};
// The hooks for ResolveEvent: Random -> a child index, Sound -> a variation index (-1 = as copied:
// no frames, or none on), through KeepOrDraw. `is_missing` is passed through.
ResolveHooks MakeDrawHooks(DrawState& state, EventDraw& ev, std::function<bool(const SoundNode&)> is_missing);

// ---- P9: event slots -------------------------------------------------------------------------

struct DesiredSlot {
    int         pass = 0;
    std::string node;
    int         variation = -1;
};
struct ExistingSlot {
    int         pass = -1;  // the pass nearest the placed item
    std::string node;
    int         variation = -1;
    Ownership   own = Ownership::Populated;
};
enum class SlotAction { Keep, SetVariation, ReplaceNode, Remove, Place };
struct SlotStep {
    SlotAction       act = SlotAction::Keep;
    int              desired = -1;    // index into desired (not for Remove)
    std::vector<int> existing;        // indices into existing (Keep / SetVariation / ReplaceNode: the
                                      // slot's items, the first one is replaced; Remove: removed)
};
// Per pass: an existing Populated slot (its items grouped by node) with the same node keeps it
// (same variation) or changes the variation; the remaining desired slots take the remaining
// existing slots in order (other node); the rest of the existing are removed, the rest of the
// desired placed. A desired node that has a Deleted placement in the event is never placed again
// (no Place, no ReplaceNode into another slot), but its live copies are kept. Attached / Spread
// entries are ignored.
std::vector<SlotStep> PlanEventSlots(const std::vector<DesiredSlot>& desired, const std::vector<ExistingSlot>& existing);
// The index of the pass time nearest `pos` (-1 when none).
int NearestPass(const std::vector<double>& pass_times, double pos);
// The pass of a placed item: the pass nearest where its sync point lands (its position plus its
// sync offset, minus the Blend offset it was placed with).
int NearestPassOfItem(const std::vector<double>& pass_times, double item_pos, double sync_offset, double blend_ms);
// P9: every Populated placement whose item no longer exists (`exists` false) becomes Deleted.
// Returns how many were marked.
int MarkDeletedPlacements(EventSounds& ev, const std::function<bool(const std::string&)>& exists);
// True when Populate / Reshuffle / replace leave the event alone (an Attached or Spread placement);
// the footer count uses the same test.
bool EventSkippedByPopulate(const EventSounds* ev);
// The user's nudge of a placed item: its position minus where it lands with no nudge.
double NudgeOf(double item_pos, double project_t, double sync_offset, double blend_ms);
// The Blend offsets (ms) summed from the node up to the root (what ResolveEvent adds on its path).
double BlendOffsetMs(const SoundLibrary& lib, const std::string& node_uid);
// True when the event has an Attached or Spread placement (Populate leaves it alone).
bool EventHasUserPlacement(const EventSounds& ev);
// True when the event has a live Populated placement.
bool EventIsPopulated(const EventSounds& ev);

// ---- P15: the footer -------------------------------------------------------------------------

// The events Populate places: one per event whose rule has a library that is not blocked.
// `rule_ok` per block (index = ShownEvent::block); `event_blocks` = the blocks of the events.
int CountEvents(const std::vector<int>& event_blocks, const std::vector<char>& rule_ok);
// "Last populate: 24 events · 2 missing · 1 replaced · 1 rule gone" (events < 0: "Last change: ...").
std::string ReportSummary(const SoundReport& report, int events);

}  // namespace rav
