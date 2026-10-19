// SPDX-License-Identifier: MIT
//
// The Attach manager (13-68e v5, pure; mockup ux-designs/epic-13-sound-library/maquette-attach-manager-13-68e-v5.html):
// what its window shows and what its gestures write, from a snapshot of the project the REAPER side reads
// (sound_spread_selection.h ReadAttachManager), and the linked instances a gesture made in REAPER reaches.
//
// The model (Antho 2026-10-19): one Attached library per marker kind holds *instances*: every sound Attach or Add to
// pile places (the original the user put on the marker included) is one. Per-animation and per-side content goes
// through RAV-owned Switches, never another library:
//   root   := AnimSwitch | Group
//   AnimSwitch := a Switch on the RAV group "<kind> animations": one case per animation (its value set at item level
//             on every item of that animation: the same name, linked copies included), its default value's case = what
//             the other animations play
//   Group  := SideSwitch (RAV - Side: L / R cases, each a Pile: L and R separated) | Pile (L and R together)
//   Pile   := the content: a Blend of layers (Add to pile), else one layer (an Attach: a Random of the piles)
// A gesture's reach, the "Apply to" switch (a project-level setting, default Anim): Item = this instance only (the
// library unchanged), Anim = every item of this animation (same name, linked copies included; its own case: copied from the shared content the
// first time), Project = every instance of the kind (every Group of the library).
// No REAPER: host-tested (tests/attach_manager_plan_test.cpp). The window: attach_manager_ui.h.

#pragma once

#include <functional>
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
    std::string           guid;
    std::string           name;            // its take's name
    bool                  selected = false;  // selected in REAPER
    std::vector<ValueSet> values;          // its item-level Switch values (the per-animation value)
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
    std::string            kind_name;  // its kind's side-free name ("FS", from its rule)
    char                   side = 0;
    std::vector<Placed>    placed;     // live only (deleted ones left out)
    std::vector<SoundDraw> draws;
};

// A selected item placed for an Attached library (an Add to pile reference) and its event.
struct StackRef {
    std::string owner;  // the animation item's GUID
    std::string rid;
    double      t = 0.0;
    int         event = -1;  // index into Snapshot::events (-1 = not found)
    std::string layer;  // "<library uid>|<layer uid>" of the instance ("" = not from the library: its marker alone)
    double      pos = 0.0;  // its event's project time
};
// Add to pile's reference among the selected instances: they must be one layer's linked instances (a plain click selects
// them all) or sit on one marker; the one whose event is nearest the new items (`anchor`, project s; the first when
// `has_anchor` is false). `distinct` gets how many layers / markers they span; -1 when more than one (or none).
int PileReference(const std::vector<StackRef>& refs, bool has_anchor, double anchor, int* distinct);

struct Snapshot {
    std::vector<Anim>          anims;
    std::vector<Marker>        markers;      // in the time selection, and the clicked ones
    bool                       has_time_sel = false;
    double                     ts0 = 0.0, ts1 = 0.0;
    std::vector<AttachedEvent> events;       // every event holding attached / placed sounds of an Attached library
    int                        sounds = 0;   // eligible selected sounds (Attach takes them all)
    int                        fresh = 0;    // eligible selected sounds that are no instance's (Add to pile's new items)
    int                        piles = 0;    // piles of the eligible selection
    double                     pile_offset_s = 0.0;  // Add to pile: the new items' top snap minus the stack's event
    bool                       has_pile_offset = false;
    double                     fresh_anchor = 0.0;    // Add to pile: the new items' top snap (project s)
    bool                       has_fresh_anchor = false;
    std::vector<StackRef>      stack_refs;   // selected instances of an Attached library
    std::set<std::string>      selected_items;  // GUIDs (upper case) of every selected item
    std::string                take_only;    // non-empty: Markers = Take only and nothing could be read (why)
};

