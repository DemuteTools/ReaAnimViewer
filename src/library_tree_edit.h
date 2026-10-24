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
#include <utility>
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
// 13-70: `copy` = a Copy / Paste (the originals stay): into itself, a descendant or next to itself
// is allowed (the copy is taken before it is placed); onto its own value still is not.
bool CanDrop(const SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows,
             const DropTarget& to, bool copy = false);
// What a move onto a value that already plays something does with that content.
// Default (Antho 2026-10-24, keep both): a Random / Blend there takes the moved rows (last); a Sound
// or a Switch there goes, with the moved rows after it, into a new Random named after the value.
// Swap (Alt held at the drop): one row of the same library swaps with what was there (the old
// content takes the moved row's place); several rows or another library: as Default.
enum class TakenValue { Default, Swap };
// Moves the rows (drag and drop, Cut / Paste). Onto an empty value: one row plays it, several
// rows become a new Random named after the value. Onto a taken value: see TakenValue. Across
// libraries the subtrees get new uids. `moved` gets the moved nodes' uids in the destination.
bool MoveRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
              std::vector<std::string>* moved, TakenValue taken = TakenValue::Default);
// The drag's bubble: what a move of `rows` dropped at `to` does (`alt` = Alt held), e.g. "Move
// before Step_02", "Move into Wood", "Move to Run", "Move to Wood: Y + X in a new Random", "Swap
// with Y". `swap_hint` = "Hold Alt: swap with Y" when a swap is possible and Alt is not held, else
// "". A refused drop: the dragged name(s) only (DragLabel).
struct DropHintText {
    std::string text;
    std::string swap_hint;
};
DropHintText DropHint(const SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows,
                      const DropTarget& to, bool alt);
// The dragged rows as the drag shows them: one node's name, else "N rows".
std::string DragLabel(const SoundLibrary& lib, const std::vector<LibraryRow>& rows);
// The name a slot is shown with: a value row's value name, else its node's name.
std::string SlotLabel(const SoundStore& store, const SoundLibrary& lib, const LibraryRow& row);
// "Sound", "Random", "Blend", "Switch".
const char* TypeWord(SoundNodeType t);
// 13-70: Copy / Paste. Places a deep copy of each row's subtree (CopySubtree, new uids: children,
// sounds, settings, Switch cases) at `to` with MoveRows' placing rules, except onto a taken value:
// the old content goes to that Switch's Unassigned (13-70); the originals stay. A Switch
// keeps its Switch Group uid: the groups are the store's, shared by every library, so the group
// always exists in the target library. Assignments do not follow (the copies' uids are new).
// `created` gets the copies' top uids.
bool CopyRows(SoundStore& store, const std::string& src_lib, const std::vector<LibraryRow>& rows, const DropTarget& to,
              std::vector<std::string>* created);

// Wraps each row's node in a new container taking its place (the root too).
bool WrapRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows, SoundNodeType type,
              const std::string& group_uid, std::vector<std::string>* created);
// Groups several rows into ONE new container at the first row's place.
// Unwrap (Antho 2026-10-13, the opposite of Wrap): a container (Random / Blend / Switch) holding exactly
// one node is removed and that node takes its place, the library's root included; the container's dB,
// Blend offset and Lands in carry onto it. Rows whose node is not such a container are skipped.
bool CanUnwrap(const SoundLibrary& lib, const std::string& uid);
bool UnwrapRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows);
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
// 13-68 v3: a Switch on a RAV-owned group keeps its default: MakeDefault leaves it, FollowGroupDefault and
// SetGroupDefault return false on it (IsRavGroup: the group is rav_owned).
bool IsRavGroup(const SoundStore& store, const std::string& group_uid);
bool MakeDefault(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows);
// 13-18 D4: the Switch's own default is cleared (it plays its group's default). False when none was set.
bool FollowGroupDefault(SoundStore& store, const std::string& lib_uid, const std::string& switch_uid);
// "Clear (plays the default)": the value's content goes to Unassigned.
bool ClearValue(SoundStore& store, const std::string& lib_uid, const LibraryRow& row);

