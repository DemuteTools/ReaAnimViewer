// SPDX-License-Identifier: MIT
//
// Library tree edits (13-7a): the rows of column 2 and every gesture the tree, its toolbar,
// its menus and column 1 make on the sound store. Pure, no REAPER, host-tested
// (tests/library_tree_edit_test.cpp). The UI (library_tree_ui.cpp) wraps each call in one
// ModifySoundStore undo point.
//
// A row is a slot, as in the mockup: {parent, key}, key = the child node's uid, or a value uid
// under a Switch (the value row shows that value's content and expands into it). The root row
// is {"", root uid}. Every edit returns false when it changes nothing (the store is then left
// as it was, `why` says why when given).

#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

struct LibraryRow {
    std::string parent;  // "" = the root row
    std::string key;     // node uid, or value uid under a Switch
    bool operator==(const LibraryRow& o) const { return parent == o.parent && key == o.key; }
    bool operator!=(const LibraryRow& o) const { return !(*this == o); }
    bool operator<(const LibraryRow& o) const { return parent < o.parent || (parent == o.parent && key < o.key); }
};

enum class TreeRowKind { Node, Value, UnassignedHeader, Hint };

struct TreeRow {
    LibraryRow  row;
    TreeRowKind kind = TreeRowKind::Node;
    int         depth = 0;
    std::string node;         // the node shown (a value's content; "" = empty value / non-node row)
    std::string label;        // name shown (value rows: the value's name)
    bool        container = false;  // the shown node is a Random / Blend / Switch (has a chevron)
    bool        open = false;
    bool        is_default = false;  // a Switch's default value row
    bool        unassigned = false;  // a child in a Switch's Unassigned list
    int         count = 0;    // UnassignedHeader: how many
};

// Visible rows, depth first. `open` holds the uids of the open containers; the root is open
// unless "!root" is in the set. 13-17c: each level is listed A to Z (case-insensitive, stable:
// equal names keep the stored order; a Switch's value rows by value name, its "(deleted value)"
// rows after them, its Unassigned rows by name); the stored order is unchanged. A non-empty
// `search` keeps the Node / Value rows whose label (or a value row's content name) contains it,
// case-insensitive, with their ancestors, every container on the way shown open; hint rows go,
// an Unassigned header stays while one of its rows does. Nothing matches = no row.
std::vector<TreeRow> BuildTreeRows(const SoundStore& store, const SoundLibrary& lib, const std::set<std::string>& open,
                                   const std::string& search = {});

// The node a row targets ("" = none: an empty value or a non-node row).
std::string RowNode(const SoundLibrary& lib, const LibraryRow& row);
// True when the row is a Switch value slot.
bool RowIsValue(const SoundLibrary& lib, const LibraryRow& row);
// The row a node is shown on (its value row under a Switch case, else {parent, uid}).
LibraryRow RowOfNode(const SoundLibrary& lib, const std::string& uid);
// True when `uid` is `anc` or lies under it.
bool IsInSubtree(const SoundLibrary& lib, const std::string& uid, const std::string& anc);

// Shift-click: every Node / Value row between anchor and target at the anchor's depth. A target
// at another depth is replaced by its row at the anchor's depth (the nearest one going back to
// the anchor).
std::vector<LibraryRow> RangeSelectByLevel(const std::vector<TreeRow>& rows, const LibraryRow& anchor,
                                           const LibraryRow& target);

// A node to add. Sound: `sound` is its data.
struct NewNodeSpec {
    SoundNodeType type = SoundNodeType::Random;
    std::string   name;       // "" = the type's name ("New Sound" for a Sound)
    std::string   group_uid;  // Switch
    SoundData     sound;
    double        volume_db = 0.0;  // Sound: its Volume (an imported item's own volume, Antho 2026-10-13)
};

// Whether a "+ <type>" can act on a row (empty row = no selection: the root), and why not.
bool CanAddAt(const SoundStore& store, const SoundLibrary& lib, const LibraryRow* row, SoundNodeType type,
              std::string* why);
// Adds the nodes at a row (DECISIONS section 3): inside a Random / Blend, next to a sound in its
// Random / Blend, or as a Switch value's content (a container wraps what was there; a Sound on a
// taken value is refused; several Sounds on an empty value become a Random holding them).
// `created` gets the uids of the new nodes.
bool AddNodes(SoundStore& store, const std::string& lib_uid, const LibraryRow* row, const std::vector<NewNodeSpec>& specs,
              std::vector<std::string>* created, std::string* why);

// Removes the rows' nodes and their subtrees (a value row: its content). The root is refused.
bool RemoveRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows);

