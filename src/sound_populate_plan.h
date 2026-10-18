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
#include <utility>
#include <vector>

#include "sound_model.h"
#include "sound_place_plan.h"  // 13-62i (D): PieceApply
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
struct PopulateDrawState {
    const SoundLibrary*                     lib = nullptr;
    uint64_t                                seed = 0;
    std::map<std::string, std::vector<int>> history;  // node uid -> picks so far
    // 13-62i (D): a filtered Populate's timeline: per history key, the events that play it (index in time order)
    // and their pick (-1 = drawn by this Populate), so a draw also avoids the picks kept after it (AheadPicks);
    // `at` = the index of the event being drawn. Empty = a full Populate (every event drawn in time order).
    std::map<std::string, std::vector<std::pair<int, int>>> timeline;
    int                                                     at = -1;
};
// 13-62i (D): fills the timelines of a filtered Populate: `draws[i]` = event i's recorded draws (null = none:
// not in the timeline), `fixed[i]` = it keeps them (left out but still playing); else it is drawn now. A loop
// pass's entry (PassDrawKey) goes to that pass's state in `passes` (made when missing; null = dropped).
void SetDrawTimeline(PopulateDrawState& state, std::map<long long, PopulateDrawState>* passes,
                     const std::vector<const std::vector<SoundDraw>*>& draws, const std::vector<char>& fixed);
// 13-62i (D): the picks a draw of `key` at event `state.at` avoids ahead: the kept picks among the next `n`
// events of the timeline that play `key` (the events drawn now avoid it by themselves).
std::vector<int> AheadPicks(const PopulateDrawState& state, const std::string& key, int n);
// One event's draw: reads `prev` (its stored draws), writes `out` (the draws this resolution used,
// a hand pick kept hand), and `no_active` gets every Sound with frames but none on.
struct EventDraw {
    std::string                  rid;
    double                       t = 0.0;
    const std::vector<SoundDraw>* prev = nullptr;
    char                         side = 0;  // 13-73: the event's side ('L' / 'R', EventSide; 0 = none): SideVariations
    std::vector<SoundDraw>       out;
    std::set<std::string>        no_active;
    // 13-23 (A5): Sound uid -> the part (value uid; "" = the whole variation) none of its on variations
    // has anything of (empty or excluded everywhere): nothing placed, a NoSound line (NoPartText).
    std::map<std::string, std::string> no_part;
};
// The hooks for ResolveEvent: Random -> a child index, Sound -> a variation index (-1 = as copied:
// no frames, or none on), through KeepOrDraw. `is_missing` is passed through. 13-23: a Sound draws
// among the variations that can play its part (ActiveVariationsFor), its history kept per node and
// part ("node|part"), and its draw records the part (SoundDraw::part).
ResolveHooks MakeDrawHooks(PopulateDrawState& state, EventDraw& ev, std::function<bool(const SoundNode&)> is_missing);
// 13-62i (D): an event a filtered Populate skips (one rule, a time, only the populated ones) still plays its
// recorded draws: they go into the history (key node, or node|part, as MakeDrawHooks), so the events drawn
// after it avoid repeating them. A loop pass's entry (PassDrawKey) goes to that pass's state in `passes`
// (made when missing; null = dropped).
void SeedHistory(PopulateDrawState& state, std::map<long long, PopulateDrawState>* passes,
                 const std::vector<SoundDraw>& draws);

// 13-62i (D review): which events a Populate draws. `rid` / `rids` / `has_t` + `t` / `only_populated` = its filter (all
// empty / false = every event).
struct PopulateFilter {
    std::string              rid;
    std::vector<std::string> rids;
    bool                     has_t = false;
    double                   t = 0.0;
    bool                     only_populated = false;
    // Resilience (Antho 2026-10-19, an assignment changed on a populated item): with only_populated, the item has
    // live populated sounds, so every event of the rules follows the new assignment (an event never placed or
    // emptied by a gap is placed too), except an event whose sounds were all deleted by hand (EventDeletedByHand).
    bool                     follow_item = false;
    bool Active() const { return !rid.empty() || !rids.empty() || has_t || only_populated; }
};
// What a Populate does with one event (time order): Draw it; Keep it (left out by the filter, or attached / spread
// by the user: it keeps the draws it plays -- they feed the history, SeedHistory, and are fixed in the timeline); Skip
// it (a disabled rule, or left out with nothing placed: no draw of its own).
enum class EventPlan { Draw, Keep, Skip };
EventPlan PlanPopulateEvent(const PopulateFilter& f, const std::string& rid, double t, const EventSounds* ev, bool enabled = true);
// The timeline of a Populate (SetDrawTimeline) from its events' plans and records (`evs[i]` null = no record):
// a Keep event fixed with its recorded draws, a Draw event drawn now (in the timeline when it has a record). Set only
// when some event is kept (else every event is drawn in time order: nothing to look ahead at). True when set.
bool SetPopulateTimeline(PopulateDrawState& state, std::map<long long, PopulateDrawState>* passes,
                         const std::vector<EventPlan>& plans, const std::vector<const EventSounds*>& evs);