// GUIDs compared without case.
std::string GuidKey(const std::string& g);
// A placed item's tag counts only when its owner's record lists the item (a live placement of that GUID): a duplicate
// or a pasted copy carries the original's tag under a new GUID and is a plain new item (never an instance).
bool TagListed(const ItemSounds& owner_rec, const std::string& guid);

// ---- Apply to (Item / Anim / Project) -----------------------------------------------------------------------

enum class Scope { Item, Anim, Project };
// The project's setting (SoundStore::attach_scope), default Anim. Readable with the window closed: the
// REAPER-side gestures (selection, delete, volume, fades) follow it too.
Scope ScopeOf(const SoundStore& store);
// False when it already was `s`.
bool SetScope(SoundStore& store, Scope s);
const char* ScopeLabel(Scope s);  // "Item" / "Anim" / "Project"
const char* ScopeWords(Scope s);  // "this instance" / "this animation" / "the project"
const char* ScopeTip(Scope s);    // what each gesture reaches (the switch's tooltip)

// An undo point's name is a delete of items (REAPER's "Remove items", "Delete items"..., or one of `localized`).
bool IsDeleteUndoName(const std::string& desc, const std::vector<std::string>& localized = {});

// ---- Attach / Add to pile: the window's choices ----------------------------------------------------------------

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

enum class Gesture { Attach, Pile };
// What the user asked of the two-button switch: nothing (follow the selection), Attach or Add to pile.
enum class GestureWant { Auto, Attach, Pile };

struct GestureChoice {
    Gesture     gesture = Gesture::Attach;
    bool        pile_ok = false;    // the selection holds instances and new items
    int         stack_events = 0;   // layers / markers the selected instances span (PileReference)
    int         stack_event = -1;   // the reference's event (index into Snapshot::events) when they span one
    int         marker = -1;        // Attach: the marker (index into s.markers), -1 = none
    std::string reason;             // non-empty: the run button is off, this says why
    std::string from;               // where the marker comes from (the Marker field's faint line)
};
// Attach when the selection holds no instance, Add to pile (on the reference's marker, PileReference) when it holds
// instances and new items, unless the user's switch says otherwise. The reasons: instances of several layers; Add to pile asked
// without an instance; no time selection and no label; nothing to attach.
GestureChoice ChooseGesture(const Snapshot& s, const std::vector<int>& choices, int marker, GestureWant want);

// The run button: "Attach · Anim", "Add to pile · Item".
std::string RunLabel(Gesture g, Scope s);
// The line under the gesture switch: `bold` = "2 items" / "1 new item", `rest` = " · onto FS L · 1.70 s, then every
// FS (L and R) of this animation" / " → a layer at +20 ms on every linked instance". `split` = the marker's Group is
// separated; `target` = "FS L · 1.70 s".
void GestureInfo(Gesture g, Scope s, const Snapshot& snap, const std::string& kind, char side, bool split,
                 const std::string& target, std::string* bold, std::string* rest);

// ---- The library's layout --------------------------------------------------------------------------------------

// The per-animation Switch's group: RAV-owned, named "<kind> animations" ("RAV - FS animations").
constexpr const char kAnimGroupSuffix[] = " animations";
bool IsAnimGroup(const SwitchGroup& g);

// Where a node sits: a case of the Switch `sw` (`value`), or the library's root (`sw` "").
struct Place {
    std::string sw;
    std::string value;
};

