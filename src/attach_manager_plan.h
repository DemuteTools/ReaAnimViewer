// SPDX-License-Identifier: MIT
//
// The Attach manager (13-68e, pure): what its window shows and what its gestures write in the records, from a
// snapshot of the project the REAPER side reads (sound_spread_selection.h ReadAttachManager). The Attach tab: the
// Marker list (one entry per marker kind and side of REAPER's time selection, each at its first marker, plus the
// clicked RAV marker label) and its preselection, the gesture (Attach / Add to stack) chosen from the item selection
// and why the run button is off. The Detach tab: what the list shows (a selected animation, else the selected items,
// else the time selection, else everything), one row per marker holding attached / placed sounds grouped by
// animation, or one row per variation of the Attached libraries with the markers it is placed on; the record edits of
// Detach (the marker then stays empty: its sounds marked deleted, as a sound deleted by hand) and of Remove from
// library (the Random draws after the removed child moved down one, so every other marker keeps its sound). No
// REAPER: host-tested (tests/attach_manager_plan_test.cpp). The window: attach_manager_ui.h.

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {
namespace am {

// ---- The snapshot ----------------------------------------------------------------------------------------------

// A RAV animation item.
struct Anim {
    std::string guid;
    std::string name;            // its take's name
    bool        selected = false;  // selected in REAPER
};

// A RAV marker of an animation item (a committed or preview project marker, or a detected event).
struct Marker {
    int         anim = -1;      // index into Snapshot::anims
    std::string name;           // as its rule writes it ("FS L"; a preview's "FS L - Preview"): the gesture reads it
    std::string label;          // "FS L"
    std::string kind_key;       // SpreadKind::key (both sides)
    std::string kind_name;      // SpreadKind::name ("FS")
    char        side = 0;       // 'L' / 'R' (EventSide of its rule), 0 = none
    double      pos = 0.0;      // project s
    bool        clicked = false;  // its label is selected in REAPER
};

// One sound placed on an event (its record placement and the item it names).
struct Placed {
    std::string item_guid;
    std::string name;            // the item's take name (gone items are left out of the snapshot)
    std::string node;            // the record's node, else the library Sound whose source it is ("" = unknown)
    Ownership   own = Ownership::Populated;
    double      offset_s = 0.0;
};

// An event of an Attached library's rule that holds live sounds (populated or attached).
struct AttachedEvent {
    int                    anim = -1;
    std::string            rid;
    double                 t = 0.0;    // clip s (the event's identity with rid)
    double                 pos = 0.0;  // project s
    std::string            lib_uid;
    std::string            label;      // the marker's type ("FS L")
    char                   side = 0;
    std::vector<Placed>    placed;     // live only (deleted ones left out)
    std::vector<SoundDraw> draws;
};

// A selected item placed for an Attached library (an Add to stack reference) and its event.
struct StackRef {
    std::string owner;  // the animation item's GUID
    std::string rid;
    double      t = 0.0;
    int         event = -1;  // index into Snapshot::events (-1 = not found)
};

struct Snapshot {
    std::vector<Anim>          anims;
    std::vector<Marker>        markers;      // in the time selection, and the clicked ones
    bool                       has_time_sel = false;
    double                     ts0 = 0.0, ts1 = 0.0;
    std::vector<AttachedEvent> events;       // every event holding attached / placed sounds of an Attached library
    int                        sounds = 0;   // eligible selected sounds (Attach takes them all)
    int                        fresh = 0;    // eligible selected sounds that are no stack's (Add to stack's new items)
    int                        piles = 0;    // piles of the eligible selection
    std::vector<StackRef>      stack_refs;   // selected items of an Attached library's stack
    std::set<std::string>      selected_items;  // GUIDs (upper case) of every selected item
    std::string                take_only;    // non-empty: Markers = Take only and nothing could be read (why)
};

// GUIDs compared without case.
std::string GuidKey(const std::string& g);

// ---- Attach tab ------------------------------------------------------------------------------------------------

// A marker's identity across refreshes: its animation's GUID, name and position (ms).
std::string MarkerKey(const Snapshot& s, const Marker& m);

// The Marker list: one entry per kind and side among the markers of the time selection (each at its first in time),
// plus every clicked marker not already listed; in time order. Indices into s.markers.
std::vector<int> MarkerChoices(const Snapshot& s);
// The entry preselected: the one of `pick_key` (the user's pick) when listed, else the clicked one, else the first in
// time. -1 when the list is empty.
int PreselectMarker(const Snapshot& s, const std::vector<int>& choices, const std::string& pick_key);
// "FS L · 1.70 s".
std::string MarkerText(const Marker& m);

enum class Gesture { Attach, Stack };
// What the user asked of the two-button switch: nothing (follow the selection), Attach or Add to stack.
enum class GestureWant { Auto, Attach, Stack };

struct GestureChoice {
    Gesture     gesture = Gesture::Attach;
    bool        stack_ok = false;   // the selection holds stack items of one marker and new items
    int         stack_events = 0;   // distinct events of the selected stack items
    int         stack_event = -1;   // that event (index into Snapshot::events) when one
    int         marker = -1;        // Attach: the marker (index into s.markers), -1 = none
    std::string reason;             // non-empty: the run button is off, this says why
    std::string from;               // where the marker comes from (the Marker field's faint line)
};
// Attach when the selection holds no stack item, Add to stack (on that stack's marker) when it holds stack items and
// new items, unless the user's switch says otherwise. The reasons: stack items of several markers; Add to stack
// asked without a stack; no time selection and no label; nothing to attach.
GestureChoice ChooseGesture(const Snapshot& s, const std::vector<int>& choices, int marker, GestureWant want);

// The run button's label: "Attach · this animation · both sides", "Add to stack · all with FS · L only".
std::string RunLabel(Gesture g, bool all, bool side_only, const std::string& kind, char side);

// ---- Detach tab: what the list shows -----------------------------------------------------------------------

enum class FilterKind { Animations, Items, TimeSelection, Everything };
struct ListFilter {
    FilterKind            kind = FilterKind::Everything;
    std::set<std::string> anims;   // FilterKind::Animations: their GUIDs (upper case)
    std::set<std::string> items;   // FilterKind::Items: the selected items' GUIDs (upper case)
    double                t0 = 0.0, t1 = 0.0;
    std::string           label;   // "the selected animation (Walk_01)" ... "everything"
};
// First that applies: selected animation items -> what is attached to them; else selected items -> only them (their
// markers); else a time selection -> what it covers; else everything.
ListFilter MakeFilter(const Snapshot& s);
bool FilterKeeps(const Snapshot& s, const ListFilter& f, const AttachedEvent& e);

// One row of the This marker list.
struct MarkerRow {
    int         event = -1;   // index into Snapshot::events
    std::string l1;           // "Step_dirt_A + 1 layer"
    std::string l2;           // "FS L · 1.70 s · attached original"
};
// An animation's group of rows.
struct AnimGroup {
    int                    anim = -1;
    std::string            name;
    std::vector<MarkerRow> rows;
};
// The events of `lib_uid` ("" = every Attached library) the filter keeps, grouped by animation (in the order of their
// first event in time), each group's rows in time order.
std::vector<AnimGroup> MarkerRows(const Snapshot& s, const ListFilter& f, const std::string& lib_uid);

// ---- Detach tab: Whole library -----------------------------------------------------------------------------

enum class VariationKind { Variation, Takes, Layer };
struct VariationRow {
    std::string   lib_uid;
    std::string   lib_name;
    std::string   node;        // the variation's node (a pile, a Takes Sound, a layer)
    std::string   name;
    VariationKind kind = VariationKind::Variation;
    int           takes = 0;   // a Takes Sound's takes
    std::string   where;       // the Switch value it plays under ("L", "Walk_01"; "" = everywhere)
    std::string   random;      // its parent Random ("" = none)
    int           index = -1;  // its index among that Random's children
    int           markers = 0; // events it is placed on
};
// The variations of an Attached library: each child of a Random (a pile, a Takes Sound) and each layer (a child of a
// Blend after its base, a per-animation or per-side layer), with the markers it is placed on (EventUsesVariation).
std::vector<VariationRow> VariationRows(const SoundStore& store, const SoundLibrary& lib, const Snapshot& s);
// The Sound nodes under `uid` (itself included).
std::set<std::string> SoundsUnder(const SoundLibrary& lib, const std::string& uid);
// The event plays the variation: one of its sounds is placed from a Sound under it, or (under a Random) its draw of
// that Random is the variation's index -- the same test as EventUsedRemoved.
bool EventUsesVariation(const SoundLibrary& lib, const VariationRow& v, const AttachedEvent& e);
// The items placed from the variation (what its select button selects): on each event that plays it, the sounds
// from a Sound under it, and under a Random the event's own sound (offset 0) whose Sound is not known.
std::vector<std::string> VariationItems(const SoundLibrary& lib, const VariationRow& v, const Snapshot& s);
// "2 takes · L · on 3 markers".
std::string VariationText(const VariationRow& v);

// ---- The record edits ----------------------------------------------------------------------------------------

// Detach from this marker: every live sound of `ev` leaves it (marked deleted, as a sound deleted by hand), and every
// Sound of `lib` is marked deleted there too, so Populate places nothing on it again. Returns the GUIDs of the items
// that left (each once). An event with no live sound is left unchanged (empty result).
std::vector<std::string> DetachEventInRecord(EventSounds& ev, const SoundLibrary& lib);

// What Remove from library took out.
struct RemovedVariation {
    std::string           node;
    std::string           random;   // its parent Random ("" = none)
    int                   index = -1;
    std::set<std::string> sounds;    // the Sounds under it
    std::set<std::string> siblings;  // the Sounds under its parent Random (what a redraw there could place)
    std::set<std::string> removed;   // every node removed
};
// Takes `node` out of the library (RemoveSubtree). False when absent.
bool RemoveVariationFromLibrary(SoundLibrary& lib, const std::string& node, RemovedVariation* out);
// Whether an event placed the removed variation (before the removal): its draw of the Random is the index, or one of
// its sounds is from a Sound under it. `node_of` = item GUID (upper case) -> the library Sound whose source it is.
bool EventUsedRemoved(const EventSounds& ev, const RemovedVariation& r, const std::map<std::string, std::string>& node_of);
// The record of one animation item after Remove from library (`rids` = its rules holding the library): on each event
// that played it, its items leave (Keep: marked deleted with every Sound its Random could draw instead, so the
// marker gets nothing else; Delete: dropped, so Populate draws it again from what is left); on every event, a draw of
// the Random after the removed index moves down
// one (the same child plays). Both drop the event's draws of the Random and of the removed nodes. Returns the GUIDs of
// the items that left; `events` (optional) counts the events.
std::vector<std::string> RemoveVariationInRecord(ItemSounds& rec, const std::set<std::string>& rids, const RemovedVariation& r,
                                                 bool keep, const std::map<std::string, std::string>& node_of,
                                                 int* events = nullptr);

}  // namespace am
}  // namespace rav
