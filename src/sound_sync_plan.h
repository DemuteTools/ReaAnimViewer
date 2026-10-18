// SPDX-License-Identifier: MIT
//
// 13-35 (pure, no REAPER): Sync placed sounds. Each placed item's tag records what the Library looked like when RAV
// placed it (a print per node of its path, PlacedTag::lib) and what RAV wrote on it (PlacedTag::wr). A node whose print
// now differs is behind: the Library shows it (the Placed sounds card, the node's ↻ mark) and Sync pushes it onto the
// placed sounds through Populate's engine (sound_populate.h SyncPlacedSounds), keeping the draw. A placed item that no
// longer holds what RAV wrote (volume, fades, length) or was moved (its nudge) is edited by hand: Sync asks, or follows
// the project's rule (HandEditRuleOf).

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sound_model.h"
#include "sound_populate_plan.h"  // SlotAction, EventPlan

namespace rav {

using NodePrint = PlacedTag::NodePrint;
using PlacedWrite = PlacedTag::Written;

// ---- Prints ----------------------------------------------------------------------------------

// The print of one node: its volume, its Blend offset and, for a Sound, hashes of the user settings that shape what it
// places for variation `variation` (fades, sync point, zones, level) and whether that variation is on. Measured data
// (sync points, trims, automatic snaps) is left out: measuring it again never makes a sound behind.
NodePrint PrintOf(const SoundNode& n, int variation);
// The prints of the node's path in `lib`: the node first, the library's root last. Empty when it is not in `lib`.
std::vector<NodePrint> PathPrints(const SoundLibrary& lib, const std::string& node_uid, int variation);
// The library holding node `uid` (nullptr when none).
const SoundLibrary* LibraryOfNode(const SoundStore& store, const std::string& uid);
// The nodes of the tag's path whose print now differs, node first. None for a tag without prints, a node gone or a
// path that changed (a structure change: Populate's).
std::vector<std::string> BehindNodes(const PlacedTag& tag, const SoundStore& store);
// The stored print of `node` in `tag` (nullptr when none).
const NodePrint* TagPrint(const PlacedTag& tag, const std::string& node);
// One node's change: "Volume −3 → −6 dB", "Blend offset 12 → 18 ms", "fades", "sync", "zones",
// "level", "variation 3 excluded" (`variation` = the tag's, 0-based), joined with ", ". "" when equal.
std::string DescribeChange(const NodePrint& was, const NodePrint& now, int variation);
// True when the change switches off the variation the sound plays (Sync draws it again).
bool ChangeRedraws(const NodePrint& was, const NodePrint& now);
// A re-Populate that kept a sound applied the Library's volume to it (KeptItemVolume): each stored print whose volume
// differs takes the current one, whatever else differs (that stays behind). True when one changed.
bool AdoptVolumes(std::vector<NodePrint>& stored, const std::vector<NodePrint>& now);
// The tag of an item a re-Populate kept and set from `vol_was` to `vol_new` (the Library's volume): its prints adopt the
// volume (AdoptVolumes against `now`, its path's prints now) and what RAV wrote follows the same ratio, so a hand edit
// of its volume stays one and none appears.
void KeptTagAfterVolume(PlacedTag& tag, const std::vector<NodePrint>& now, double vol_was, double vol_new);

// ---- What a Sync does in Populate's engine (sound_populate.cpp PopulateOne, PopOpts::sync) ------

// A slot step of a Sync: Place / Remove / ReplaceNode and a step touching no sound behind in scope are skipped (Sync
// keeps the draw, nothing else moves); Keep is updated in place; SetVariation (a variation switched off, zones re-split,
// a sync point moved) is placed again.
enum class SyncStep { Skip, KeepInPlace, Replace };
SyncStep SyncStepAction(SlotAction act, bool touches_scope);
// An event of a Sync: a Draw event with no sound behind in scope is kept as it is (Keep with a record: its draws feed the
// history; Skip without one); any other plan is left.
EventPlan SyncEventPlan(EventPlan plan, bool has_ev, bool any_behind_in_scope);
// "Run › Stone": the names down to the node, its library's root left out, a Switch case's content read as its
// value's name and the other Switches left out.
std::string SyncNodePath(const SoundStore& store, const SoundLibrary& lib, const std::string& uid);

// ---- Hand edits ------------------------------------------------------------------------------

struct HandEdit {
    bool        vol = false, fades = false, length = false, moved = false;
    double      vol_db = 0.0;   // the user's change of its volume (dB)
    double      nudge_s = 0.0;  // its nudge
    std::string text;           // "volume −4 dB, moved +12 ms, fade-out 40 ms, length"
    bool Any() const { return vol || fades || length || moved; }
};
// What the user changed on a placed item: `wr` = what RAV wrote, `now` = the item now, `nudge_s` = its nudge (the
// item record's offset). Tolerances: 0.05 dB, 0.5 ms, curvature 0.001.
HandEdit HandEdits(const PlacedWrite& wr, const PlacedWrite& now, double nudge_s);
// A sound of several items: `b` merged into `a` (its text rebuilt).
void MergeHandEdit(HandEdit& a, const HandEdit& b);

// The project's rule for hand-edited placed sounds on Sync (the store header's `syncedit=keep|over` token; none = Ask).
enum class HandEditRule { Ask, Keep, Overwrite };
HandEditRule HandEditRuleOf(const SoundStore& store);
// Writes the token (Ask removes it), the header's other tokens untouched. False when unchanged.
bool SetHandEditRule(SoundStore& store, HandEditRule rule);
const char* HandEditRuleText(HandEditRule rule);  // "Ask each time", "Keep my edits", "Overwrite them"

// What a Sync does to one placed item, given the user's choice: `overwrite` false = Keep (the hand-edited properties stay
// the user's: its volume relative, its fades, length and nudge as they are), true = the Library's values, nudge 0.
// `want_db` / `g` = the dB it plays at now / was placed at (its tag's g).
struct SyncItemPlan {
    double vol = 1.0;            // its new D_VOL
    double wr_vol = 1.0;         // what RAV now writes (its tag's wr.vol)
    bool   set_fades = true;     // write the Library's fades
    bool   set_length = true;    // write the Library's length
    double nudge_s = 0.0;        // the nudge it is placed with
};
SyncItemPlan PlanSyncItem(const PlacedWrite& wr, const PlacedWrite& now, const HandEdit& hand, bool overwrite, double want_db,
                          double g, double nudge_s);

// ---- The Library's view of it ----------------------------------------------------------------

// One placed item as the scan reads it (REAPER side: sound_sync.cpp).
struct SyncPlacedItem {
    std::string guid;
    std::string owner;   // the animation item's GUID
    std::string name;    // its take's name (the sound's)
    PlacedTag   tag;
    PlacedWrite now;     // what the item holds now
    double      nudge_s = 0.0;
    double      anchor = 0.0;  // where its pile is anchored (its pieces share it)
};
// An animation item that holds placed sounds.
struct SyncOwner {
    std::string guid, name, track;
    bool        selected = false;  // selected in REAPER
};
// One placed sound behind (its items: a placement's pieces).
struct SyncSound {
    std::string              owner, lib, node;
    std::vector<std::string> behind;  // its behind nodes, node first
    std::vector<std::string> items;   // its placed items' GUIDs
    std::string              name;
    double                   t = 0.0;  // its event's clip time
    HandEdit                 hand;
    bool                     redraw = false;  // its variation is off now
};
struct SyncState {
    std::vector<SyncSound>             sounds;
    std::vector<SyncOwner>             owners;  // those holding a sound behind, in the order given
    std::map<std::string, std::string> what;    // behind node -> its change (the first sound's)
    std::set<std::string>              redraws; // behind nodes whose change redraws
    std::set<std::string>              libs_placed;  // the libraries some stamped placed sound plays (the card shows)
};
// The behind sounds of `items` (grouped by owner, event, node, part and anchor), with what changed on each node.
SyncState BuildSyncState(const SoundStore& store, const std::vector<SyncPlacedItem>& items, const std::vector<SyncOwner>& owners);

// A sound in the scope of library `lib` and node `node` ("" = every behind node of the library).
bool SyncInScope(const SyncSound& s, const std::string& lib, const std::string& node);
// The card of library `lib`: its behind nodes, sounds and the items holding them.
struct SyncCard {
    int changes = 0, sounds = 0, items = 0;
};
SyncCard SyncCardOf(const SyncState& st, const std::string& lib);
// "1 change not yet on 26 placed sounds in 3 items."
std::string SyncCardText(const SyncCard& c);
// The ↻ marks: behind node -> its behind sounds, for library `lib`.
std::map<std::string, int> SyncMarksOf(const SyncState& st, const std::string& lib);
// The where-dialog's lines (one per animation item holding a sound in scope, grouped by track in the owners' order).
struct SyncItemLine {
    std::string              owner, name, track;
    bool                     selected = false;
    int                      sounds = 0, hand = 0;
    std::vector<std::string> nodes;  // its behind nodes in scope
};
std::vector<SyncItemLine> SyncItemLines(const SyncState& st, const std::string& lib, const std::string& node);
// The hand-edited sounds of the chosen items in scope.
std::vector<const SyncSound*> SyncHandSounds(const SyncState& st, const std::string& lib, const std::string& node,
                                             const std::set<std::string>& owners);
// "Apply to N items" only for a strict non-empty subset.
bool SyncApplyCheckedEnabled(size_t checked, size_t all);
// "Don't ask again" only when every choice is the same (`overwrite` per sound).
bool SyncRememberEnabled(const std::vector<char>& overwrite);

// What a Sync did (per sound).
struct SyncCounts {
    int adjusted = 0, redrawn = 0, kept = 0, overwritten = 0, skipped = 0;
};
// "One undo point “RAV: Sync placed sounds”: 26 adjusted, 3 redrawn, 1 hand edit kept, 2 overwritten".
std::string SyncToastText(const SyncCounts& c, bool remembered);
// "26 placed sounds" ("1 placed sound").
std::string SyncPlural(int n, const char* word);
// A dB value: "+2 dB", "−4 dB", "0 dB" (one decimal when needed).
std::string SyncDbText(double db);

}  // namespace rav