// One Group: the content every animation playing `value` of the AnimSwitch hears (the root's Group: `value` "").
struct Group {
    std::string value;          // its AnimSwitch value ("" = the root's Group, no AnimSwitch)
    bool        is_default = false;  // the AnimSwitch's default value (what the other animations play), or the root's
    Place       at;             // where its node sits
    std::string node;           // its node ("" = none: that value plays nothing)
    bool        split = false;  // a RAV - Side Switch: L and R separated
    std::string pile;           // not split: the Pile
    std::string pile_l, pile_r; // split: each side's Pile ("" = none)
};
struct Layout {
    std::string        anim_switch;  // "" = none (every animation plays the root's Group)
    std::string        anim_group;
    std::string        default_value;
    std::vector<Group> groups;       // the default (or the root's) first, then each animation's
};
Layout ReadLayout(const SoundStore& store, const SoundLibrary& lib);
// A Group plays something (an empty Blend, what a deleted last layer leaves, is no content).
bool HasContent(const Group& g);
// A library an earlier test version laid out (v3's "<kind> layers" per-animation Switch, v4's Side Switch inside the
// content or with a side missing): the v5 gestures refuse it (`why` says so; no migration).
bool LegacyLayout(const SoundStore& store, const SoundLibrary& lib, std::string* why);
// The Group an animation with these item-level values plays (its case, else the default). -1 = none.
int GroupOf(const Layout& lay, const std::vector<ValueSet>& item_values);
// The Pile of a Group for an event of `side`: the Group's own when it is not split, else that side's.
std::string PileOf(const Group& g, char side);
// The layers of a Pile: a Blend's children, else the Pile itself. Empty when "".
std::vector<std::string> PileLayers(const SoundLibrary& lib, const std::string& pile);
// Where a node plays from: its Group, the slot ('L' / 'R' in a split Group, 0 = both), its Pile and its layer.
struct LayerAt {
    int         group = -1;
    char        slot = 0;
    std::string pile;
    std::string layer;
};
LayerAt LayerOfNode(const SoundLibrary& lib, const Layout& lay, const std::string& node);
// The Sound an original (an item a Sound plays from, `source_guid`) plays as: the one with that source in the content an
// animation with `item_values` hears on `side` (its Group's Pile for that side). "" when that content has none (the
// caller then takes any). A copy (another animation's case, the other side after Separate) has the same source: never
// taken for it.
std::string PlayedSoundOf(const SoundStore& store, const SoundLibrary& lib, const Layout& lay, const std::vector<ValueSet>& item_values,
                          char side, const std::string& source_guid);
// A layer's identity across Groups (a copy has the same sources): its Sounds' source item GUIDs, sorted.
std::string LayerSig(const SoundLibrary& lib, const std::string& layer);

// ---- The library edits (one per gesture; the REAPER side applies the record changes they report) -------------

// The animation an Anim gesture acts for.
struct PoolKey {
    std::string                 anim_name;       // its take's name (its value's name)
    std::vector<ValueSet>       item_values;     // its item-level values
    std::set<std::string>       held_elsewhere;  // values held by items of another animation (never reused)
    std::string                 kind_name;       // "FS": the AnimSwitch's group is "FS animations"
};

struct EditOut {
    bool                               ok = false;
    std::string                        why;           // !ok: what went wrong
    ValueSet                           anim_value;    // Anim: the value to set on every item of the animation
    // Uids copied when an animation got its own case from the shared content: the old uid -> its copy's, for the
    // records and tags of that animation's events (both sides).
    std::map<std::string, std::string> remap_pool;
    // Separate: each split Group's R Pile is a copy: old uid -> the copy's, for the events of side R (any animation:
    // the old uids are only theirs).
    std::map<std::string, std::string> remap_side;
    std::set<char>                     slots;         // the slots written (0 = both sides, 'L' / 'R')
    char                               dropped_side = 0;  // Merge: the side whose sounds are replaced
    int                                groups = 0;    // Groups changed
};

// Attach (Anim / Project): `nodes` (SpreadLayerNodes, nodes[0] its root) become the Pile of the marker's slot ('L' / 'R'
// in a split Group, else both) in the animation's Group (Anim: made its own first) or in every Group (Project: each
// another copy; an AnimSwitch's default gets one too, so every animation plays it). What was there is replaced.
EditOut AttachContent(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                      std::vector<SoundNode> nodes, const std::function<std::string()>& new_uid);
// Add to pile (Anim / Project): `layer` joins the slot's Pile at `offset_ms` (a Pile that is not a Blend is wrapped in
// one first, the old content its first layer). A slot with no content is skipped (no layer alone); none at all: !ok.
// Every edit is refused (!ok, `why`) on a LegacyLayout library and leaves the store untouched when !ok.
EditOut AddPileLayer(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char side,
                     std::vector<SoundNode> layer, double offset_ms, const std::function<std::string()>& new_uid);