// ---- P9: event slots -------------------------------------------------------------------------

struct DesiredSlot {
    int         pass = 0;
    std::string node;
    int         variation = -1;
    std::string part;    // 13-23: the part it plays (value uid, "" = the whole variation)
    int         pieces = 0;  // 13-20: the items it is placed as (one per span); 0 = not compared (a pile)
    double      sync = -1.0;  // 13-25: its anchor piece's snap (item s); < 0 = not compared (a pile, Takes)
    // 13-62i (D, P2): the dB each of its items plays at now, by piece index (gain_db + the piece's db; one entry
    // gain_db when it has no pieces: a pile, Takes, as copied). Not compared: a kept item's volume is rewritten
    // (KeptItemVolume).
    std::vector<double> piece_db;
    bool                pile = false;  // a folder pile (its members' volumes are relative to their source's)
};
// 13-62i (D, P2): DesiredSlot::piece_db of a sound resolved at `gain_db` placed as `pieces` (PieceApplyFor):
// gain_db + each piece's db, in piece order; {gain_db} when it has none (one item, or a pile's members).
std::vector<double> PieceDbFor(const std::vector<PieceApply>& pieces, double gain_db);
struct ExistingSlot {
    int         pass = -1;  // the pass nearest the placed item
    std::string node;
    int         variation = -1;
    Ownership   own = Ownership::Populated;
    std::string part;  // 13-23: its tag's part (PlacedTag::pt)
    double      sync = -1.0;  // 13-25: its tag's sy (the snap it was placed by); < 0 = unknown
};
enum class SlotAction { Keep, SetVariation, ReplaceNode, Remove, Place };
struct SlotStep {
    SlotAction       act = SlotAction::Keep;
    int              desired = -1;    // index into desired (not for Remove)
    std::vector<int> existing;        // indices into existing (Keep / SetVariation / ReplaceNode: the
                                      // slot's items, the first one is replaced; Remove: removed)
};
// Per pass: an existing Populated slot (its items grouped by node) with the same node keeps it
// (same variation, same part, as many items as the desired pieces when counted, the same anchor snap
// within 1 us when compared: a placement made before sync points, or under another one) or is placed
// again (SetVariation: its first item replaced, the others removed, the missing pieces added); the remaining desired slots take the remaining
// existing slots in order (other node); the rest of the existing are removed, the rest of the
// desired placed. A desired node that has a Deleted placement in the event is never placed again
// (no Place, no ReplaceNode into another slot), but its live copies are kept. Attached / Spread
// entries are ignored.
std::vector<SlotStep> PlanEventSlots(const std::vector<DesiredSlot>& desired, const std::vector<ExistingSlot>& existing);
// 13-56 review: a looped item's new passes filled (sound_follow): PlanEventSlots' Place steps only,
// the passes that have their sound left exactly as they are (no SetVariation, ReplaceNode or Remove).
std::vector<SlotStep> PlanPassFill(const std::vector<DesiredSlot>& desired, const std::vector<ExistingSlot>& existing);
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
// The Blend offsets (ms) summed from the node up to the root (what ResolveEvent adds on its path) for
// an event with `role` (13-31: "" = combined, every offset; else a role-using Blend's layer adds its
// offset only when layer_keep_offset).
double BlendOffsetMs(const SoundLibrary& lib, const std::string& node_uid, const std::string& role = std::string());
// True when the event has an Attached or Spread placement (Populate leaves it alone).
bool EventHasUserPlacement(const EventSounds& ev);
// True when the event has a live Populated placement.
bool EventIsPopulated(const EventSounds& ev);

// ---- Resilience 2026-10-19: resilience to timeline edits (Antho 2026-10-19) -----------------------------------------
//
// True when some event of the item has a live Populated placement (the item was populated).
bool ItemIsPopulated(const ItemSounds& item);
// True when the event's sounds were deleted by hand: a Deleted placement and no live Populated one.
bool EventDeletedByHand(const EventSounds& ev);
// The events of the item that hold a sound deleted by hand (a Deleted placement): what Restore puts back.
int CountDeletedByHand(const ItemSounds& item);
// Restore: the event's Deleted marks forgotten (Populate places those sounds again). Returns how many.
int ForgetDeletedMarks(EventSounds& ev);
// An event of the item now (rule id, clip time): the list Populate and the follow read (SoundEventsOf).
struct LiveEvent {
    std::string rid;
    double      t = 0.0;
};
// A RAV copy of this item in the project (tagged for it, placed by Populate) that its record does not list.
struct StrayCopy {
    std::string item_guid;
    std::string rid;
    double      t = 0.0;
    std::string node;
};
struct StrayAdoption {
    int                      adopted = 0;
    std::vector<std::string> remove_guids;  // copies of an event that no longer exists
};
// Re-run never doubles: each stray copy goes back into the record, as a Populated placement of the record event
// of its (rule, time) (made when missing) when that event is live (within kEventMatchSeconds), so Populate keeps,
// replaces or removes it instead of placing a second copy beside it; a copy of an event that no longer exists is
// removed (`remove_gone`; else left alone). A GUID the record already lists is ignored (any case).
StrayAdoption AdoptStrayCopies(ItemSounds& item, const std::vector<StrayCopy>& strays, const std::vector<LiveEvent>& live,
                               bool remove_gone = true);