// ---- 13-72: restructuring the tree ---------------------------------------------------------------
// "Wrap each in" is WrapRows with several rows (one new container around each, each in its place).
// Wrapping in a Switch: the wrapped node plays the group's default value (GroupDefaultValue) unless
// its name matches a value; Unassigned only when the group has no value (Antho 2026-10-19).
//
// "Wrap value contents in": the value rows whose content gets wrapped: a value row itself, or every
// value of a Switch row that plays something. Empty values are skipped.
std::vector<LibraryRow> ValueContentRows(const SoundLibrary& lib, const std::vector<LibraryRow>& rows);
// WrapRows on ValueContentRows(rows): inside each value, a new container around what it plays.
bool WrapValueContents(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                       SoundNodeType type, const std::string& group_uid, std::vector<std::string>* created);
// Every tree edit notes, on each node it puts in a Switch's Unassigned, where it was just before
// (SoundNode::was_path, and was_switch / was_value when it was a value's content); a node that
// leaves Unassigned forgets it. "Put back where it was": an Unassigned row goes back onto its
// former value of that Switch when both still exist (what the value plays now goes to Unassigned).
bool CanPutBack(const SoundStore& store, const SoundLibrary& lib, const std::string& uid, std::string* why);
bool PutBackRows(SoundStore& store, const std::string& lib_uid, const std::vector<LibraryRow>& rows,
                 std::vector<std::string>* placed);
// The store's assignments (Project: first "" ; a Character: its name) choosing the value an orphan
// was on, each with its assign key ("legs.step"); a target set to no library or to another library
// than lib_uid is left out. Rule / item / event values live on the animation items, not here.
std::vector<std::pair<std::string, std::string>> OrphanUsedBy(const SoundStore& store, const std::string& lib_uid,
                                                              const SoundNode& n);
// "Paste onto": the target node keeps its place, name, settings and assignments.
// Contents: its children are replaced by a copy of the source's (CopySubtree, new uids) and it takes
// the source's type (a Switch: its group, cases, default, Unassigned); two containers, neither inside
// the other. Settings: dB, Avoid repeat (Blend: linked variations; two Switches on one group: the
// default value); two containers or two Sounds. Any two libraries of the store.
enum class PasteOntoWhat { Contents, Settings };
bool CanPasteOnto(const SoundStore& store, const std::string& src_lib, const std::string& src_uid,
                  const std::string& dst_lib, const std::string& dst_uid, PasteOntoWhat what, std::string* why);
bool PasteOnto(SoundStore& store, const std::string& src_lib, const std::string& src_uid, const std::string& dst_lib,
               const std::string& dst_uid, PasteOntoWhat what);