// Separate L and R (Anim / Project): a Group that is not split becomes a RAV - Side Switch, L its Pile (same uids),
// R a copy (remap_side). Nothing to separate: !ok.
EditOut SeparateSides(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool,
                      const std::function<std::string()>& new_uid);
// Merge L and R (Anim / Project): a split Group plays `keep`'s Pile on both sides again; the other side's goes.
EditOut MergeSides(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, char keep,
                   const std::function<std::string()>& new_uid);
// A layer deleted (Anim / Project): `layer` (a layer uid as the deleted instance played it) leaves the animation's
// Group (Anim: made its own first, the layer's copy removed) or, with every copy of it (the same LayerSig, not on
// the other side: `slot`), every Group (Project). Its Pile left with no layer goes too.
EditOut RemoveLayer(SoundStore& store, const std::string& lib_uid, Scope scope, const PoolKey& pool, const std::string& layer,
                    char slot, const std::function<std::string()>& new_uid);

// An event's records after an edit: its draws and placements named by the old uids name the copies.
bool RemapEventNodes(EventSounds& ev, const std::map<std::string, std::string>& uids);
// The same for a placed item's tag (its node and its Library prints). True when changed.
bool RemapTagNodes(PlacedTag& tag, const std::map<std::string, std::string>& uids);

// ---- The L and R card -------------------------------------------------------------------------------------------

struct LrCard {
    std::string text;    // "FS: L and R play the same sound."
    std::string pill;    // "Together" / "Separate" / "—"
    std::string button;  // "Separate L and R" / "Merge L and R (keep the L sound)"
    bool        merge = false;
    bool        enabled = false;
    std::string tip;     // why it is off ("" when on)
};
struct LrInput {
    bool        has_marker = false;  // a marker (or an instance) is chosen
    bool        marker_read = true;  // its animation item is read (an instance's marker may not be)
    std::string kind;
    char        side = 0;
    const Group* group = nullptr;     // its animation's Group (null = nothing attached)
    bool        any_together = false, any_split = false;  // over every Group with content (Project)
    std::string legacy;               // non-empty: LegacyLayout's why
};
// Anim: the marker's animation's Group. Project: Separate while any Group is together, else Merge while any is separate.
LrCard LrCardOf(const LrInput& in, Scope scope);

// ---- Linked instances (gestures made in REAPER) ---------------------------------------------------------------

// A placed item of an Attached library.
struct Instance {
    std::string guid;   // upper case
    std::string owner;  // its animation item's GUID (upper case)
    std::string pool;   // its animation's key: its name (every item of that animation shares it, pooled or not)
    std::string rid;
    double      t = 0.0;
    std::string lib;
    std::string layer;  // "" = not from the library (attached on one marker only): linked to nothing
    std::string sig;    // LayerSig
    char        slot = 0;
};
// The instances a gesture on `i` reaches (`i` first): Item = itself; Anim = the same layer in the same pool; Project =
// the same layer, or a copy of it (same sig) in another Group, not on the other side. An instance with no layer:
// itself.
std::vector<int> Linked(const std::vector<Instance>& all, int i, Scope scope);

// A click selected exactly one instance (`now`, not Ctrl): the items to select (its linked set, `now` first); empty
// = leave the selection alone (Item scope, Ctrl, several selected, not an instance, linked to nothing).
std::vector<std::string> ExpandSelection(const std::vector<Instance>& all, const std::set<std::string>& now, Scope scope, bool ctrl);