// The record events no live event plays (the nearest record event of its rule within kEventMatchSeconds,
// FindEventSounds: an event moved by Detect, detected no more, or a second record event near the same one):
// their Populated copies are returned (to remove), their Populated / Deleted placements and draws dropped; the
// event goes unless it keeps an Attached / Spread placement (the follow's, 13-9).
std::vector<std::string> PruneStaleEvents(ItemSounds& item, const std::vector<LiveEvent>& live);

// ---- P15: the footer -------------------------------------------------------------------------

// The events Populate places: one per event whose rule has a library that is not blocked.
// `rule_ok` per block (index = ShownEvent::block); `event_blocks` = the blocks of the events.
int CountEvents(const std::vector<int>& event_blocks, const std::vector<char>& rule_ok);
// 13-23 (A5): the NoSound line of a Sound with nothing left for a part: "Nothing placed: <Sound> has
// no <Value> part left" ("has nothing left to place" for the whole variation).
std::string NoPartText(const std::string& sound_name, const std::string& value_name);

// ---- 13-59: the variation of each loop pass ----------------------------------------------------
//
// A looped animation item plays each event once per loop pass (13-56). Same: every pass places the
// event's draw (the 13-8 behaviour). New (the default, as Populate draws every event and an animation
// copy anew): the first pass (id 0) places the event's draw, every other pass draws its own with the
// same rules (Random / Sound, avoid repeat), its history kept per pass, its seed the item's mixed with
// the pass id. A pass's picks are stored in the event's draws under PassDrawKey (node uid + "#p" + id),
// so a re-Populate, the follow and a later fill keep them; Reshuffle and Repopulate (reset) forget them
// with the other draws not picked by hand. A pick by hand on the event (its plain node entry) is kept
// on every pass.
enum class LoopPassDraw { New, Same };
// The project's choice: the store header's `loopvar=same` token (none or another value = New).
LoopPassDraw LoopPassDrawOf(const SoundStore& store);
// Writes the token (New removes it), the header's other tokens untouched. False when unchanged.
bool SetLoopPassDraw(SoundStore& store, LoopPassDraw draw);
// "uid#p3": a draw entry of pass `pass` (never 0) for node `node_uid`.
std::string PassDrawKey(const std::string& node_uid, long long pass);
// True when `key` is a PassDrawKey; then its pass and node.
bool ParsePassDrawKey(const std::string& key, long long* pass, std::string* node_uid);
// The seed pass `pass` draws with (the item's seed for pass 0).
uint64_t PassDrawSeed(uint64_t seed, long long pass);
// What pass `pass` reads as its stored draws: the event's picks by hand first (they win), then that
// pass's own entries under their node uid.
std::vector<SoundDraw> PassPrevDraws(const std::vector<SoundDraw>& draws, long long pass);
// Replaces pass `pass`'s entries of `draws` by `out` (an EventDraw::out of that pass), never by hand.
void MergePassDraws(std::vector<SoundDraw>& draws, long long pass, const std::vector<SoundDraw>& out);
// 13-59 review: true when `draws` holds entries of pass `pass`.
bool HasPassDraws(const std::vector<SoundDraw>& draws, long long pass);
// 13-59 review: a sound placed on a pass (its node, variation, part), read back as that pass's draws.
struct PlacedPick {
    std::string node;
    int         variation = -1;
    std::string part;
};
// The draws (under their node uid, never by hand) that make a pass play `placed` again: each placed
// Sound's variation (none when -1, as copied) and, above it, each Random's child on its way. A fill that
// finds a pass already playing seeds that pass's entries from it, so a later Populate keeps it.
std::vector<SoundDraw> PassDrawsFromPlaced(const SoundLibrary& lib, const std::vector<PlacedPick>& placed);
// 13-59 review: drops the pass entries whose pass is not in `keep` (a pass cut off; every pass when
// `keep` is empty), so the record does not grow with passes that no longer play.
void PrunePassDraws(std::vector<SoundDraw>& draws, const std::set<long long>& keep);

// "Last populate: 24 events · 2 missing · 1 replaced · 1 rule gone" (events < 0: "Last change: ...").
std::string ReportSummary(const SoundReport& report, int events);

}  // namespace rav