enum class DropMode { Into, Before, After, OntoValue };
struct DropTarget {
    std::string lib;   // the library dropped into
    LibraryRow  row;   // Into: a container's row; Before / After: a sibling row; OntoValue: a value row
    DropMode    mode = DropMode::Into;
};
// Whether rows of `src_lib` may drop there (never a container into itself or a descendant).
bool CanDrop(const SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows,
             const DropTarget& to);
// Moves the rows (drag and drop, Cut / Paste). Onto a value: one row swaps with what was there
// (the old content takes the moved row's place; from another library or several rows: it goes
// to Unassigned); several rows become a new Random there. Across libraries the subtrees get
// new uids. `moved` gets the moved nodes' uids in the destination.
bool MoveRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
              std::vector<std::string>* moved);

// Wraps each row's node in a new container taking its place (the root too).
bool WrapRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
              const std::string& group_uid, std::vector<std::string>* created);
// Groups several rows into ONE new container at the first row's place.
bool GroupRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
               const std::string& group_uid, std::string* created);
// Duplicates each row's subtree (new uids): a value's content goes to that Switch's Unassigned,
// anything else right after the original. The root is refused.
bool DuplicateRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                   std::vector<std::string>* created);
bool RenameRow(SoundStore& store, const std::string& lib_uid, const LibraryRow& row, const std::string& name);
// The container a row's type change acts on: the row's own container; a Switch value row playing a
// Sound or nothing acts on its Switch (a select-all over Switch rows also holds their value rows).
// "" = none (a Sound row, the Unassigned / add-value / hint rows).
std::string RowTypeNode(const SoundLibrary& lib, const LibraryRow& row);
// Changes the rows' containers' type (ConvertNodeType, RowTypeNode); Sounds are skipped.
bool SetRowsType(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
                 const std::string& group_uid);
// A value row becomes its Switch's default (one per Switch).
bool MakeDefault(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows);
// 13-18 D4: the Switch's own default is cleared (it plays its group's default). False when none was set.
bool FollowGroupDefault(SoundStore& store, const std::string& lib_uid, const std::string& switch_uid);
// "Clear (plays the default)": the value's content goes to Unassigned.
bool ClearValue(SoundStore& store, const std::string& lib_uid, const LibraryRow& row);

// ---- Switch Groups (column 1) -------------------------------------------------------------------
bool AddGroup(SoundStore& store, const std::string& name, std::string* uid);
bool RenameGroup(SoundStore& store, const std::string& group_uid, const std::string& name);
// Every Switch on the group becomes a Random keeping all its children.
bool DeleteGroup(SoundStore& store, const std::string& group_uid);
bool AddValue(SoundStore& store, const std::string& group_uid, const std::string& name, std::string* uid);
bool RenameValue(SoundStore& store, const std::string& group_uid, const std::string& value_uid, const std::string& name);
// Decision 2 (Antho 2026-10-12): every Switch on the group, in every library: the value's
// content goes to Unassigned; a default naming it becomes "" (it follows the group) when the
// group's default (GroupDefaultValue, after the delete) has a child there, else the first
// remaining value (group order) that has a child, else ""; a preview naming it becomes "".
// 13-18 D5: the group's own default naming it is cleared (the first value plays).
bool DeleteValue(SoundStore& store, const std::string& group_uid, const std::string& value_uid);
// 13-18: the Switches (every library) on the group whose own default is a value of the group other
// than `value_uid` (they would not follow a new group default `value_uid`).
int CountDefaultOverrides(const SoundStore& store, const std::string& group_uid, const std::string& value_uid);
// 13-18: the group's default becomes `value_uid` (a value of the group); `follow` also clears every
// own default of the Switches on the group (they follow it). False when nothing changed.
bool SetGroupDefault(SoundStore& store, const std::string& group_uid, const std::string& value_uid, bool follow);
// "Name", "Name 2", ... not used by `taken` (names compared with SwitchNameKey).
std::string UniqueName(const std::string& base, const std::vector<std::string>& taken);

// ---- Libraries ----------------------------------------------------------------------------------
// "Library N" (first free), colour 0, root = an empty Random named after it.
bool NewLibrary(SoundStore& store, std::string* uid);
bool RenameLibrary(SoundStore& store, const std::string& lib_uid, const std::string& name);
bool DeleteLibrary(SoundStore& store, const std::string& lib_uid);
// How many Sounds of the library are missing.
int CountMissing(const SoundLibrary& lib, const std::function<bool(const SoundNode&)>& is_missing);

}  // namespace rav