// Instances deleted by hand (`gone`, upper case GUIDs; the others still present): what their layers' removal takes.
struct LayerRemoval {
    std::string lib;
    std::string owner;  // one animation of the pool (its PoolKey is read from it)
    std::string layer;
    char        slot = 0;
};
struct DeletePlan {
    std::vector<std::string>  delete_guids;  // the linked instances still there
    std::vector<LayerRemoval> removals;      // one per (pool for Anim, library for Project) and layer
};
DeletePlan PlanPropagatedDelete(const std::vector<Instance>& before, const std::set<std::string>& gone, Scope scope);

// What a gesture on an item's look changes: its volume and fades (REAPER's D_VOL, D_FADEINLEN / D_FADEOUTLEN,
// C_FADEINSHAPE / C_FADEOUTSHAPE, D_FADEINDIR / D_FADEOUTDIR).
struct Look {
    double vol = 1.0;
    double fade_in = 0.0, fade_out = 0.0;
    int    in_shape = 0, out_shape = 0;
    double in_dir = 0.0, out_dir = 0.0;
};
bool SameLook(const Look& a, const Look& b);
struct LookWrite {
    std::string guid;
    Look        look;
};
// Instances whose look changed (`before` -> `now`, by GUID): their linked instances (unchanged ones) take the same
// change: the volume by the same ratio, a changed fade side as set. Each item written once (the first changed
// instance reaching it wins).
std::vector<LookWrite> PlanPropagatedLooks(const std::vector<Instance>& all, const std::map<std::string, Look>& before,
                                           const std::map<std::string, Look>& now, Scope scope);

// A drag of one instance moved its whole expanded selection (REAPER moves every selected item): the others go back
// (Antho 2026-10-19: moving an instance never propagates). `dragged` = the item under the mouse at the drop (else the
// expansion's anchor). Positions by GUID. Empty when it was not that (fewer than 2 moved, moved apart, not all ours).
struct MoveBack {
    std::string guid;
    double      pos = 0.0;
};
std::vector<MoveBack> PlanMoveBack(const std::set<std::string>& expanded, const std::string& dragged,
                                   const std::map<std::string, double>& before, const std::map<std::string, double>& now);

// ---- The watcher's decision (sound_spread_selection.cpp AttachWatchTick), from two reads and REAPER's state ------

struct WatchSnap {
    std::vector<Instance>         inst;
    std::map<std::string, Look>   looks;
    std::map<std::string, double> pos;        // the instances' positions
    std::set<std::string>         present;    // every item's GUID (upper case)
    std::map<std::string, double> owners;     // the RAV items' GUIDs (upper case) and positions
    std::set<std::string>         selected;
};
struct WatchIn {
    Scope                 scope = Scope::Anim;
    std::string           top;              // REAPER's last undo point ("" = none)
    bool                  redo = false;     // a redo waits: an undo just ran
    bool                  new_undo = false; // an undo point (or a delete action) since the last read
    std::vector<std::string> cut_names, delete_names;  // REAPER's localized names
    bool                  clicked = false;  // a mouse click (down and up) since the last read
    bool                  ctrl = false;     // Ctrl held when that button went down
    std::string           under_mouse;      // the item under the mouse at the drop (upper case)
    std::set<std::string> expanded;         // the linked selection RAV made last
    std::string           anchor;
};
struct WatchOut {
    std::set<std::string>    gone;      // a user delete: the instances to propagate
    std::vector<LookWrite>   looks;
    std::vector<MoveBack>    back;
    std::vector<std::string> select;    // a click: the linked selection to make
    bool                     clear_expanded = false;
};
// A gesture of the user's = a new undo point that is not RAV's, not a cut, no redo waiting. A delete (its name is one)
// propagates the instances it removed that were selected, their animation still there; else a volume / fade change
// propagates; a drag of one instance of the expanded selection (a click, no animation item moved) moves the others
// back; with nothing written, a click that selected one instance selects its linked instances (not with Ctrl).
WatchOut DecideWatch(const WatchSnap& last, const WatchSnap& now, const WatchIn& in);

}  // namespace am
}  // namespace rav
