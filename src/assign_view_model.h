// SPDX-License-Identifier: MIT
//
// Epic 13 (13-15b, pure, no REAPER, no ImGui) -- what the Sound assignment window (tagging_assign_ui.h) and
// the Tagging strip's summary lines show, decided here so it is host-tested: the item's targets (its bone
// rules, paired by side, and its auto detection types, each with the blocks they hold), the state of one
// target read from the 13-15a resolver (its library, its Switch values, where each comes from, whether its
// Switch walk reaches a group), the summary text of one or several targets, the mixed-selection rules, the
// "Plays" / ladder / track texts, and the tree's selection rules (click, Shift range, Ctrl add, same kind).
// The mockup is ux-designs/epic-13-sound-library/maquette-assignation-sons.html.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bone_events.h"
#include "sound_assign.h"
#include "sound_model.h"
#include "sound_switch.h"

namespace rav {
namespace assign {

// ---- The item's targets -------------------------------------------------------------------------------

// The key of the "Item values" row; a leaf's key is "b<block index>" (stable while the item's blocks are).
constexpr const char kItemKey[] = "item";
std::string LeafKey(int block);
bool        BlockOfKey(const std::string& key, int* block);

struct Leaf {
    std::string key;
    std::string name;  // the marker: "FS L"
    int         block = -1;
    std::string rid;   // "" = the rule has no id yet (a gesture gives it one)
};
// A parent row: a bone rule (both sides of a marker pair "Scuff L" / "Scuff R" are one), or an auto type.
struct Parent {
    std::string       id;    // "r:b3" / "a:step"
    std::string       name;  // "Scuff", "Step"
    bool              is_auto = false;
    std::vector<Leaf> leaves;
};
struct Targets {
    std::vector<Parent> rules;
    std::vector<Parent> autos;
    std::string         signature;  // changes when the blocks (their number or markers) do: a selection of keys is then stale
    const Parent*       ParentOf(const std::string& leaf_key) const;
    const Leaf*         FindLeaf(const std::string& leaf_key) const;
};
Targets BuildTargets(const std::vector<Block>& blocks);
// "Scuff" for the markers "Scuff L" / "Scuff R" (the stem), "" for any other marker.
std::string PairStem(const std::string& marker, char* side);

// ---- One target's state -------------------------------------------------------------------------------

enum class SegTone { Normal, Inherited, Mixed, Faint, Warn };
struct Seg {
    std::string text;
    SegTone     tone = SegTone::Normal;
};

// One Switch Group of a target's library (in the library's walk order).
struct GroupState {
    std::string group_uid;
    std::string group_name;     // "" when the group is gone
    bool        deleted = false;
    std::string value_uid;      // the effective value ("" = none)
    std::string value_name;
    std::string from;           // "this rule", "item", "Project \xC2\xB7 Feet", "default"
    bool        own = false;    // set at the target's own layer (the rule's, or the item's for the Item row)
    std::string own_uid;        // the own layer's value ("" = inherit)
    std::string inherit_name;   // what "inherit" gives: the value below the own layer
    std::string inherit_from;
    bool        reached = true;
    std::string not_reached;    // "not reached under Crouch"
    const SoundNode* first_switch = nullptr;
};

struct Target {
    std::string      key;
    std::string      title;      // "Scuff \xC2\xB7 FS L", "Item values"
    std::string      part, type;
    bool             is_item = false;
    EffectiveAssign  ea;         // the rule (or none) through its layers
    EffectiveAssign  inherited;  // the same with no rule of its own (what "inherit" gives)
    const SoundLibrary* lib = nullptr;
    bool             lib_empty = false;    // no Sound in it: nothing to place yet
    bool             lib_blocked = false;  // Populate reports Blocked (a Switch lost its group...)
    std::string      blocked_reason;
    LibState         own_state = LibState::Inherit;  // the rule's own library state
    std::string      own_library;
    std::vector<GroupState> groups;
    bool Unset() const { return ea.library.empty() && !ea.none && ea.level == AssignLevel::Unset; }
    bool OwnLib() const { return ea.level == AssignLevel::Rule; }
    bool Plays() const { return lib && !lib_empty && !lib_blocked; }
    const GroupState* Group(const std::string& group_uid) const;
};

// The target of a block of the item (`rec` its sound record, `ch` its character or null).
Target MakeTarget(const SoundStore& store, const ItemSounds& rec, const SoundCharacter* ch, const Block& b,
                  const std::string& key, const std::string& title);
// The "Item values" row: the item's own Switch values over the Switch defaults. `others` are the item's
// targets (the Switch Groups it shows are the ones their libraries use).
Target MakeItemTarget(const SoundStore& store, const ItemSounds& rec, const std::vector<Target>& others);

// ---- Summary text -------------------------------------------------------------------------------------

struct Summary {
    bool             has_dot = false;
    uint32_t         dot_color = 0;  // the library's colour (Block::color form)
    std::vector<Seg> segs;           // library name, " \xC2\xB7 ", values joined by " \xE2\x80\xBA "
    bool             blocked = false;  // a target's library is empty or blocked: the red "!"
    std::string      Plain() const;
};
// Several targets at once: "mixed libraries" when their libraries differ, "(per side)" for a group whose
// value differs, "nothing set", "No sound", " \xC2\xB7 empty library", "! group deleted". `with_lib` puts the
// library (dot + name) first.
Summary Summarize(const SoundStore& store, const std::vector<const Target*>& targets, bool with_lib);
// The Item values row: the values it sets, or "defaults".
Summary SummarizeItem(const SoundStore& store, const Target& item);

// ---- Mixed selections ---------------------------------------------------------------------------------

struct Mix {
    bool lib = false;        // the effective libraries differ
    int  own_settings = 1;   // distinct own library settings (inherit / None / a library)
    bool groups = false;     // same library, but a Switch value (or where it is set) differs
};
Mix MixOf(const std::vector<const Target*>& targets);
// The own library setting: "" inherit, kLibNoneWord, else the library uid.
std::string OwnLibraryArg(const Target& t);

// ---- Details text -------------------------------------------------------------------------------------

// "Scuff \xC2\xB7 both sides" (the whole parent), "3 targets", the leaf's title for one, "Item values".
std::string SelectionTitle(const Targets& tg, const std::vector<const Target*>& sel);
// "this item + 2 pool copies \xC2\xB7 character Hero", "every rule of this item, unless the rule sets its own".
std::string ScopeLine(bool item_row, bool any_bone_rule, const std::string& pool_text, const std::string& character);
// One "Plays" line: the path text and the targets (titles) that play it.
struct PlaysLine {
    std::string              text;
    std::vector<std::string> titles;
};
std::vector<PlaysLine> PlaysLines(const std::vector<const Target*>& targets);
std::string            PlaysText(const Target& t);  // "FS Hero \xE2\x80\xBA Run \xE2\x80\xBA Boots", "No sound", "X \xE2\x80\xBA blocked"
// "Run: item \xC2\xB7 Boots: Hero \xC2\xB7 Feet" -- the source of each value a single target plays.
std::string WhyText(const Target& t);
// "Run-Boots-Dirt": the Switch path joined by '-' (the Flat track layout's name).
std::string TrackNameOf(const Target& t);
// How many events of these rules set their own value of the group.
int EventOverrideCount(const ItemSounds& rec, const std::vector<std::string>& rids, const std::string& group_uid);

// One level of "Where the library comes from", general first.
struct LadderRow {
    std::string label;     // "Project \xC2\xB7 Feet", "this rule"
    bool        set = false;
    bool        none = false;
    std::string library;   // uid when set and not none
    bool        kit = false;
    bool        hit = false;  // the level that gives the library: "\xE2\x86\x90 plays"
};
struct Ladder {
    std::vector<std::string> kits;  // library names whose kit serves the part
    std::string              part_label;
    std::vector<LadderRow>   rows;
};
Ladder BuildLadder(const SoundStore& store, const SoundCharacter* ch, const Target& t);

// ---- Overview (Item scope) ------------------------------------------------------------------------------

// The groups the item's libraries use, once each, in the store's group order.
std::vector<std::string> OverviewGroups(const SoundStore& store, const std::vector<Target>& targets);
enum class CellKind { NotUsed, NotReached, Value };
struct Cell {
    CellKind    kind = CellKind::NotUsed;
    std::string text;
    bool        inherited = false;
};
Cell OverviewCell(const Target& t, const std::string& group_uid);

// ---- Last change line -----------------------------------------------------------------------------------

struct ChangeText {
    std::string prefix;  // "Undo point "
    std::string bold;    // "RAV: Set rule library"
    std::string rest;    // " \xC2\xB7 Hero_Run_01 \xC2\xB7 Library = FS Hero \xC2\xB7 2 targets"
};
ChangeText LastChange(const std::string& undo_name, const std::string& where, const std::string& what, int targets);

// ---- Selection (tree rows) --------------------------------------------------------------------------------

struct Selection {
    std::vector<std::string> keys;
    std::string              anchor;  // Shift extends from it
    std::string              cursor;  // the last row clicked / moved to
    bool Has(const std::string& key) const;
    void Clear();
};
// "item" and "rule" leaves are two kinds and are never mixed in one selection.
enum class RowKind { Item, Rule };
RowKind KindOf(const std::string& key);
// A click on a leaf / the Item row. `visible` = the keys of the rows shown, in order. Plain: only this one.
// Shift: the visible rows of the same kind between the anchor and this one. Ctrl: add or remove (same kind only).
void ClickRow(Selection& s, const std::string& key, bool shift, bool ctrl, const std::vector<std::string>& visible);
// A click on a parent row (or its box): its leaves become the selection; with `toggle` (the box, Ctrl or
// Shift) they are added, or all removed when they were all selected.
void ClickParent(Selection& s, const std::vector<std::string>& leaf_keys, bool toggle);
// 0 none of the leaves selected, 1 all, 2 some.
int ParentState(const Selection& s, const std::vector<std::string>& leaf_keys);
// The selection without the keys `valid` does not hold (rows that went away).
void Prune(Selection& s, const std::vector<std::string>& valid);

// ---- Filter -----------------------------------------------------------------------------------------------

// ASCII case-insensitive "contains"; an empty filter matches.
bool FilterMatch(const std::string& filter, const std::string& text);

}  // namespace assign
}  // namespace rav