// ---- Switch Groups (column 1) -------------------------------------------------------------------
// 13-68 v3: a RAV-owned group (SwitchGroup::rav_owned) is never renamed, deleted (but as an orphan), nor given / stripped of / renamed
// values by hand: RenameGroup, DeleteGroup, AddValue, RenameValue and DeleteValue (and a value row's RenameRow)
// return false on it. RAV's own code writes such a group directly.
// The store's groups as listed: the user's first, then RAV's (rav_owned), each in store order.
std::vector<const SwitchGroup*> GroupsInListOrder(const SoundStore& store);
bool AddGroup(SoundStore& store, const std::string& name, std::string* uid);
bool RenameGroup(SoundStore& store, const std::string& group_uid, const std::string& name);
// Every Switch on the group becomes a Random keeping all its children. 13-68 v3: a RAV-owned group only when no
// Switch of any library uses it (an orphan).
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
// ---- 13-77a: libraries linked to a .ravlib --------------------------------------------------------
// Linked = it names a .ravlib of the User Library folder (ravlib_path). An Attached library never is.
bool LibraryLinked(const SoundLibrary& lib);
// The common version of a linked library (null = none yet).
const LinkBase* FindLinkBase(const SoundStore& store, const std::string& lib_uid);
LinkBase*       FindLinkBase(SoundStore& store, const std::string& lib_uid);
// Drops the common versions of libraries that are gone or no longer linked. True when any went.
bool PruneLinkBases(SoundStore& store);
// Links the library to `file` with `base` as its common version (replacing any). False for an unknown or
// Attached library.
bool LinkLibraryToFile(SoundStore& store, const std::string& lib_uid, const std::string& file, const LinkBase& base);
// Unlinks it: ravlib_path and the common version cleared. False when it was not linked.
bool UnlinkLibraryFromFile(SoundStore& store, const std::string& lib_uid);
// Points a linked library at another file name (Locate...); the common version stays. False when not linked or
// already that name.
bool RelinkLibraryFile(SoundStore& store, const std::string& lib_uid, const std::string& file);
// Review 13-77a H1: links an unlinked, non-Attached library to a file that already holds it, with NO common version
// (13-77b then reads every difference as changed on both sides). False when it is not unlinked or `file` is empty.
bool LinkLibraryWithoutBase(SoundStore& store, const std::string& lib_uid, const std::string& file);
// Replaces (or sets) the common version of a linked library. False when it is not linked.
bool SetLinkBase(SoundStore& store, const LinkBase& base);
// Why a Sound is missing (spec missing-sounds-window, the one rule): a File Sound whose file is not
// found; an item Sound whose REAPER item is not in the project AND whose remembered file (its
// SoundSource::asset) is not found either, or was never remembered (an older Sound: file unknown).
// An item Sound whose item is gone but whose file is still on disk plays that file: not missing.
enum class MissingWhy { FileNotFound, ItemGoneFileKnown, ItemGoneFileUnknown };
struct MissingSound {
    std::string uid;
    std::string name;
    MissingWhy  why = MissingWhy::FileNotFound;
    std::string lib_uid;
    std::string path;  // the recorded path (asset.last_path), "" = unknown
    std::string file;  // the recorded file name (asset.name, else last_path's), "" = unknown
};
// The one rule for one Sound: `item_gone` = its item is not in the project (asked of item Sounds
// only), `file_missing` = its file (a File Sound's, an item Sound's remembered one) is not found.
// True when missing, `why` (optional) set.
bool SoundIsMissing(const SoundNode& s, const std::function<bool(const SoundNode&)>& item_gone,
                    const std::function<bool(const SoundNode&)>& file_missing, MissingWhy* why = nullptr);
// The library's missing Sounds, in its order (SoundIsMissing). The one list the red count, its
// tooltip and the Missing sounds window read.
std::vector<MissingSound> ListMissing(const SoundLibrary& lib, const std::function<bool(const SoundNode&)>& item_gone,
                                      const std::function<bool(const SoundNode&)>& file_missing);
// Each missing Sound with its reason (at most `max_lines`, then "and N more").
std::string MissingTip(const std::vector<MissingSound>& missing, size_t max_lines = 12);
// The Missing sounds window's status line of a still-missing Sound (the mockup's strings):
// "File not found · was <path>", "Item not in the project · file not found either: <file>",
// "Item not in the project · file unknown".
std::string MissingStatus(const MissingSound& m);
// A fixed row's status: "Fixed · <what it plays>", or found by its name only:
// "Found by name: check it · <path>".
std::string FixedStatus(const std::string& plays, bool by_name);
// A file whose stem (its name less the last extension) is `name`, ignoring ASCII case.
bool StemMatches(const std::string& path, const std::string& name);
// The first of `paths` (path order, ignoring ASCII case) whose stem is `name`; "" = none.
std::string FirstByStem(const std::vector<std::string>& paths, const std::string& name);
// Search a folder's toast: "Found N in <folder> (k by name: check them), M still missing. Ctrl+Z undoes."
// (each part only when it applies; nothing found: "Nothing found in <folder>, M still missing"). `ambiguous` (the sounds
// with several equally good candidates, never relinked): ", K with several candidates: Locate them one by one".
std::string SearchToastText(int found, int by_name, const std::string& folder, int still_missing, int ambiguous = 0);
// That ambiguous part alone ("" when none), for the other Locate toasts.
std::string AmbiguousText(int ambiguous);

}  // namespace rav
